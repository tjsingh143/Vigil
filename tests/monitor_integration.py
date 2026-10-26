#!/usr/bin/env python3
"""Checkpoint 2 real Linux gate. Requires otherwise idle vcan0.

Written expectations: docs/CHECKPOINT_2_VERIFICATION.md and scenario docstrings.
Readiness/condition driven, bounded deadlines. Real time is used only to prove
the Linux timeout/event-loop behavior; physical checks use deterministic C tests.
"""
from verification import require  # also refuses optimized execution before fixture imports

import argparse
from contextlib import contextmanager
import os
from pathlib import Path
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from simulator.protocol import heartbeat, reading, crc8  # noqa: E402


def config_text(pods=(1, 2), timeout=600, status=100, rate=500, flat=0):
    text = f"tick_ms 20\nstatus_ms {status}\n"
    for pod in pods:
        text += (f"pod {pod} {timeout}\n"
                 f"source {pod} temperature {timeout} -4000 8500 {rate} {flat} 2700 2600\n"
                 f"source {pod} door {timeout} 0 1 0 0 1 0\n")
    return text


def parse(line):
    event, *fields = line.split()
    result = {"event": event}
    for field in fields:
        key, value = field.split("=", 1)
        # SQLSTATE is a five-character identifier: 08001 must retain its zero.
        result[key] = int(value) if key != "last_error" and value.lstrip("-").isdigit() else value
    return result


def source(snap, pod, channel):
    return next(row for row in snap["rows"] if row["event"] == "source"
                and row["pod"] == pod and row["channel"] == channel)


def pod_status(snap, pod):
    return next(row for row in snap["rows"] if row["event"] == "pod" and row["pod"] == pod)


def rejection(snap, reason):
    return next(row["count"] for row in snap["rows"]
                if row["event"] == "reject_count" and row["reason"] == reason)


def progress(snap, pod, channel):
    return next(row for row in snap["rows"] if row["event"] == "progress"
                and row["pod"] == pod and row["channel"] == channel)


class Daemon:
    def __init__(self, executable, config, persistence=False, extra_args=(), env=None, pipesize=-1, startup_timeout=5):
        self.config = tempfile.NamedTemporaryFile(mode="w+", prefix="vigil-live-", suffix=".conf")
        self.config.write(config)
        self.config.flush()
        self.child = subprocess.Popen([executable, "--config", self.config.name,
                                      *([] if persistence else ["--no-persistence"]), *extra_args], cwd=ROOT, env=env,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, pipesize=pipesize)
        self.buffer = b""
        self.pending = None
        self.snapshots = []
        self.events = []
        self.transcript = bytearray()
        self.errors = bytearray()
        self.allow_rx_overflow = False
        try:
            require((select.select([self.child.stderr], [], [], startup_timeout)[0]), lambda: ("daemon readiness timeout"))
            line = self.child.stderr.readline()
            self.errors.extend(line)
            require((line.startswith(b"ready ")), lambda: (line))
            # /proc enumeration is not atomic: the separate PG owner may
            # close an ephemeral socket between readdir and readlink.
            targets = []
            for path in Path(f"/proc/{self.child.pid}/fd").iterdir():
                try:
                    targets.append(os.readlink(path))
                except FileNotFoundError:
                    continue
            require((self.child.poll() is None))
            require((targets.count("anon_inode:[signalfd]") == 1), lambda: (targets))
            require((targets.count("anon_inode:[timerfd]") == 1), lambda: (targets))
            if not persistence:
                require((sum(target.startswith("socket:") for target in targets) == 1), lambda: (targets))
            require((self.config.name not in targets), lambda: (targets))  # config closed before ready
        except BaseException:
            self.close()
            raise

    def pump(self, timeout=0.1):
        if not select.select([self.child.stdout], [], [], timeout)[0]:
            require((self.child.poll() is None), lambda: (self.child.stderr.read().decode()))
            return
        data = os.read(self.child.stdout.fileno(), 65536)
        if not data:
            require((self.child.poll() in (None, 0)), lambda: (self.child.stderr.read().decode()))
            return
        self.buffer += data
        self.transcript.extend(data)
        while b"\n" in self.buffer:
            line, self.buffer = self.buffer.split(b"\n", 1)
            row = parse(line.decode())
            self.events.append(row)
            if row["event"] == "snapshot":
                self.pending = dict(row, rows=[])
            elif row["event"] == "snapshot_end":
                require((self.pending is not None and self.pending["id"] == row["id"]))
                require((row["diagnostics_dropped"] == 0), lambda: (row))
                self.snapshots.append(self.pending)
                self.pending = None
            elif self.pending is not None:
                self.pending["rows"].append(row)

    def wait(self, predicate, timeout=5):
        deadline = time.monotonic() + timeout
        checked = 0
        while time.monotonic() < deadline:
            for snap in self.snapshots[checked:]:
                if predicate(snap):
                    require((snap["frames"] == snap["valid_readings"] + snap["heartbeats"] + snap["rejected"]))
                    require((snap["rejected"] == sum(row["count"] for row in snap["rows"]
                                                   if row["event"] == "reject_count")))
                    require((self.allow_rx_overflow or snap["rx_overflow"] == 0))
                    return snap
            checked = len(self.snapshots)
            self.pump(min(0.1, max(0, deadline - time.monotonic())))
        raise AssertionError(f"condition timeout; latest={self.snapshots[-1:]}")

    def stop(self, stop_signal=signal.SIGTERM):
        self.child.send_signal(stop_signal)
        deadline = time.monotonic() + 2
        while self.child.poll() is None and time.monotonic() < deadline:
            self.pump(0.05)
        require((self.child.wait(timeout=0.1) == 0))
        self.pump(0)
        stderr = self.child.stderr.read().decode()
        self.errors.extend(stderr.encode())
        require((not stderr), lambda: (stderr))  # includes any sanitizer report
        require((any(row["event"] == "stopped" for row in self.events)))

    def close(self):
        if self.child.poll() is None:
            self.child.terminate()
            try:
                self.child.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.child.kill()
                self.child.wait(timeout=2)
        self.transcript.extend(self.child.stdout.read())
        self.errors.extend(self.child.stderr.read())
        directory = os.environ.get("VIGIL_EVIDENCE_DIR")
        if directory:
            prefix = Path(directory) / f"daemon-{self.child.pid}"
            prefix.with_suffix(".stdout").write_bytes(self.transcript)
            prefix.with_suffix(".stderr").write_bytes(self.errors)
            self.config.seek(0)
            prefix.with_suffix(".conf").write_text(self.config.read())
        self.child.stdout.close()
        self.child.stderr.close()
        self.config.close()


