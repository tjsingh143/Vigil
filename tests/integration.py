#!/usr/bin/env python3
"""Checkpoint 1 Linux gate. Requires an otherwise idle vcan0.

Expected: can-utils exchange a literal frame; two simulator pods produce exactly
18 consistent frames; malformed traffic fails by reason; SIGINT/SIGTERM promptly
stop the simulator. Both C build variants run this same gate.
"""

import argparse
from contextlib import contextmanager
import json
from pathlib import Path
import select
import signal
import socket
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from simulator.protocol import crc8  # noqa: E402


@contextmanager
def process(args):
    child = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             text=True, cwd=ROOT)
    try:
        yield child
    finally:
        if child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=2)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait(timeout=2)
        child.stdout.close()
        child.stderr.close()


def line_ready(stream, timeout=5):
    assert select.select([stream], [], [], timeout)[0], "readiness timeout"
    line = stream.readline()
    assert line.startswith("ready "), line


def receive(raw):
    frame = raw.recv(16)
    assert len(frame) == 16
    can_id, length, data = struct.unpack("=IB3x8s", frame)
    assert length <= 8
    return can_id, data[:length]


def can_utils_proof():
    with process(["candump", "-L", "-n", "1", "vcan0,123:7FF"]) as dump:
        # candump has no readiness message. Send a bounded handshake until its
        # independent subscription observes it, rather than assume a sleep works.
        deadline = time.monotonic() + 3
        while not select.select([dump.stdout], [], [], 0.02)[0]:
            assert time.monotonic() < deadline, "candump subscription timeout"
            subprocess.run(["cansend", "vcan0", "123#11223344"], check=True)
        stdout, stderr = dump.communicate(timeout=2)
        assert dump.returncode == 0, stderr
        assert "123#11223344" in stdout, stdout
        print("PASS can-utils independent observation:", stdout.strip())


def simulator_proof(decoder):
    with socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW) as raw:
        raw.bind(("vcan0",))
        raw.settimeout(3)
        with process([decoder, "--listen", "vcan0", "18"]) as reader:
            line_ready(reader.stderr)
            sim = subprocess.run([sys.executable, "-m", "simulator.pod",
                                  "--pod-id", "1", "--pod-id", "2",
                                  "--interval", "0.02", "--cycles", "3"],
                                 capture_output=True, text=True, timeout=5, cwd=ROOT)
            assert sim.returncode == 0, sim.stderr
            assert "stopped cycles=3" in sim.stdout
            frames = [receive(raw) for _ in range(18)]
            stdout, stderr = reader.communicate(timeout=5)
            assert reader.returncode == 0, (stdout, stderr)
            decoded = [json.loads(line) for line in stdout.splitlines()]
            assert len(decoded) == 18
            for index, ((can_id, data), message) in enumerate(zip(frames, decoded)):
                tick, slot = divmod(index, 6)
                pod = slot // 3 + 1
                kind = slot % 3
                assert message["result"] == "ok"
                assert message["pod"] == pod
                assert data[-1] == crc8(data[:-1])
                if kind < 2:
                    assert can_id == 0x100 + pod and len(data) == 6
                    channel, value, status, counter = struct.unpack("<BhBB", data[:-1])
                    assert channel == kind and status == 0
                    assert value == (2200 + tick if kind == 0 else 0)
                    assert counter == tick * 2 + kind
                    assert message == dict(result="ok", kind=1, pod=pod,
                                           channel=channel, value=value, status=0,
                                           counter=counter, state=0, uptime_s=0)
                else:
                    assert can_id == 0x700 + pod and len(data) == 7
                    state, uptime, counter = struct.unpack("<BIB", data[:-1])
                    assert state == 1 and counter == tick and uptime <= 4
                    assert message == dict(result="ok", kind=2, pod=pod,
                                           channel=0, value=0, status=0,
                                           counter=counter, state=1, uptime_s=uptime)
                if index < 6:
                    print(f"  observed {can_id:03X}#{data.hex().upper()} -> {message}")
            # The finite simulator must not send an extra cycle.
            assert not select.select([raw], [], [], 0.05)[0]
    print("PASS simulator -> real vcan0 -> independent receiver AND C decoder: 18 frames")


