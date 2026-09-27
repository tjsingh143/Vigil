#!/usr/bin/env python3
"""Executable brief scenario table. Genuine SocketCAN/PostgreSQL/systemd required."""
import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import signal
import socket
import struct
import sys
import time
from evidence import Evidence
from monitor_integration import (Daemon, bus, config_text, send, source, pod_status,
                                 rejection, progress)
from persistence_integration import Postgres, outage, systemd_gate
from simulator.pod import Pod
from simulator.protocol import reading, heartbeat


@contextmanager
def monitored(executable, **config):
    d = Daemon(executable, config_text(pods=(1,), rate=0, **config))
    try:
        yield d
    finally:
        d.close()


def alarm_events(d, kind, channel=0):
    return [e for e in d.events if e["event"] == "alarm" and e["kind"] == kind
            and e["pod"] == 1 and e["channel"] == channel]


def silent(executable):
    with monitored(executable, timeout=500) as d, bus() as raw:
        pod = Pod(1)
        for tick in range(3):
            for frame in pod.cycle(tick, 0):
                send(raw, frame)
        healthy = d.wait(lambda s: s["frames"] == 9)
        assert all(source(healthy, 1, c)["current"] for c in (0, 1))
        stale = d.wait(lambda s: s["now_ms"] > healthy["now_ms"] and s["stale_events"] == 3)
        measured = []
        for channel in (-1, 0, 1):
            last = (pod_status(stale, 1)["last_heartbeat_ms"] if channel == -1
                    else source(stale, 1, channel)["last_valid_ms"])
            event = next(e for e in alarm_events(d, "communication_lost", channel)
                         if e["state"] == "active")
            age = event["now_ms"] - last
            assert 500 <= age <= 720, age  # tick 20 ms + explicitly bounded VM allowance
            measured.append(dict(channel=channel, last_valid_ms=last,
                                 alarm_ms=event["now_ms"], detection_age_ms=age))
            if channel >= 0:
                row = source(stale, 1, channel)
                assert row["current"] == 0 and row["freshness"] == "stale"
                assert row["value"] == source(healthy, 1, channel)["value"]
        # Restarted counters after actual stream silence must recover.
        for frame in Pod(1).cycle(0, 0):
            send(raw, frame)
        recovered = d.wait(lambda s: s["frames"] == 12 and s["recoveries"] == 3)
        assert all(source(recovered, 1, c)["current"] for c in (0, 1))
        assert recovered["counter_resets"] == 2 and recovered["rejected"] == 0
        d.stop()
        return dict(timeout_ms=500, tick_ms=20, scheduling_allowance_ms=200,
                    sources=measured, recoveries=3, recovery_snapshot_ms=recovered["now_ms"])


def malformed(executable):
    with monitored(executable, timeout=3000) as d, bus() as raw:
        send(raw, reading(1, 0, 2800, 0, 10))
        before = d.wait(lambda s: s["frames"] == 1)
        send(raw, (0x101, b"\0"))
        send(raw, (0x101, bytes.fromhex("0098080000b6")))
        after = d.wait(lambda s: s["frames"] == 3)
        assert after["rejected"] == 2 and after["valid_readings"] == 1
        assert rejection(after, "length") == rejection(after, "crc") == 1
        assert after["alarm_transitions"] == before["alarm_transitions"] == 1
        for field in ("value", "current", "high", "last_valid_ms", "accepted", "validity"):
            assert source(after, 1, 0)[field] == source(before, 1, 0)[field]
        d.stop()
        return dict(frames=3, rejected=2, length=1, crc=1, alarm_transitions=1,
                    before_ms=before["now_ms"], after_ms=after["now_ms"])


def nonadvancing(executable):
    with monitored(executable, timeout=3000) as d, bus() as raw:
        send(raw, reading(1, 0, 2200, 0, 10))
        before = d.wait(lambda s: s["frames"] == 1)
        for counter in (10, 9, 138):
            send(raw, reading(1, 0, 2800, 0, counter))
        after = d.wait(lambda s: s["frames"] == 4)
        assert after["valid_readings"] == 1 and after["rejected"] == 3
        assert rejection(after, "repeat") == 1 and rejection(after, "old") == 2
        assert source(after, 1, 0)["value"] == 2200 and source(after, 1, 0)["high"] == 0
        assert source(after, 1, 0)["last_valid_ms"] == source(before, 1, 0)["last_valid_ms"]
        assert progress(after, 1, -2)["counter"] == 10
        d.stop()
        return dict(repeat=1, old_or_ambiguous=2, accepted=1, baseline_counter=10,
                    observed_ms=after["now_ms"])


def jump(executable):
    with monitored(executable, timeout=3000) as d, bus() as raw:
        send(raw, reading(1, 0, 2200, 0, 10))
        send(raw, reading(1, 0, 2400, 0, 14))
        snap = d.wait(lambda s: s["frames"] == 2)
        assert snap["loss"] == 3 and snap["valid_readings"] == 2 and snap["rejected"] == 0
        assert source(snap, 1, 0)["value"] == 2400 and source(snap, 1, 0)["current"] == 1
        assert progress(snap, 1, -2)["counter"] == 14
        d.stop()
        return dict(previous=10, counter=14, inferred_loss=3, accepted=2,
                    value=2400, observed_ms=snap["now_ms"])