@contextmanager
def daemon(executable, config):
    instance = Daemon(executable, config)
    try:
        yield instance
    finally:
        instance.close()


@contextmanager
def simulator(pod, observer=None):
    child = subprocess.Popen([sys.executable, "-m", "simulator.pod", "--pod-id", str(pod),
                              "--interval", "0.05"], cwd=ROOT,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 5
        while not select.select([child.stdout], [], [], 0.02)[0]:
            require((time.monotonic() < deadline), lambda: ("simulator readiness timeout"))
            if observer is not None:
                observer.pump(0)
        require((child.stdout.readline().startswith(b"ready ")))
        yield child
    finally:
        if child.poll() is None:
            child.terminate()
        try:
            deadline = time.monotonic() + 2
            while child.poll() is None and observer is not None and time.monotonic() < deadline:
                observer.pump(0.02)
            out, err = child.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            child.kill()
            child.communicate(timeout=2)
            raise
        require((child.returncode == 0), lambda: ((out, err)))
        require((b"stopped cycles=" in out), lambda: (out))


@contextmanager
def bus():
    with socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW) as raw:
        raw.bind(("vcan0",))
        yield raw


def send(raw, frame):
    can_id, data = frame
    require((raw.send(struct.pack("=IB3x8s", can_id, len(data), data)) == 16))