def malformed_proof(decoder):
    bad_channel = bytes.fromhex("0298080000")
    bad_status = bytes.fromhex("0098080200")
    cases = [(0x101, b"\x00", "length"),
             (0x101, bytes.fromhex("0098080000b6"), "crc"),
             (0x100, bytes.fromhex("0098080000b7"), "id"),
             (0x80000101, bytes.fromhex("0098080000b7"), "format"),
             (0x40000101, b"", "format"),
             (0x101, bad_channel + bytes([crc8(bad_channel)]), "channel"),
             (0x101, bad_status + bytes([crc8(bad_status)]), "status")]
    with socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW) as raw:
        raw.bind(("vcan0",))
        with process([decoder, "--listen", "vcan0", str(len(cases))]) as reader:
            line_ready(reader.stderr)
            for can_id, data, _ in cases:
                raw.sendall(struct.pack("=IB3x8s", can_id, len(data), data))
            stdout, stderr = reader.communicate(timeout=5)
            assert reader.returncode == 1, (stdout, stderr)
            actual = [json.loads(line)["result"] for line in stdout.splitlines()]
            assert actual == [case[2] for case in cases], actual
    print("PASS malformed live traffic: length, CRC, ID, EFF, RTR, channel, status")


def shutdown_proof():
    for stop_signal in (signal.SIGINT, signal.SIGTERM):
        with process([sys.executable, "-m", "simulator.pod", "--interval", "30"]) as sim:
            line_ready(sim.stdout)
            sim.send_signal(stop_signal)
            stdout, stderr = sim.communicate(timeout=2)
            assert sim.returncode == 0, (stdout, stderr)
            assert "stopped cycles=" in stdout
    print("PASS simulator SIGINT and SIGTERM shutdown within 2-second deadline")


def cli_proof(decoder):
    guard = subprocess.run([sys.executable, "-O", str(Path(__file__).resolve()),
                            "--decoder", decoder], capture_output=True,
                           text=True, timeout=3)
    assert guard.returncode != 0 and "assertions must remain enabled" in guard.stderr
    assert not guard.stdout
    for line in (ROOT / "tests/protocol_vectors.txt").read_text().splitlines():
        if line.startswith("#"):
            continue
        can_id, payload, *_ = line.split()
        result = subprocess.run([decoder, can_id, payload], capture_output=True,
                                text=True, timeout=3)
        assert result.returncode == 0, result.stderr
        assert json.loads(result.stdout)["result"] == "ok"
    for args in [("101", "zz"), ("101", "0"), ("-1", "00"),
                 ("\t101", "00"), ("", "00"),
                 ("100000000", "00"), ("101", "00" * 9),
                 ("--listen", "vcan0", "0"), ("--listen", "absent0", "1")]:
        result = subprocess.run([decoder, *args], capture_output=True, timeout=3)
        assert result.returncode == 2, (args, result.stderr)
    for args in [("--interval", "nan"), ("--interval", "inf"),
                 ("--interval", "0"), ("--pod-id", "0"),
                 ("--pod-id", "1", "--pod-id", "1"), ("--cycles", "-1")]:
        result = subprocess.run([sys.executable, "-m", "simulator.pod", *args],
                                capture_output=True, timeout=3, cwd=ROOT)
        assert result.returncode == 2, (args, result.stderr)
    print("PASS literal CLI vectors and invalid decoder/simulator arguments")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--decoder", required=True)
    args = parser.parse_args()
    decoder = str(Path(args.decoder).resolve())
    if sys.platform != "linux":
        raise RuntimeError("real Linux is required")
    if sys.flags.optimize:
        raise RuntimeError("assertions must remain enabled; remove -O/PYTHONOPTIMIZE")
    can_utils_proof()
    simulator_proof(decoder)
    malformed_proof(decoder)
    shutdown_proof()
    cli_proof(decoder)
    print("PASS Checkpoint 1 integration:", decoder)


if __name__ == "__main__":
    main()
