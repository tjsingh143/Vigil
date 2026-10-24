#!/usr/bin/env python3
"""Stage 1 software crash/fault gate: real Linux SocketCAN and native SQLite WAL.
Only disposable state. Hook tokens select exact transaction boundaries.
No online SQL opener: inventories use stopped DB/WAL copies and pinned SQLite.
"""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from monitor_integration import Daemon, bus, config_text, send, source
from simulator.protocol import reading, heartbeat

SQLITE = "/opt/vigil-sqlite-3.51.3/bin/sqlite3"


def sql(path, query):
    p = subprocess.run([SQLITE, "-readonly", "-json", str(path), query], check=True,
                       capture_output=True, text=True, timeout=30)
    return json.loads(p.stdout or "[]")


def inventory(path, output):
    output.mkdir(parents=True)
    sizes = {}
    for suffix in ("", "-wal", "-shm"):
        src = Path(str(path) + suffix)
        if src.exists():
            sizes[suffix or "db"] = src.stat().st_size
            shutil.copy2(src, output / src.name)
    copy = output / path.name
    assert sql(copy, "PRAGMA quick_check") == [{"quick_check": "ok"}]
    assert not sql(copy, "PRAGMA foreign_key_check")
    meta = sql(copy, "SELECT * FROM spool_meta")[0]
    rows = sql(copy, "SELECT * FROM outbox_batches ORDER BY order_id")
    runs = sql(copy, "SELECT * FROM origin_runs ORDER BY run_no")
    assert meta["pending_records"] == sum(r["record_count"] for r in rows)
    assert meta["pending_payload_bytes"] == sum(len(r["payload"].encode()) for r in rows)
    assert meta["pending_batches"] == len(rows)
    assert meta["admitted_records"] == meta["pending_records"] + meta["retired_records"]
    assert meta["retired_readings"] <= meta["retired_records"]
    assert [r["order_id"] for r in rows] == list(range(meta["last_retired_order"]+1, meta["next_order_id"]))
    assert sizes["db"] <= 512 * 1024**2
    assert sizes.get("-wal", 0) < 520 * 1024**2
    assert sizes.get("-shm", 0) <= 2 * 1024**2
    for r in rows:
        payload = json.loads(r["payload"])
        assert r["payload_bytes"] == len(r["payload"].encode())
        assert payload["format"] == 2 and payload["order_id"] == r["order_id"]
        assert payload["run_id"] == r["run_id"] and payload["batch_id"] == r["batch_id"]
        origin = next(o for o in runs if o["run_id"] == r["run_id"])
        assert all(payload["origin"][k] == origin[k] for k in
                   ("run_id", "run_no", "run_token", "boot_id", "started_ms", "started_utc_ms"))
        assert payload["origin"]["spool_id"] == meta["spool_id"]
        records = payload["records"]
        assert len(records) == r["record_count"]
        assert [e["seq"] for e in records] == list(range(r["first_seq"] or 0,
                                                        (r["first_seq"] or 0) + len(records)))
    data = dict(meta=meta, rows=rows, runs=runs, sizes=sizes)
    (output / "inventory.json").write_text(json.dumps(data, indent=2))
    return data


def wait_local(d, predicate, timeout=6):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        d.pump(0.02)
        row = next((e for e in reversed(d.events) if e["event"] == "outbox"), {})
        if row and predicate(row):
            return row
    raise AssertionError(("outbox status deadline", d.events[-5:]))


class Hook:
    def __init__(self, name, hit=2, repeat=1):
        self.nr, self.nw = os.pipe()
        self.rr, self.rw = os.pipe()
        self.name = name
        self.next_hit = hit
        self.env = dict(os.environ, VIGIL_DB_HOST="/tmp/vigil-test-no-postgres", VIGIL_TEST_HOOK=name, VIGIL_TEST_HIT=str(hit), VIGIL_TEST_REPEAT=str(repeat),
                        VIGIL_TEST_NOTIFY_FD=str(self.nw), VIGIL_TEST_RELEASE_FD=str(self.rr))
        # Daemon helper uses close_fds=True; use a dedicated subprocess wrapper
        # below to preserve precisely these two test FDs.
    def token(self, timeout=6):
        assert select.select([self.nr], [], [], timeout)[0], f"no hook {self.name}"
        token = os.read(self.nr, 80).rstrip(b"\0").decode()
        assert token == f"{self.name}:{self.next_hit}", token
        self.next_hit += 1
        return token
    def release(self, command=b"x"):
        os.write(self.rw, command)
    def close(self):
        for fd in (self.nr, self.nw, self.rr, self.rw):
            os.close(fd)