def finite_simulator(executable, repeated=False):
    """Two pods x three cycles: 12 readings + 6 heartbeats; repeats add 18 rejections."""
    # Count/transport evidence must not depend on receive-time slope estimates
    # when the scheduler delivers several queued cycles in one poll turn.
    with daemon(executable, config_text(timeout=1200, rate=0)) as d:
        args = [sys.executable, "-m", "simulator.pod", "--pod-id", "1", "--pod-id", "2",
                "--cycles", "3", "--interval", "0.05"]
        if repeated:
            args.append("--repeat-each-frame")
        sim = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=ROOT)
        try:
            deadline = time.monotonic() + 5
            while sim.poll() is None:
                require((time.monotonic() < deadline), lambda: ("finite simulator deadline"))
                d.pump(0.02)
            _, errors = sim.communicate(timeout=1)
            require((sim.returncode == 0), lambda: (errors))
        finally:
            if sim.poll() is None:
                sim.kill()
                sim.wait(timeout=2)
            sim.stdout.close()
            sim.stderr.close()
        expected = 36 if repeated else 18
        snap = d.wait(lambda s: s["frames"] >= expected)
        require((snap["frames"] == expected), lambda: (snap))
        require((snap["valid_readings"] == 12 and snap["heartbeats"] == 6), lambda: (snap))
        require((snap["rejected"] == (18 if repeated else 0)), lambda: (snap))
        require((rejection(snap, "repeat") == (18 if repeated else 0)))
        require((snap["loss"] == 0))
        for pod in (1, 2):
            for channel, value in ((0, 2202), (1, 0)):
                row = source(snap, pod, channel)
                require((row["accepted"] == 3 and row["value"] == value and row["current"] == 1))
            require((progress(snap, pod, -2)["counter"] == 5))
            require((progress(snap, pod, -1)["counter"] == 2))
        d.stop()
    print(f"PASS exact simulator counts: frames={expected}, readings=12, heartbeats=6, "
          f"repeats={18 if repeated else 0}, loss=0")


def silence_restart(executable):
    """Stopping pod 1 stales its two channels + heartbeat, while pod 2 stays fresh."""
    # Rate boundaries have deterministic clock-driven C tests; this case isolates
    # freshness/recovery from receive-time batching after VM scheduling stalls.
    with daemon(executable, config_text(rate=0)) as d, simulator(2, d):
        with simulator(1, d):
            healthy = d.wait(lambda s: all(source(s, p, c)["accepted"] >= 3
                                 and source(s, p, c)["current"] == 1
                                 for p in (1, 2) for c in (0, 1))
                                 and all(pod_status(s, p)["heartbeat"] == "fresh" for p in (1, 2)))
        stale = d.wait(lambda s: s["now_ms"] > healthy["now_ms"]
                       and all(source(s, 1, c)["freshness"] == "stale" for c in (0, 1))
                       and pod_status(s, 1)["heartbeat"] == "stale")
        for c in (0, 1):
            require((source(stale, 1, c)["current"] == 0))
            require((source(stale, 2, c)["current"] == 1))
        require((pod_status(stale, 2)["heartbeat"] == "fresh"))
        # Interpreter/process startup may exceed a freshness interval on a VM.
        # Measure this fault from the observed healthy state, not process birth.
        require((stale["stale_events"] - healthy["stale_events"] == 3), lambda: ((healthy, stale)))
        ages = []
        for c in (-1, 0, 1):
            event = next(e for e in d.events if e["event"] == "alarm"
                         and e["kind"] == "communication_lost" and e["state"] == "active"
                         and e["pod"] == 1 and e["channel"] == c
                         and e["now_ms"] > healthy["now_ms"])
            last = (pod_status(stale, 1)["last_heartbeat_ms"] if c == -1
                    else source(stale, 1, c)["last_valid_ms"])
            age = event["now_ms"] - last
            require((600 <= age <= 820), lambda: (age))  # 20 ms tick + 200 ms VM scheduling allowance
            ages.append(age)
        with simulator(1, d):
            recovered = d.wait(lambda s: s["now_ms"] > stale["now_ms"]
                               and s["recoveries"] - healthy["recoveries"] == 3 and
                               all(source(s, 1, c)["current"] == 1 for c in (0, 1)))
            require((recovered["counter_resets"] - healthy["counter_resets"] == 2))
            require((recovered["rejected"] == 0), lambda: (recovered))
            require((recovered["loss"] == 0), lambda: (recovered))
        d.stop(signal.SIGINT)
    print(f"PASS pod 1 stop/restart; pod 2 remained fresh; communication alarm ages={ages} ms; "
          "3 recoveries, 2 counter resets, no rejection/loss")


