#!/usr/bin/env python3
"""Checkpoint 2 real Linux gate. Requires otherwise idle vcan0.

Written expectations: docs/CHECKPOINT_2_VERIFICATION.md and scenario docstrings.
Readiness/condition driven, bounded deadlines. Real time is used only to prove
the Linux timeout/event-loop behavior; physical checks use deterministic C tests.
"""

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
        result[key] = int(value) if value.lstrip("-").isdigit() else value
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
    def __init__(self, executable, config, persistence=False, extra_args=(), env=None):
        self.config = tempfile.NamedTemporaryFile(mode="w", prefix="vigil-live-", suffix=".conf")
        self.config.write(config)
        self.config.flush()
        self.child = subprocess.Popen([executable, "--config", self.config.name,
                                      *([] if persistence else ["--no-persistence"]), *extra_args], cwd=ROOT, env=env,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.buffer = b""
        self.pending = None
        self.snapshots = []
        self.events = []
        try:
            assert select.select([self.child.stderr], [], [], 5)[0], "daemon readiness timeout"
            line = self.child.stderr.readline()
            assert line.startswith(b"ready "), line
            targets = [os.readlink(path) for path in Path(f"/proc/{self.child.pid}/fd").iterdir()]
            assert targets.count("anon_inode:[signalfd]") == 1, targets
            assert targets.count("anon_inode:[timerfd]") == 1, targets
            if not persistence:
                assert sum(target.startswith("socket:") for target in targets) == 1, targets
            assert self.config.name not in targets, targets  # config closed before ready
        except BaseException:
            self.close()
            raise

    def pump(self, timeout=0.1):
        if not select.select([self.child.stdout], [], [], timeout)[0]:
            assert self.child.poll() is None, self.child.stderr.read().decode()
            return
        data = os.read(self.child.stdout.fileno(), 65536)
        if not data:
            assert self.child.poll() in (None, 0), self.child.stderr.read().decode()
            return
        self.buffer += data
        while b"\n" in self.buffer:
            line, self.buffer = self.buffer.split(b"\n", 1)
            row = parse(line.decode())
            self.events.append(row)
            if row["event"] == "snapshot":
                self.pending = dict(row, rows=[])
            elif row["event"] == "snapshot_end":
                assert self.pending is not None and self.pending["id"] == row["id"]
                assert row["diagnostics_dropped"] == 0, row
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
                    assert snap["frames"] == snap["valid_readings"] + snap["heartbeats"] + snap["rejected"]
                    assert snap["rejected"] == sum(row["count"] for row in snap["rows"]
                                                   if row["event"] == "reject_count")
                    assert snap["rx_overflow"] == 0
                    return snap
            checked = len(self.snapshots)
            self.pump(min(0.1, max(0, deadline - time.monotonic())))
        raise AssertionError(f"condition timeout; latest={self.snapshots[-1:]}")

    def stop(self, stop_signal=signal.SIGTERM):
        self.child.send_signal(stop_signal)
        deadline = time.monotonic() + 2
        while self.child.poll() is None and time.monotonic() < deadline:
            self.pump(0.05)
        assert self.child.wait(timeout=0.1) == 0
        self.pump(0)
        stderr = self.child.stderr.read().decode()
        assert not stderr, stderr  # includes any sanitizer report
        assert any(row["event"] == "stopped" for row in self.events)

    def close(self):
        if self.child.poll() is None:
            self.child.terminate()
            try:
                self.child.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.child.kill()
                self.child.wait(timeout=2)
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
def simulator(pod):
    child = subprocess.Popen([sys.executable, "-m", "simulator.pod", "--pod-id", str(pod),
                              "--interval", "0.05"], cwd=ROOT,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        assert select.select([child.stdout], [], [], 5)[0]
        assert child.stdout.readline().startswith(b"ready ")
        yield child
    finally:
        if child.poll() is None:
            child.terminate()
        try:
            out, err = child.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            child.kill()
            child.communicate(timeout=2)
            raise
        assert child.returncode == 0, (out, err)
        assert b"stopped cycles=" in out, out


@contextmanager
def bus():
    with socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW) as raw:
        raw.bind(("vcan0",))
        yield raw


def send(raw, frame):
    can_id, data = frame
    assert raw.send(struct.pack("=IB3x8s", can_id, len(data), data)) == 16


def finite_simulator(executable, repeated=False):
    """Two pods x three cycles: 12 readings + 6 heartbeats; repeats add 18 rejections."""
    with daemon(executable, config_text(timeout=1200)) as d:
        args = [sys.executable, "-m", "simulator.pod", "--pod-id", "1", "--pod-id", "2",
                "--cycles", "3", "--interval", "0.05"]
        if repeated:
            args.append("--repeat-each-frame")
        sim = subprocess.run(args, capture_output=True, timeout=5, cwd=ROOT)
        assert sim.returncode == 0, sim.stderr
        expected = 36 if repeated else 18
        snap = d.wait(lambda s: s["frames"] >= expected)
        assert snap["frames"] == expected, snap
        assert snap["valid_readings"] == 12 and snap["heartbeats"] == 6, snap
        assert snap["rejected"] == (18 if repeated else 0), snap
        assert rejection(snap, "repeat") == (18 if repeated else 0)
        assert snap["loss"] == 0
        for pod in (1, 2):
            for channel, value in ((0, 2202), (1, 0)):
                row = source(snap, pod, channel)
                assert row["accepted"] == 3 and row["value"] == value and row["current"] == 1
            assert progress(snap, pod, -2)["counter"] == 5
            assert progress(snap, pod, -1)["counter"] == 2
        d.stop()
    print(f"PASS exact simulator counts: frames={expected}, readings=12, heartbeats=6, "
          f"repeats={18 if repeated else 0}, loss=0")


def silence_restart(executable):
    """Stopping pod 1 stales its two channels + heartbeat, while pod 2 stays fresh."""
    with daemon(executable, config_text()) as d, simulator(2):
        with simulator(1):
            d.wait(lambda s: all(source(s, p, c)["accepted"] >= 3
                                 for p in (1, 2) for c in (0, 1)))
        stale = d.wait(lambda s: all(source(s, 1, c)["freshness"] == "stale" for c in (0, 1))
                       and pod_status(s, 1)["heartbeat"] == "stale")
        for c in (0, 1):
            assert source(stale, 1, c)["current"] == 0
            assert source(stale, 2, c)["current"] == 1
        assert pod_status(stale, 2)["heartbeat"] == "fresh"
        assert stale["stale_events"] == 3
        ages = []
        for c in (-1, 0, 1):
            event = next(e for e in d.events if e["event"] == "alarm"
                         and e["kind"] == "communication_lost" and e["state"] == "active"
                         and e["pod"] == 1 and e["channel"] == c)
            last = (pod_status(stale, 1)["last_heartbeat_ms"] if c == -1
                    else source(stale, 1, c)["last_valid_ms"])
            age = event["now_ms"] - last
            assert 600 <= age <= 820, age  # 20 ms tick + 200 ms VM scheduling allowance
            ages.append(age)
        with simulator(1):
            recovered = d.wait(lambda s: s["recoveries"] == 3 and
                               all(source(s, 1, c)["current"] == 1 for c in (0, 1)))
            assert recovered["counter_resets"] == 2
            assert recovered["rejected"] == 0
            assert recovered["loss"] == 0
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
        assert bad["rejected"] == 9 and bad["alarm_transitions"] == before["alarm_transitions"]
        for c in (0, 1):
            assert source(bad, 1, c)["value"] == source(before, 1, c)["value"]
            assert source(bad, 1, c)["last_valid_ms"] == source(before, 1, c)["last_valid_ms"]
        for reason, count in (("length", 1), ("crc", 1), ("id", 1), ("format", 2),
                              ("channel", 1), ("status", 1), ("value", 1), ("state", 1)):
            assert rejection(bad, reason) == count, (reason, bad)
        for frame in (reading(1, 0, 2300, 0, 15), reading(1, 0, 2400, 0, 15),
                      reading(1, 0, 2400, 0, 14), reading(1, 0, 8501, 0, 16),
                      reading(1, 0, 2300, 1, 17), reading(2, 0, 2200, 0, 0),
                      reading(1, 0, 2700, 0, 18), reading(1, 0, 2699, 0, 19),
                      reading(1, 0, 2701, 0, 20), reading(1, 0, 2601, 0, 21),
                      reading(1, 0, 2600, 0, 22)):
            send(raw, frame)
        after = d.wait(lambda s: s["frames"] == 23)
        assert after["loss"] == 3 and after["rejected"] == 14
        assert after["valid_readings"] == 8 and after["heartbeats"] == 1
        for reason in ("repeat", "old", "range", "sensor_fault", "unconfigured_pod"):
            assert rejection(after, reason) == 1
        assert source(after, 1, 0)["value"] == 2600 and source(after, 1, 0)["current"] == 1
        high_events = [e for e in d.events if e["event"] == "alarm" and e["kind"] == "high"]
        assert [e["state"] for e in high_events] == ["active", "cleared"]
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
        assert snap["now_ms"] - start["now_ms"] >= 1000
        assert snap["valid_readings"] == 2 and snap["counter_resets"] == 0
        assert rejection(snap, "repeat") >= 10
        assert all(source(snap, 1, c)["freshness"] == "stale" for c in (0, 1))
        assert pod_status(snap, 1)["heartbeat"] == "fresh"
        assert snap["stale_events"] == 2
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
        assert result.returncode == 2, (args, result.stderr)
        assert b"AddressSanitizer" not in result.stderr and b"runtime error:" not in result.stderr
    guard = subprocess.run([sys.executable, "-O", __file__, "--daemon", executable],
                           capture_output=True, timeout=3)
    assert guard.returncode != 0 and b"assertions must remain enabled" in guard.stderr
    print("PASS SIGUSR1 snapshots, SIGINT/SIGTERM shutdown, startup error paths, optimized-Python guard")


def diagnostic_backpressure(executable):
    """Hold a 4 KiB output pipe unread for 0.6 s; processing must precede the drain."""
    with tempfile.NamedTemporaryFile(mode="w", prefix="vigil-pressure-") as config:
        config.write(config_text(pods=(1,), timeout=200, status=20))
        config.flush()
        child = subprocess.Popen([executable, "--config", config.name, "--no-persistence"], cwd=ROOT,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, pipesize=4096)
        try:
            assert select.select([child.stderr], [], [], 5)[0]
            assert child.stderr.readline().startswith(b"ready ")
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
                assert select.select([child.stdout], [], [], 1)[0]
                chunk = os.read(child.stdout.fileno(), 65536)
                assert chunk, child.stderr.read().decode()
                data += chunk
                while b"\n" in data:
                    line, data = data.split(b"\n", 1)
                    row = parse(line.decode())
                    if row["event"] == "snapshot" and row["now_ms"] >= drain_ms:
                        assert row["diagnostics_dropped"] > 0, row
                        assert row["valid_readings"] == 1 and row["stale_events"] == 3, row
                        assert row["alarm_transitions"] == 4, row  # high + 3 communication
                        found_summary = True
                    if row["event"] == "source" and row["channel"] == 0 and row["freshness"] == "stale":
                        assert row["last_valid_ms"] < drain_ms - 200, row
                        assert row["high"] == 1 and row["current"] == 0, row
                        found_source = True
            assert found_summary and found_source
            child.stdout.close()  # A vanished reader must not terminate via SIGPIPE.
            child.send_signal(signal.SIGUSR1)
            child.terminate()
            assert child.wait(timeout=2) == 0
            stderr = child.stderr.read()
            assert not stderr, stderr
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
