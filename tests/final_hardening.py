#!/usr/bin/env python3
"""Final boundary/lifecycle checks; expectations precede each real Linux run."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from evidence import Evidence
from monitor_integration import Daemon, bus, config_text, send, source, progress
from persistence_integration import (Postgres, daemon, wait_p, caught_up, readings,
                                     current_alarms, latest, assert_current)
from simulator.protocol import reading, heartbeat


def rapid_restart(exe):
    d = Daemon(exe, config_text(pods=(1,), timeout=500, status=50, rate=0))
    try:
        with bus() as raw:
            def cycle(value, door, state=1):
                for frame in (reading(1, 0, value, 0, 0), reading(1, 1, door, 0, 1),
                              heartbeat(1, state, 0, 0)):
                    send(raw, frame)
            cycle(2800, 1)
            healthy = d.wait(lambda s: s["frames"] == 3)
            ages = []
            for index in range(6):
                # A restart before stream silence cannot use uptime=0/booting
                # to turn old/repeated counters into new measurements.
                cycle(2500, 0, state=0)
                rejected = d.wait(lambda s: s["frames"] == 6 + index * 6)
                assert rejected["rejected"] == (index + 1) * 3
                assert rejected["counter_resets"] == index * 2
                for channel in (0, 1):
                    for field in ("value", "last_valid_ms", "accepted", "high"):
                        assert source(rejected, 1, channel)[field] == source(healthy, 1, channel)[field]
                last_traffic = max(progress(rejected, 1, c)["last_seen_ms"] for c in (-1, -2))
                stale = d.wait(lambda s: s["now_ms"] >= last_traffic + 500 and
                               s["stale_events"] == (index + 1) * 3)
                assert all(source(stale, 1, c)["current"] == 0 for c in (0, 1))
                event = next(e for e in d.events if e["event"] == "alarm" and e["pod"] == 1
                             and e["channel"] == 0 and e["kind"] == "communication_lost"
                             and e["state"] == "active" and e["now_ms"] > healthy["now_ms"])
                age = event["now_ms"] - source(healthy, 1, 0)["last_valid_ms"]
                assert 500 <= age <= 720, age
                ages.append(age)
                cycle(2500 if index % 2 == 0 else 2800, index % 2)
                healthy = d.wait(lambda s: s["frames"] == 9 + index * 6 and
                                 s["recoveries"] == (index + 1) * 3)
                assert healthy["counter_resets"] == (index + 1) * 2
                assert all(source(healthy, 1, c)["current"] for c in (0, 1))
                assert source(healthy, 1, 0)["high"] == index % 2
            d.stop()
            return dict(cycles=6, rejected=18, stream_resets=12, recoveries=18,
                        detection_ages_ms=ages)
    finally:
        d.close()


def repeated_outages(exe, pg):
    with daemon(exe, pg, capacity=4) as d:
        initial = wait_p(d, caught_up)
        rid = initial["run_id"]
        expected = []
        dropped = queued = 0
        durations = []
        loss_bounds = None
        for index, count in enumerate((3, 9, 2)):
            previous = latest(d)
            pg.stop()
            failed = wait_p(d, lambda s: not s["connected"] and
                            s["connection_failures"] > previous["connection_failures"] and
                            s["in_flight_readings"] == 0 and s["degraded"])
            assert failed["last_error"] == "08001", failed
            base = 2800 if index == 1 else 2400
            values = [base + i for i in range(count)]
            sent_ms = time.monotonic_ns() // 1000000
            with bus() as raw:
                for i, value in enumerate(values):
                    send(raw, reading(1, 0, value, 0, i))
            queued += count
            dropped += max(0, count - 4)
            full = wait_p(d, lambda s: s["readings_queued"] == queued)
            assert full["reading_depth"] == min(count, 4) and full["readings_dropped"] == dropped
            if count > 4:
                loss_bounds = (sent_ms, full["now_ms"])
            stale = d.wait(lambda s: s["frames"] == queued and
                           source(s, 1, 0)["freshness"] == "stale")
            assert source(stale, 1, 0)["current"] == 0
            assert source(stale, 1, 0)["high"] == int(index == 1)
            pg.start()
            expected.extend(values[-4:])
            done = wait_p(d, lambda s: caught_up(s) and s["readings_written"] == len(expected))
            assert done["run_id"] == rid and readings(pg, rid) == expected
            assert done["outages"] == initial["outages"] + index + 1
            durations.append(done["last_outage_ms"])
            assert done["outage_total_ms"] - initial["outage_total_ms"] == sum(durations)
            assert done["readings_queued"] == done["readings_written"] + done["readings_dropped"]
            assert pg.sql(f"SELECT sum(reading_drops),sum(event_drops) FROM vigil.checkpoints "
                          f"WHERE run_id='{rid}'") == f"{dropped}|0"
            assert_current(pg, rid, high=index == 1)
        d.stop()
        assert latest(d)["abandoned_readings"] == latest(d)["abandoned_events"] == 0
        checkpoints = json.loads(pg.sql(f"SELECT json_agg(x ORDER BY batch_id) FROM "
                                f"(SELECT * FROM vigil.checkpoints WHERE run_id='{rid}') x"))
        gaps = [c for c in checkpoints if c["kind"] == "gap"]
        assert len(gaps) == 1 and gaps[0]["reading_drops"] == 5
        health = gaps[0]["detail"]
        assert loss_bounds[0] <= health["first_drop_ms"] <= health["last_drop_ms"] <= loss_bounds[1]
        Path(os.environ["VIGIL_EVIDENCE_DIR"], "checkpoints.json").write_text(json.dumps(checkpoints, indent=2) + "\n")
        return dict(run_id=rid, cycles=3, arrivals=14, retained=9, dropped=5,
                    sql_values=expected, outage_durations_ms=durations,
                    first_drop_ms=health["first_drop_ms"], last_drop_ms=health["last_drop_ms"],
                    gap_markers=sum(c["kind"] == "gap" for c in checkpoints))


def permission_recovery(exe, pg):
    pg.sql("REVOKE EXECUTE ON FUNCTION vigil.write_batch(jsonb) FROM vigil")
    try:
        with daemon(exe, pg, capacity=8) as d:
            first = wait_p(d, lambda s: s["query_failures"] >= 1 and s["degraded"])
            assert first["last_error"] == "42501", first
            with bus() as raw:
                for counter in range(5):
                    send(raw, reading(1, 0, 2800 + counter, 0, counter))
            stale = d.wait(lambda s: s["valid_readings"] == 5 and
                           source(s, 1, 0)["freshness"] == "stale")
            assert source(stale, 1, 0)["high"] == 1 and source(stale, 1, 0)["current"] == 0
            pg.sql("GRANT EXECUTE ON FUNCTION vigil.write_batch(jsonb) TO vigil")
            done = wait_p(d, lambda s: caught_up(s) and s["readings_written"] == 5)
            assert readings(pg, done["run_id"]) == list(range(2800, 2805))
            assert done["readings_dropped"] == done["events_dropped"] == 0
            assert_current(pg, done["run_id"])
            d.stop()
            return dict(sqlstate=first["last_error"], retained=5, dropped=0,
                        outage_ms=done["last_outage_ms"])
    finally:
        pg.sql("GRANT EXECUTE ON FUNCTION vigil.write_batch(jsonb) TO vigil")


def maximum_snapshot(exe, pg):
    # Large observation pipe isolates serialization capacity from the documented
    # counted diagnostic-drop behavior of a small/undrained output pipe.
    d = Daemon(exe, config_text(pods=tuple(range(1, 17)), timeout=1500, status=100, rate=0),
               persistence=True, extra_args=("--queue-capacity", "64"), env=pg.env(), pipesize=1048576)
    try:
        wait_p(d, caught_up)
        with bus() as raw:
            for counter, hot in ((0, True), (2, False)):
                for pod in range(1, 17):
                    for frame in (reading(pod, 0, 2800 if hot else 2500, 0, counter),
                                  reading(pod, 1, int(hot), 0, counter + 1),
                                  heartbeat(pod, 1, 0, counter // 2)):
                        send(raw, frame)
                total = 48 if hot else 96
                snap = d.wait(lambda s: s["frames"] == total)
                assert snap["rejected"] == 0 and snap["valid_readings"] == total * 2 // 3
                assert all(source(snap, p, c)["current"] for p in range(1, 17) for c in (0, 1))
                done = wait_p(d, lambda s: caught_up(s) and s["readings_written"] == total * 2 // 3)
                alarms = current_alarms(pg, done["run_id"])
                assert len(alarms) == 193
                assert len({(a["pod"], a["channel"], a["detail"]["kind"]) for a in alarms}) == 193
                highs = [a for a in alarms if a["detail"]["kind"] == "high"]
                assert len(highs) == 32 and all(a["detail"]["active"] == hot for a in highs)
                assert done["readings_dropped"] == done["events_dropped"] == 0
                if hot:
                    d.wait(lambda s: s["frames"] == 48 and
                           all(source(s, p, c)["freshness"] == "stale" for p in range(1, 17) for c in (0, 1)))
        d.stop()
        assert latest(d)["abandoned_readings"] == latest(d)["abandoned_events"] == 0
        return dict(pods=16, readings=64, heartbeats=32, snapshot_entries=193,
                    distinct_high_alarms=32, dropped=0)
    finally:
        d.close()


def startup_validation(exe):
    cases = [(["--queue-capacity", "0"], {}), (["--queue-capacity", "65537"], {}),
             ([], {"VIGIL_DB_HOST": "localhost"}), ([], {"VIGIL_DB_HOST": "/tmp,localhost"}),
             ([], {"VIGIL_DB_PORT": "5432x"}), ([], {"VIGIL_DB_PORT": "0"}),
             ([], {"PGSERVICE": "vigil-audit"}), ([], {"PGHOSTADDR": "127.0.0.1"}),
             ([], {"WATCHDOG_USEC": "1000"}), ([], {"VIGIL_DB_USER": ""})]
    for args, values in cases:
        env = dict(os.environ, **values, PGPASSWORD="vigil-test-canary-no-real-secret")
        result = subprocess.run([exe, *args], env=env, capture_output=True, text=True, timeout=3)
        assert result.returncode == 2 and "ready " not in result.stderr, (args, values, result)
        assert "vigil-test-canary-no-real-secret" not in result.stdout + result.stderr
        assert "AddressSanitizer" not in result.stderr and "runtime error:" not in result.stderr
    return dict(invalid_startups=len(cases), expected_exit=2, credential_canary_logged=False)


def database_case(exe, action):
    pg = Postgres()
    try:
        return action(exe, pg)
    finally:
        Path(os.environ["VIGIL_EVIDENCE_DIR"], "postgres.log").write_text((pg.root / "postgres.log").read_text())
        pg.close()


def main():
    if sys.platform != "linux" or sys.flags.optimize:
        raise RuntimeError("real Linux required; assertions must remain enabled")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--daemon", default="build-release/vigild")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    exe = str(Path(args.daemon).resolve())
    evidence = Evidence(args.output)
    for name, expected, action in (
        ("rapid-restarts", "six rapid counter restarts reject 18 frames; full silence permits 12 stream resets/18 recoveries", rapid_restart),
        ("repeated-outages", "one run, capacity4, arrivals3/9/2: retain9, drop5 exactly once, three recoveries", lambda e: database_case(e, repeated_outages)),
        ("permission-recovery", "ready/monitoring despite SQLSTATE42501; restored grant backfills all five once", lambda e: database_case(e, permission_recovery)),
        ("maximum-snapshot", "16 pods, 193 unique alarm entries, 64 readings/32 heartbeats, high raises/clears; no JSON truncation", lambda e: database_case(e, maximum_snapshot)),
        ("startup-validation", "ten invalid queue/DB/watchdog configurations exit2 before readiness; no credential disclosure", startup_validation),
    ):
        evidence.run(name, expected, lambda action=action: action(exe))
    evidence.finish()


if __name__ == "__main__":
    main()