def faults(executable):
    """Malformed frames leave alarms/values untouched; gap accepted, repeats/old rejected."""
    with daemon(executable, config_text(pods=(1,), timeout=3000, rate=0)) as d, bus() as raw:
        for frame in (reading(1, 0, 2200, 0, 10), reading(1, 1, 0, 0, 11), heartbeat(1, 1, 5, 0)):
            send(raw, frame)
        before = d.wait(lambda s: s["frames"] == 3)
        malformed = [(0x101, b"\0"), (0x101, bytes.fromhex("0098080000b6")),
                     (0x100, bytes.fromhex("0098080000b7")),
                     (0x80000101, bytes.fromhex("0098080000b7")), (0x40000101, b"")]
        for payload in (bytes.fromhex("0298080000"), bytes.fromhex("0098080200"),
                        bytes.fromhex("0102000000")):
            malformed.append((0x101, payload + bytes([crc8(payload)])))
        payload = bytes.fromhex("030000000000")
        malformed.append((0x701, payload + bytes([crc8(payload)])))
        for frame in malformed:
            send(raw, frame)
        bad = d.wait(lambda s: s["frames"] == 12)
        require((bad["rejected"] == 9 and bad["alarm_transitions"] == before["alarm_transitions"]))
        for c in (0, 1):
            require((source(bad, 1, c)["value"] == source(before, 1, c)["value"]))
            require((source(bad, 1, c)["last_valid_ms"] == source(before, 1, c)["last_valid_ms"]))
        for reason, count in (("length", 1), ("crc", 1), ("id", 1), ("format", 2),
                              ("channel", 1), ("status", 1), ("value", 1), ("state", 1)):
            require((rejection(bad, reason) == count), lambda: ((reason, bad)))
        for frame in (reading(1, 0, 2300, 0, 15), reading(1, 0, 2400, 0, 15),
                      reading(1, 0, 2400, 0, 14), reading(1, 0, 8501, 0, 16),
                      reading(1, 0, 2300, 1, 17), reading(2, 0, 2200, 0, 0),
                      reading(1, 0, 2700, 0, 18), reading(1, 0, 2699, 0, 19),
                      reading(1, 0, 2701, 0, 20), reading(1, 0, 2601, 0, 21),
                      reading(1, 0, 2600, 0, 22)):
            send(raw, frame)
        after = d.wait(lambda s: s["frames"] == 23)
        require((after["loss"] == 3 and after["rejected"] == 14))
        require((after["valid_readings"] == 8 and after["heartbeats"] == 1))
        for reason in ("repeat", "old", "range", "sensor_fault", "unconfigured_pod"):
            require((rejection(after, reason) == 1))
        require((source(after, 1, 0)["value"] == 2600 and source(after, 1, 0)["current"] == 1))
        high_events = [e for e in d.events if e["event"] == "alarm" and e["kind"] == "high"]
        require(([e["state"] for e in high_events] == ["active", "cleared"]))
        # Independent command-line injector reaches vigild too, with next counter 23.
        _, payload = reading(1, 0, 2500, 0, 23)
        subprocess.run(["cansend", "vcan0", f"101#{payload.hex()}"], check=True, timeout=2)
        d.wait(lambda s: s["frames"] == 24 and source(s, 1, 0)["value"] == 2500)
        d.stop()
    print("PASS live malformed reasons, immutable values/alarms, gap loss=3, repeat/old, "
          "range/fault rejection, hysteresis (one raise/clear), independent cansend")


def heartbeat_only_and_repeats(executable):
    """Repeated readings cannot keep values current; progressing heartbeat stays fresh."""
    with daemon(executable, config_text(pods=(1,), timeout=300)) as d, bus() as raw:
        send(raw, reading(1, 0, 2200, 0, 0))
        send(raw, reading(1, 1, 0, 0, 1))
        start = d.wait(lambda s: s["valid_readings"] == 2)
        deadline = time.monotonic() + 3
        counter = 0
        next_send = time.monotonic()
        while time.monotonic() < deadline:
            if time.monotonic() >= next_send:
                send(raw, reading(1, 1, 1, 0, 1))
                send(raw, heartbeat(1, 1, 0, counter))
                counter = (counter + 1) % 256
                next_send = time.monotonic() + 0.05
            d.pump(0.01)
            if d.snapshots and d.snapshots[-1]["now_ms"] - start["now_ms"] >= 1000:
                break
        snap = d.snapshots[-1]
        require((snap["now_ms"] - start["now_ms"] >= 1000))
        require((snap["valid_readings"] == 2 and snap["counter_resets"] == 0))
        require((rejection(snap, "repeat") >= 10))
        require((all(source(snap, 1, c)["freshness"] == "stale" for c in (0, 1))))
        require((pod_status(snap, 1)["heartbeat"] == "fresh"))
        require((snap["stale_events"] == 2))
        d.stop()
    print("PASS continuous repeated readings stay stale across >3 timeout intervals; heartbeat stays fresh")