@contextmanager
def daemon(exe, path, hook=None, env=None, capacity=16, pipesize=-1, config=None):
    # Inherit test FDs only for the separate test binary. Reuse all existing
    # monitoring snapshot/accounting oracles without weakening their assertions.
    original = subprocess.Popen
    def popen(*args, **kwargs):
        if hook:
            kwargs["pass_fds"] = (hook.nw, hook.rr)
        return original(*args, **kwargs)
    subprocess.Popen = popen
    d = None
    try:
        d = Daemon(str(exe), config or config_text(pods=(1,), timeout=500, status=100, rate=0),
                   persistence=True, extra_args=("--outbox-path", str(path), "--queue-capacity", str(capacity)),
                   env=hook.env if hook else (env or dict(os.environ, VIGIL_DB_HOST="/tmp/vigil-test-no-postgres")), pipesize=pipesize)
        yield d
    finally:
        subprocess.Popen = original
        if d:
            d.close()


def create(root, init):
    root.mkdir(mode=0o700, parents=True)
    path = root / "outbox.sqlite3"
    subprocess.run([str(init), str(path)], check=True, timeout=10)
    return path


def committed_records(inv):
    return {(r["run_id"], e["seq"]): e for r in inv["rows"] for e in json.loads(r["payload"])["records"]}


def crash_cut(exe, init, root, output, name, repeat):
    print(f"EXPECT: {name}: precommit candidate absent; postcommit exact identity/payload survives {repeat} SIGKILL/restarts", flush=True)
    path = create(root, init)
    previous = {}
    previous_batches = {}
    tokens = []
    for i in range(repeat):
        hook = Hook(name, hit=1, repeat=2)
        try:
            with daemon(exe, path, hook) as d, bus() as raw:
                # Pin the startup candidate first. Queue all N inputs and
                # observe them in main before allowing the selected N-record
                # admission to begin; scheduling cannot split this batch.
                first_token = hook.token()
                for n in range(8):
                    send(raw, reading(1, 0, 2200, 0, n))
                d.wait(lambda s: s["valid_readings"] == 8)
                hook.release()
                tokens.extend((first_token, hook.token()))
                # At this hook only the storage thread is paused. Monitor must
                # continue accepting frames and expire genuine Linux timers.
                latest = d.wait(lambda s: s["valid_readings"] == 8 and source(s, 1, 0)["freshness"] == "stale")
                assert source(latest, 1, 0)["current"] == 0
                d.child.kill()
                d.child.wait(timeout=3)
                inv = inventory(path, output / f"kill-{i}")
                current = committed_records(inv)
                assert all(current.get(k) == v for k, v in previous.items())
                batches = {r["order_id"]: r["payload"] for r in inv["rows"]}
                assert all(batches.get(k) == v for k, v in previous_batches.items())
                previous_batches = batches
                readings = [e for e in current.values() if e["kind"] == "reading"]
                if name == "after_local_commit_before_publish":
                    assert len(readings) == 8 * (i + 1), len(readings)
                    assert inv["sizes"].get("-wal", 0) > 32
                else:
                    assert len(readings) == 0, readings
                previous = current
        finally:
            hook.close()
    with daemon(exe, path) as d:
        wait_local(d, lambda s: s["durable_pending"] >= len(previous))
        # Recovered history never initializes current measurements.
        snap = d.wait(lambda s: s["frames"] == 0)
        assert source(snap, 1, 0)["has_value"] == 0
        d.stop()
    final = inventory(path, output / "restarted")
    assert all(committed_records(final).get(k) == v for k, v in previous.items())
    assert all(next(r["payload"] for r in final["rows"] if r["order_id"] == k) == v for k, v in previous_batches.items())
    return dict(kills=repeat, tokens=tokens, records=len(previous), sizes=final["sizes"])


