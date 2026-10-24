#!/usr/bin/env python3
"""Disposable loop-mounted ext4 ENOSPC/read-only tests, plus hardened systemd.
No installed database or default Vigil unit is modified.
"""
import argparse
from contextlib import contextmanager
import errno
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
from resilience_integration import Hook, create, daemon, inventory, wait_local
from monitor_integration import bus, send, source
from simulator.protocol import reading


def run(args, **kwargs):
    r = subprocess.run(args, text=True, capture_output=True, timeout=30, **kwargs)
    assert r.returncode == 0, (args, r.stdout, r.stderr)
    return r


@contextmanager
def filesystem():
    with tempfile.TemporaryDirectory(prefix="vigil-stage1-ext4-") as tmp:
        root = Path(tmp)
        image, mount = root / "disk.img", root / "mount"
        mount.mkdir(mode=0o700)
        run(["truncate", "-s", "2560M", str(image)])
        run(["mkfs.ext4", "-q", "-F", "-m", "0", str(image)])
        run(["sudo", "mount", "-o", "loop,nodev,nosuid", str(image), str(mount)])
        try:
            run(["sudo", "chown", f"{os.getuid()}:{os.getgid()}", str(mount)])
            mount.chmod(0o700)
            yield mount
        finally:
            run(["sudo", "mount", "-o", "remount,rw", str(mount)])
            run(["sudo", "umount", str(mount)])


def physical(build, output, readonly):
    name = "read-only" if readonly else "ENOSPC"
    print(f"EXPECT: actual ext4 {name}, monitoring keeps high/stale alarms; no failed local acknowledgement; exact existing inventory recovers", flush=True)
    with filesystem() as mount:
        path = create(mount / "state", build / "vigil-outbox-init")
        hook = Hook("after_take_before_begin")
        try:
            with daemon(build / "vigild-resilience-test", path, hook, pipesize=1024 * 1024) as d, bus() as raw:
                initial = wait_local(d, lambda s: s["pending_batches"] >= 1)
                send(raw, reading(1, 0, 2800, 0, 0))
                token = hook.token()
                if readonly:
                    run(["sudo", "mount", "-o", "remount,ro", str(mount)])
                else:
                    with (mount / "fill").open("wb", buffering=0) as f:
                        # Reserve real ext4 blocks quickly, avoiding gigabytes
                        # of filler writeback delaying the transcript reader.
                        # The final writes exhaust the actual remaining space.
                        space = os.statvfs(mount)
                        reserved = max(0, space.f_bavail * space.f_frsize - 8 * 1024**2)
                        os.posix_fallocate(f.fileno(), 0, reserved)
                        f.seek(reserved)
                        chunk = b"x" * (1024 * 1024)
                        try:
                            while True:
                                f.write(chunk)
                                d.pump(0)
                        except OSError as e:
                            assert e.errno == errno.ENOSPC
                    assert os.statvfs(mount).f_bavail < 256
                hook.release()
                failed = wait_local(d, lambda s: s["local_state"] in ("full", "readonly", "recovering"))
                assert failed["local_committed"] == initial["local_committed"]
                stale = d.wait(lambda s: s["valid_readings"] == 1 and source(s, 1, 0)["freshness"] == "stale")
                assert source(stale, 1, 0)["high"] == 1
                start = time.monotonic()
                d.stop()
                elapsed = time.monotonic() - start
                assert elapsed < 1.25, elapsed
            if readonly:
                run(["sudo", "mount", "-o", "remount,rw", str(mount)])
            else:
                (mount / "fill").unlink()
            final = next(e for e in reversed(d.events) if e["event"] == "outbox")
            inv = inventory(path, output / name)
            # An ENOSPC attempt publishes none (asserted above). A later
            # checkpoint can reuse already allocated DB/WAL pages, so another
            # whole transaction may legitimately commit before shutdown.
            assert inv["meta"]["pending_records"] == final["local_committed"]
            assert inv["meta"]["pending_records"] >= initial["durable_pending"]
            assert inv["meta"]["admitted_readings"] <= 1
            with daemon(build / "vigild", path) as restarted:
                wait_local(restarted, lambda s: s["durable_pending"] >= initial["durable_pending"])
                restarted.stop()
            return dict(token=token, local_state=failed["local_state"], sqlite_error=failed["sqlite_error"], shutdown_s=elapsed, initial=initial, failed=failed, final=final)
        finally:
            hook.close()


def readonly_startup(build, output):
    print("EXPECT: actual read-only ext4 refuses startup/READY and preserves committed bytes; writable remount allows recovery", flush=True)
    with filesystem() as mount:
        path = create(mount / "state", build / "vigil-outbox-init")
        with daemon(build / "vigild", path) as d, bus() as raw:
            wait_local(d, lambda s: s["pending_batches"] >= 1)
            for n in range(4):
                send(raw, reading(1, 0, 2200, 0, n))
            wait_local(d, lambda s: s["local_readings"] == 4)
            d.stop()
        before = inventory(path, output / "readonly-before")
        run(["sudo", "mount", "-o", "remount,ro", str(mount)])
        rejected = subprocess.run([str(build / "vigild"), "--outbox-path", str(path)], capture_output=True, timeout=5)
        assert rejected.returncode == 2 and b"ready " not in rejected.stderr
        run(["sudo", "mount", "-o", "remount,rw", str(mount)])
        after = inventory(path, output / "readonly-after")
        assert before["rows"] == after["rows"]
        with daemon(build / "vigild", path) as d:
            wait_local(d, lambda s: s["durable_pending"] >= before["meta"]["pending_records"])
            d.stop()
        return dict(rejected_exit=2, preserved_records=before["meta"]["pending_records"])