def signals_and_cli(executable):
    """Signal-driven snapshots work with periodic output off; invalid startup exits 2."""
    for stop_signal in (signal.SIGINT, signal.SIGTERM):
        with daemon(executable, config_text(pods=(1,), status=0)) as d:
            d.wait(lambda s: s["id"] == 1)
            d.child.send_signal(signal.SIGUSR1)
            d.wait(lambda s: s["id"] == 2)
            d.stop(stop_signal)
    for args in (("--interface", "absent0"), ("--config", "/no/such/config"),
                 ("--unknown", "1"), ("--config",),
                 ("--interface", "vcan0", "--interface", "vcan0")):
        result = subprocess.run([executable, *args], capture_output=True, timeout=3)
        require((result.returncode == 2), lambda: ((args, result.stderr)))
        require((b"AddressSanitizer" not in result.stderr and b"runtime error:" not in result.stderr))
    guard = subprocess.run([sys.executable, "-O", __file__, "--daemon", executable],
                           capture_output=True, timeout=3)
    require((guard.returncode != 0 and b"assertions must remain enabled" in guard.stderr))
    print("PASS SIGUSR1 snapshots, SIGINT/SIGTERM shutdown, startup error paths, optimized-Python guard")


def diagnostic_backpressure(executable):
    """Hold a 4 KiB output pipe unread for 0.6 s; processing must precede the drain."""
    with tempfile.NamedTemporaryFile(mode="w", prefix="vigil-pressure-") as config:
        config.write(config_text(pods=(1,), timeout=200, status=20))
        config.flush()
        child = subprocess.Popen([executable, "--config", config.name, "--no-persistence"], cwd=ROOT,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, pipesize=4096)
        try:
            require((select.select([child.stderr], [], [], 5)[0]))
            require((child.stderr.readline().startswith(b"ready ")))
            with bus() as raw:
                send(raw, reading(1, 0, 2800, 0, 0))
            # This is a deliberate slow-consumer interval, not a readiness sleep.
            try:
                child.wait(timeout=0.6)
                raise AssertionError("daemon exited under backpressure")
            except subprocess.TimeoutExpired:
                pass
            drain_ms = int(time.monotonic() * 1000)
            os.set_blocking(child.stdout.fileno(), False)
            data = b""
            found_summary = found_source = False
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline and not (found_summary and found_source):
                require((select.select([child.stdout], [], [], 1)[0]))
                chunk = os.read(child.stdout.fileno(), 65536)
                require((chunk), lambda: (child.stderr.read().decode()))
                data += chunk
                while b"\n" in data:
                    line, data = data.split(b"\n", 1)
                    row = parse(line.decode())
                    if row["event"] == "snapshot" and row["now_ms"] >= drain_ms:
                        require((row["diagnostics_dropped"] > 0), lambda: (row))
                        require((row["valid_readings"] == 1 and row["stale_events"] == 3), lambda: (row))
                        require((row["alarm_transitions"] == 4), lambda: (row))  # high + 3 communication
                        found_summary = True
                    if row["event"] == "source" and row["channel"] == 0 and row["freshness"] == "stale":
                        require((row["last_valid_ms"] < drain_ms - 200), lambda: (row))
                        require((row["high"] == 1 and row["current"] == 0), lambda: (row))
                        found_source = True
            require((found_summary and found_source))
            child.stdout.close()  # A vanished reader must not terminate via SIGPIPE.
            child.send_signal(signal.SIGUSR1)
            child.terminate()
            require((child.wait(timeout=2) == 0))
            stderr = child.stderr.read()
            require((not stderr), lambda: (stderr))
        finally:
            if child.poll() is None:
                child.kill()
                child.wait(timeout=2)
            child.stdout.close()
            child.stderr.close()
    print("PASS unread/closed diagnostic pipe: readings/timeouts continued, drops counted, alarms retained")


def main():
    if sys.platform != "linux":
        raise RuntimeError("real Linux is required")
    if sys.flags.optimize:
        raise RuntimeError("assertions must remain enabled; remove -O/PYTHONOPTIMIZE")
    parser = argparse.ArgumentParser()
    parser.add_argument("--daemon", required=True)
    args = parser.parse_args()
    executable = str(Path(args.daemon).resolve())
    finite_simulator(executable)
    finite_simulator(executable, repeated=True)
    silence_restart(executable)
    faults(executable)
    heartbeat_only_and_repeats(executable)
    signals_and_cli(executable)
    diagnostic_backpressure(executable)
    print("PASS Checkpoint 2 integration:", executable)


if __name__ == "__main__":
    main()