def startup_crashes(exe, init, root, output, repeat):
    print(f"EXPECT: {repeat} kills after origin commit: new run each time, bounded empty origins, no invented history", flush=True)
    path = create(root, init)
    origins = set()
    for i in range(repeat):
        hook = Hook("after_startup_origin_commit", hit=1)
        try:
            child = subprocess.Popen([str(exe), "--outbox-path", str(path)], env=hook.env,
                                     pass_fds=(hook.nw, hook.rr), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                hook.token()
                child.kill()
                child.communicate(timeout=3)
            finally:
                if child.poll() is None:
                    child.kill()
                    child.communicate(timeout=3)
            inv = inventory(path, output / str(i))
            assert inv["rows"] == [] and len(inv["runs"]) == 1
            run = inv["runs"][0]
            assert run["run_id"] not in origins and run["run_no"] == i + 1
            origins.add(run["run_id"])
            assert inv["meta"]["next_run_no"] == i + 2
        finally:
            hook.close()
    return dict(kills=repeat, unique_origins=len(origins))


def capture_race(exe, init, root, output):
    print("EXPECT: copied capture excludes later evictions/revisions; next immutable batch captures remaining loss once", flush=True)
    path = create(root, init)
    hook = Hook("after_take_before_begin")
    try:
        with daemon(exe, path, hook, capacity=4) as d, bus() as raw:
            wait_local(d, lambda s: s["pending_batches"] >= 1)
            send(raw, reading(1, 0, 2200, 0, 0))
            token = hook.token()
            for n in range(1, 21):
                send(raw, reading(1, 0, 2800, 0, n))
            d.wait(lambda s: s["valid_readings"] == 21)
            hook.release()
            settled = wait_local(d, lambda s: s["local_readings"] == 5 and s["reading_evicted"] == 16)
            d.stop()
        inv = inventory(path, output)
        batches = [json.loads(r["payload"]) for r in inv["rows"]]
        record_batches = [b for b in batches if b["records"] and any(e["kind"] == "reading" for e in b["records"])]
        assert record_batches[0]["reading_drops"] == 0
        assert sum(b["reading_drops"] for b in batches) == 16
        assert record_batches[0]["capture_ticket"] < record_batches[-1]["capture_ticket"]
        assert inv["runs"][0]["captured_reading_drops"] == 16
        return dict(token=token, exact_reading_loss=16, locally_committed_readings=settled["local_readings"])
    finally:
        hook.close()


def runtime_fault(exe, init, root, output, command, state):
    print(f"EXPECT: runtime {state}: no failed-write durability; bounded held candidate; live high/stale alarms and paced retries", flush=True)
    path = create(root, init)
    hook = Hook("after_take_before_begin")
    try:
        with daemon(exe, path, hook, capacity=4) as d, bus() as raw:
            initial = wait_local(d, lambda s: s["pending_batches"] >= 1)
            send(raw, reading(1, 0, 2800, 0, 0))
            token = hook.token()
            hook.release(command)
            failed = wait_local(d, lambda s: s["local_state"] == state)
            assert failed["local_committed"] == initial["local_committed"]
            for n in range(1, 20):
                send(raw, reading(1, 0, 2800, 0, n))
            snap = d.wait(lambda s: s["valid_readings"] == 20 and source(s, 1, 0)["freshness"] == "stale")
            assert source(snap, 1, 0)["high"] == 1
            latest = wait_local(d, lambda s: s["local_failures"] >= 2)
            assert latest["local_failures"] <= 5, latest  # >=1s retry, no busy loop
            d.stop()
            final_status = next(e for e in reversed(d.events) if e["event"] == "persistence")
            local = next(e for e in reversed(d.events) if e["event"] == "outbox")
            assert local["durable_pending"] == initial["durable_pending"]
            assert final_status["records_written"] == 0
            assert final_status["readings_queued"] == (local["local_readings"] + final_status["readings_dropped"] +
                                                       final_status["reading_depth"] + final_status["in_flight_readings"])
        inv = inventory(path, output)
        recovered_readings = [e for e in committed_records(inv).values() if e["kind"] == "reading"]
        # A failed xSync can leave a whole recoverable transaction. The failed
        # call must publish none; recovery may establish durability later.
        assert len(recovered_readings) in ((0, 1) if command == b"S" else (0,))
        if recovered_readings:
            assert recovered_readings[0]["detail"]["counter"] == 0
        with daemon(exe, path) as restarted:
            wait_local(restarted, lambda s: s["durable_pending"] >= inv["meta"]["pending_records"])
            restarted.stop()
        after = inventory(path, output.with_name(output.name + "-restart"))
        assert all(committed_records(after).get(k) == v for k, v in committed_records(inv).items())
        return dict(token=token, state=state, failures=latest["local_failures"], sizes=inv["sizes"])
    finally:
        hook.close()


def logical_full(exe, init, root, output):
    print("EXPECT: record cap preserves older committed bytes, holds one candidate and evicts only new volatile lane records", flush=True)
    path = create(root, init)
    env = dict(os.environ, VIGIL_TEST_RECORD_LIMIT="5")
    with daemon(exe, path, env=env, capacity=4) as d, bus() as raw:
        wait_local(d, lambda s: s["pending_batches"] >= 1)
        for n in range(4):
            send(raw, reading(1, 0, 2200, 0, n))
        good = wait_local(d, lambda s: s["local_readings"] == 4)
        send(raw, reading(1, 0, 2200, 0, 4))
        wait_local(d, lambda s: s["local_state"] == "full")
        for n in range(5, 25):
            send(raw, reading(1, 0, 2200, 0, n))
        wait_local(d, lambda s: s["reading_evicted"] >= 16)
        d.stop()
    inv = inventory(path, output)
    assert inv["meta"]["pending_records"] == good["durable_pending"] == 5
    readings = [e for e in committed_records(inv).values() if e["kind"] == "reading"]
    assert len(readings) == 4
    assert [e["detail"]["counter"] for e in readings] == [0, 1, 2, 3]
    return dict(records=5, retained_readings=4, bytes=inv["meta"]["pending_payload_bytes"])


def startup_errors(exe, init, root):
    print("EXPECT: missing/symlink/corrupt/version/permissions/duplicate stores fail before READY; no silent reset", flush=True)
    root.mkdir(mode=0o700)
    missing = root / "missing.sqlite3"
    def reject(path):
        r = subprocess.run([str(exe), "--outbox-path", str(path)], capture_output=True, timeout=5)
        assert r.returncode == 2 and b"ready " not in r.stderr, r.stderr
    reject(missing)
    assert not missing.exists()
    path = create(root / "valid", init)
    link = root / "link.sqlite3"
    link.symlink_to(path)
    reject(link)
    with daemon(exe, path):
        reject(path)
    path.chmod(0o644)
    reject(path)
    path.chmod(0o600)
    before = path.read_bytes()
    with path.open("r+b") as f:
        f.write(b"not a sqlite database")
    corrupt = path.read_bytes()
    reject(path)
    assert path.read_bytes() == corrupt and path.read_bytes() != before
    return dict(rejected=5)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", type=Path, required=True)
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--restarts", type=int, default=20)
    args = ap.parse_args()
    assert sys.platform == "linux" and args.restarts >= 1
    build = args.build.resolve()
    exe, init = build / "vigild-resilience-test", build / "vigil-outbox-init"
    args.output.mkdir(parents=True, exist_ok=False)
    result = {"sqlite": subprocess.check_output([SQLITE, "--version"], text=True).strip(),
              "binary_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(), "cases": {}}
    # Keep state and DB/WAL evidence native to Linux. Console/result JSON may be
    # copied to the repository; DB/WAL native inventories remain at this path.
    native = Path(tempfile.mkdtemp(prefix="vigil-stage1-evidence-"))
    result["native_evidence"] = str(native)
    for hook in ("after_take_before_begin", "after_local_begin", "after_local_insert_before_commit", "after_local_commit_before_publish"):
        case = native / hook
        case.mkdir(mode=0o700)
        result["cases"][hook] = crash_cut(exe, init, case / "state", case / "evidence", hook, args.restarts)
    result["cases"]["origin_crashes"] = startup_crashes(exe, init, native / "origins", native / "origin-inventories", args.restarts)
    result["cases"]["capture_race"] = capture_race(exe, init, native / "capture-race", native / "capture-race-inventory")
    for command, state in ((b"F", "full"), (b"B", "busy"), (b"R", "readonly"), (b"I", "recovering"), (b"S", "recovering")):
        result["cases"][state + command.decode()] = runtime_fault(exe, init, native / (state + command.decode()),
                                                                  native / ("inventory-" + state + command.decode()), command, state)
    result["cases"]["capacity"] = logical_full(exe, init, native / "capacity", native / "capacity-inventory")
    result["cases"]["startup"] = startup_errors(exe, init, native / "startup")
    production = (build / "vigild").read_bytes()
    assert b"VIGIL_TEST_" not in production and b"vigil-test-vfs" not in production
    (args.output / "result.json").write_text(json.dumps(result, indent=2))
    print(f"PASS Stage 1: {len(result['cases'])} cases; direct DB/WAL inventories: {native}", flush=True)


if __name__ == "__main__":
    main()