def hysteresis(executable):
    values = (2699, 2700, 2699, 2701, 2699, 2700, 2601, 2600)
    with monitored(executable, timeout=3000) as d, bus() as raw:
        for counter, value in enumerate(values):
            send(raw, reading(1, 0, value, 0, counter))
        snap = d.wait(lambda s: s["frames"] == len(values))
        transitions = alarm_events(d, "high")
        assert [e["state"] for e in transitions] == ["active", "cleared"]
        assert source(snap, 1, 0)["high"] == 0 and snap["rejected"] == 0
        d.stop()
        return dict(values=list(values), raise_at=2700, clear_at=2600, transitions=transitions)


def load(executable, forced=False, count=50000):
    """Account kernel delivery independently of ambiguous 8-bit sequence gaps."""
    with monitored(executable, timeout=5000, status=100) as d, bus() as raw:
        d.allow_rx_overflow = True
        d.wait(lambda s: s["id"] == 1)
        # Encode outside the timed send loop; bounded memory, fixed traffic.
        frames = []
        for counter in range(256):
            can_id, data = reading(1, 0, 2200, 0, counter)
            frames.append(struct.pack("=IB3x8s", can_id, len(data), data))
        if forced:
            d.child.send_signal(signal.SIGSTOP)
            deadline = time.monotonic() + 2
            while "State:\tT" not in Path(f"/proc/{d.child.pid}/status").read_text():
                assert time.monotonic() < deadline, "SIGSTOP deadline"
                time.sleep(0.005)
        start = time.monotonic_ns()
        try:
            for index in range(count):
                assert raw.send(frames[index % 256]) == 16
                if not forced and index % 128 == 0:
                    d.pump(0)
        finally:
            if forced:
                d.child.send_signal(signal.SIGCONT)
        sent_ns = time.monotonic_ns()
        # Wait until the pre-probe kernel backlog has drained. A later received
        # heartbeat exposes SO_RXQ_OVFL even if the final burst frames were lost.
        end_ms = sent_ns // 1000000
        d.wait(lambda s: s["now_ms"] >= end_ms + 200)
        send(raw, heartbeat(1, 1, 0, 0))
        done = d.wait(lambda s: s["heartbeats"] == 1 and
                      s["frames"] + s["rx_overflow"] == count + 1)
        assert done["frames"] == done["valid_readings"] + done["heartbeats"] + done["rejected"]
        assert done["diagnostics_dropped"] == 0
        if forced:
            assert done["rx_overflow"] > 0
        d.stop()
        return dict(sent=count + 1, received=done["frames"], kernel_drops=done["rx_overflow"],
                    accepted_readings=done["valid_readings"], rejected=done["rejected"],
                    inferred_sequence_loss=done["loss"], send_start_mono_ns=start,
                    send_end_mono_ns=sent_ns, send_duration_s=(sent_ns-start)/1e9,
                    offered_frames_per_s=round(count * 1e9/(sent_ns-start)),
                    observed_ms=done["now_ms"], missed_ticks=done["missed_ticks"],
                    forced_receiver_pause=forced, persistence_enabled=False,
                    scope="Linux application delivery only; no physical CAN claims")


def database_case(executable, action):
    pg = Postgres()
    try:
        return action(executable, pg)
    finally:
        directory = Path(os.environ["VIGIL_EVIDENCE_DIR"])
        (directory / "postgres.log").write_text((pg.root / "postgres.log").read_text())
        pg.close()


def main():
    if sys.platform != "linux" or sys.flags.optimize:
        raise RuntimeError("real Linux required; assertions must remain enabled")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--daemon", default="build/vigild")
    parser.add_argument("--output", required=True, help="new artifact directory (never overwrite evidence)")
    parser.add_argument("--scenario", help="run just one named scenario")
    args = parser.parse_args()
    exe = str(Path(args.daemon).resolve())
    cases = [
        ("silent-pod", "500 ms freshness deadline: stale/current=0 and three alarms; "
         "observe by 720 ms (20 ms tick + 200 ms scheduling allowance); return clears all three", lambda: silent(exe)),
        ("malformed", "one length + one CRC rejection; no value/freshness/high-alarm mutation", lambda: malformed(exe)),
        ("non-advancing", "one repeat + two old/ambiguous rejected; baseline/value unchanged", lambda: nonadvancing(exe)),
        ("counter-jump", "10 -> 14 infers exactly three lost; both readings accepted", lambda: jump(exe)),
        ("hysteresis", "eight readings around 2700/2600 produce exactly one raise and one clear", lambda: hysteresis(exe)),
        ("database-backfill", "capacity 8, five outage readings: five backfill, zero drops/gaps; live timeout",
         lambda: database_case(exe, lambda e, p: outage(e, p, False))),
        ("queue-overflow", "capacity 4, eleven outage readings: newest four backfill, exactly seven drops; degraded + gap",
         lambda: database_case(exe, lambda e, p: outage(e, p, True))),
        ("crash-systemd", "two committed readings survive; exactly three known queued readings vanish; "
         "systemd restarts after SIGKILL and watchdog; no invented old-run backfill",
         lambda: database_case(exe, systemd_gate)),
        ("high-rate", "50000 readings + probe: sent = received + exact kernel drops; received = accepted + rejected; "
         "report rate for this run only, persistence disabled", lambda: load(exe)),
        ("receive-overflow", "paused receiver forces kernel drops; all 50001 sends accounted after drain/probe; "
         "separate kernel drops from sequence inference", lambda: load(exe, True)),
    ]
    if args.scenario:
        cases = [case for case in cases if case[0] == args.scenario]
        if not cases:
            parser.error("unknown scenario")
    evidence = Evidence(args.output)
    for name, expected, action in cases:
        evidence.run(name, expected, action)
    evidence.finish()


if __name__ == "__main__":
    main()