def systemd(build, output):
    print("EXPECT: temporary hardened unprivileged unit READY/watchdog and SIGKILL/watchdog restart; StateDirectory retains exact committed identities", flush=True)
    suffix = f"vigil-stage1-{os.getpid()}"
    unit = f"{suffix}.service"
    state = Path("/var/lib") / suffix
    # PrivateTmp hides /tmp and this VM mounts /run noexec. The temporary
    # executable directory is removed after the test; no installed unit changes.
    runtime = Path("/opt") / suffix
    try:
        run(["sudo", "install", "-d", "-m", "0755", str(runtime)])
        run(["sudo", "install", "-m", "0755", str(build / "vigild"), str(runtime / "vigild")])
        run(["sudo", "install", "-d", "-o", str(os.getuid()), "-g", str(os.getgid()), "-m", "0700", str(state)])
        path = state / "outbox.sqlite3"
        run([str(build / "vigil-outbox-init"), str(path)])
        props = ["User=" + str(os.getuid()), "Group=" + str(os.getgid()), "StateDirectory=" + suffix,
                 "StateDirectoryMode=0700", "ProtectSystem=strict", "ProtectHome=yes", "PrivateTmp=yes",
                 "PrivateDevices=yes", "NoNewPrivileges=yes", "CapabilityBoundingSet=", "AmbientCapabilities=",
                 "UMask=0077", "WatchdogSec=4s", "Restart=on-failure", "RestartSec=1s", "TimeoutStartSec=30s",
                 "TimeoutStopSec=3s", "NotifyAccess=main", "RestrictAddressFamilies=AF_CAN AF_UNIX AF_INET AF_INET6"]
        run(["sudo", "systemd-run", "--setenv=VIGIL_DB_HOST=/tmp/vigil-test-no-postgres", "--unit", suffix, "--service-type=notify", *[f"--property={p}" for p in props],
             str(runtime / "vigild"), "--outbox-path", str(path)])
        def property_(key):
            return run(["systemctl", "show", unit, "--property=" + key, "--value"]).stdout.strip()
        pid = int(property_("MainPID"))
        assert pid > 0 and property_("ActiveState") == "active"
        status = Path(f"/proc/{pid}/status").read_text()
        assert f"Uid:\t{os.getuid()}\t" in status and "CapEff:\t0000000000000000" in status
        with bus() as raw:
            for n in range(8):
                send(raw, reading(1, 0, 2200, 0, n))
        # Check the actual journal's local commit status before killing, then
        # freeze process to obtain a consistent file copy without online SQL.
        end = time.monotonic() + 5
        while time.monotonic() < end:
            journal = run(["journalctl", "-u", unit, "--no-pager", "-o", "cat"]).stdout
            if "local_readings=8 " in journal:
                break
            time.sleep(0.05)
        else:
            raise AssertionError(journal)
        run(["sudo", "systemctl", "kill", "--kill-who=main", "--signal=SIGSTOP", unit])
        before = inventory(path, output / "systemd-before")
        run(["sudo", "systemctl", "kill", "--kill-who=main", "--signal=SIGKILL", unit])
        end = time.monotonic() + 7
        newpid = pid
        while time.monotonic() < end:
            newpid = int(property_("MainPID"))
            if newpid > 0 and newpid != pid and property_("ActiveState") == "active":
                break
            time.sleep(0.05)
        assert newpid > 0 and newpid != pid
        run(["sudo", "systemctl", "kill", "--kill-who=main", "--signal=SIGSTOP", unit])
        end = time.monotonic() + 10
        watchdog_pid = newpid
        while time.monotonic() < end:
            watchdog_pid = int(property_("MainPID"))
            if watchdog_pid > 0 and watchdog_pid != newpid and property_("ActiveState") == "active":
                break
            time.sleep(0.05)
        assert watchdog_pid > 0 and watchdog_pid != newpid
        assert int(property_("NRestarts")) >= 2
        assert "watchdog" in run(["journalctl", "-u", unit, "--no-pager", "-o", "cat"]).stdout.lower()
        run(["sudo", "systemctl", "stop", unit])
        after = inventory(path, output / "systemd-after")
        rows = {r["order_id"]: r["payload"] for r in after["rows"]}
        assert all(rows[r["order_id"]] == r["payload"] for r in before["rows"])
        (output / "systemd-journal.txt").write_text(run(["journalctl", "-u", unit, "--no-pager", "-o", "cat"]).stdout)
        return dict(uid=os.getuid(), old_pid=pid, new_pid=newpid, watchdog_pid=watchdog_pid, preserved_records=before["meta"]["pending_records"])
    finally:
        subprocess.run(["sudo", "systemctl", "stop", unit], capture_output=True)
        subprocess.run(["sudo", "systemctl", "reset-failed", unit], capture_output=True)
        run(["sudo", "rm", "-rf", "--", str(runtime), str(state)])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", type=Path, required=True)
    ap.add_argument("--output", type=Path, required=True)
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    native = Path(tempfile.mkdtemp(prefix="vigil-stage1-physical-evidence-"))
    result = dict(native_evidence=str(native))
    result["full"] = physical(args.build.resolve(), native, False)
    result["readonly"] = readonly_startup(args.build.resolve(), native)
    result["systemd"] = systemd(args.build.resolve(), native)
    (args.output / "result.json").write_text(json.dumps(result, indent=2))
    print(f"PASS real ext4 ENOSPC/read-only/systemd: {native}", flush=True)


if __name__ == "__main__":
    main()
