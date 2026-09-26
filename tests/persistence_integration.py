#!/usr/bin/env python3
"""Real PostgreSQL + SocketCAN + systemd gate; expectations in CP3 verification.
Creates/stops/removes only its own disposable cluster. Never touches 16/main.
"""
import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import signal
import select
import threading
import socket
import subprocess
import sys
import tempfile
import time
from monitor_integration import Daemon, ROOT, bus, config_text, send, source
from simulator.protocol import reading, heartbeat


def run(args, **kwargs):
    return subprocess.run(args, check=True, text=True, capture_output=True, timeout=20, **kwargs)


class Postgres:
    def __init__(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="vigil-pg-")
        self.root = Path(self.tmp.name)
        self.data = self.root / "data"
        self.sock = self.root / "socket"
        self.sock.mkdir()
        self.bin = Path(run(["pg_config", "--bindir"]).stdout.strip())
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            self.port = s.getsockname()[1]
        self.running = False
        self.initialized = False
        try:
            run([str(self.bin / "initdb"), "-D", str(self.data), "-U", "vigil_test_owner",
                 "--auth=trust", "--no-instructions"])
            with (self.data / "postgresql.conf").open("a") as f:
                f.write(f"\nport={self.port}\nlisten_addresses='127.0.0.1'\n"
                        f"unix_socket_directories='{self.sock}'\nmax_connections=20\n"
                        "shared_buffers='16MB'\nfsync=on\nsynchronous_commit=on\n")
            self.start()
            self.sql("CREATE ROLE vigil LOGIN")
            self.sql("CREATE DATABASE vigil")
            self.sql((ROOT / "migrations/001_history.sql").read_text(), database="vigil")
            # Run schema initialization twice to prove reproducible/idempotent setup.
            self.sql((ROOT / "migrations/001_history.sql").read_text(), database="vigil")
            self.sql("REVOKE EXECUTE ON FUNCTION vigil.write_batch(jsonb) FROM PUBLIC;"
                     "GRANT USAGE ON SCHEMA vigil TO vigil;"
                     "GRANT SELECT,INSERT ON ALL TABLES IN SCHEMA vigil TO vigil;"
                     "GRANT EXECUTE ON FUNCTION vigil.write_batch(jsonb) TO vigil", database="vigil")
        except BaseException:
            self.close()
            raise

    def command(self, database="vigil"):
        return ["psql", "-X", "-qAt", "-v", "ON_ERROR_STOP=1", "-h", str(self.sock),
                "-p", str(self.port), "-U", "vigil_test_owner", "-d", database]

    def sql(self, text, database=None):
        db = database or ("vigil" if self.initialized else "postgres")
        result = run(self.command(db), input=text, env=dict(os.environ, PGOPTIONS="-c statement_timeout=2000"))
        if database == "vigil":
            self.initialized = True
        return result.stdout.strip()

    def start(self):
        run([str(self.bin / "pg_ctl"), "-D", str(self.data), "-l", str(self.root / "postgres.log"),
             "-w", "-t", "10", "start"])
        self.running = True

    def stop(self):
        if self.running:
            run([str(self.bin / "pg_ctl"), "-D", str(self.data), "-m", "fast", "-w", "stop"])
            self.running = False

    def close(self):
        self.stop()
        self.tmp.cleanup()

    def env(self):
        return dict(os.environ, VIGIL_DB_HOST=str(self.sock), VIGIL_DB_PORT=str(self.port),
                    VIGIL_DB_NAME="vigil", VIGIL_DB_USER="vigil")


def latest(d):
    return next((e for e in reversed(d.events) if e["event"] == "persistence"), {})


def caught_up(s):
    return (s["connected"] and not s["degraded"]
            and s["reading_depth"] + s["event_depth"] + s["in_flight"] == 0
            and s["persisted_revision"] == s["alarm_revision"])


def wait_p(d, predicate, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        d.pump(0.03)
        s = latest(d)
        if s and predicate(s):
            assert all(snap["rx_overflow"] == 0 for snap in d.snapshots)
            return s
    raise AssertionError(f"persistence deadline: {latest(d)}")


@contextmanager
def daemon(executable, pg, capacity=16, env=None):
    d = Daemon(executable, config_text(pods=(1,), timeout=500, status=200, rate=0),
               persistence=True, extra_args=("--queue-capacity", str(capacity)), env=env or pg.env())
    try:
        assert len(list(Path(f"/proc/{d.child.pid}/task").iterdir())) >= 2  # TSan adds a runtime thread
        yield d
    finally:
        d.close()


def inject(first, count, base=2800):
    with bus() as raw:
        for counter in range(first, first + count):
            send(raw, reading(1, 0, base + counter, 0, counter % 256))


def readings(pg, rid):
    return json.loads(pg.sql(f"SELECT coalesce(json_agg((detail->>'value')::int ORDER BY seq),'[]') "
                             f"FROM vigil.history WHERE run_id='{rid}' AND kind='reading'"))


def current_alarms(pg, rid):
    return json.loads(pg.sql(f"SELECT alarms FROM vigil.checkpoints WHERE run_id='{rid}' "
                             "AND alarms IS NOT NULL ORDER BY batch_id DESC LIMIT 1"))


def assert_current(pg, rid, high=True, communication=True):
    alarms = current_alarms(pg, rid)
    def active(pod, ch, kind):
        return next(a["detail"]["active"] for a in alarms
                    if a["pod"] == pod and a["channel"] == ch and a["detail"]["kind"] == kind)
    assert active(1, 0, "high") == high, alarms
    assert active(1, 0, "communication_lost") == communication, alarms
    assert not active(0, -3, "persistence_degraded"), alarms


def history_fields(executable, pg):
    """Useful typed history: reading, heartbeat, CRC failure, physical invalidity."""
    with daemon(executable, pg) as d:
        wait_p(d, caught_up)
        with bus() as raw:
            send(raw, reading(1, 0, 2200, 0, 0))
            send(raw, heartbeat(1, 1, 42, 0))
            can_id, payload = reading(1, 0, 2300, 0, 1)
            send(raw, (can_id, payload[:-1] + bytes([payload[-1] ^ 1])))
            send(raw, reading(1, 0, 9000, 0, 1))
        snap = d.wait(lambda s: s["frames"] == 4)
        assert source(snap, 1, 0)["value"] == 2200 and source(snap, 1, 0)["current"] == 0
        done = wait_p(d, lambda s: s["readings_written"] == 1 and caught_up(s))
        rows = json.loads(pg.sql("SELECT json_agg(x ORDER BY seq) FROM "
                          f"(SELECT * FROM vigil.history WHERE run_id='{done['run_id']}' "
                          "AND kind <> 'alarm') x"))
        assert [r["kind"] for r in rows] == ["reading", "heartbeat", "rejected", "rejected"]
        assert rows[0]["pod"] == 1 and rows[0]["channel"] == 0 and rows[0]["detail"]["valid"]
        assert rows[1]["detail"]["uptime_s"] == 42 and not rows[1]["detail"]["stale"]
        assert rows[2]["detail"]["reason"] == "crc" and not rows[2]["detail"]["decoded"]
        assert rows[3]["detail"]["reason"] == "range" and not rows[3]["detail"]["valid"]
        assert all(r["observed_at"] and r["monotonic_ms"] > 0 for r in rows)
        d.stop()
        print("PASS SQL history: identity/channel, reading/heartbeat, CRC/range reasons, validity, UTC/monotonic timestamps")


def outage(executable, pg, overflow):
    """Settled stopped DB: N+K keeps newest N, drops K; timeouts remain live."""
    cap, count = (4, 11) if overflow else (8, 5)
    with daemon(executable, pg, cap) as d:
        wait_p(d, lambda s: caught_up(s))
        inject(0, 2)
        baseline = wait_p(d, lambda s: s["readings_written"] == 2 and s["in_flight_readings"] == 0)
        rid = baseline["run_id"]
        assert readings(pg, rid) == [2800, 2801]
        pg.stop()
        wait_p(d, lambda s: not s["connected"] and s["connection_failures"] >= 1
                        and s["in_flight_readings"] == 0)
        base = 2800 if overflow else 2400
        inject(2, count, base=base)
        full = wait_p(d, lambda s: s["readings_queued"] == count + 2)
        expected_drop = max(0, count - cap)
        assert full["reading_depth"] == min(count, cap), full
        assert full["readings_dropped"] == expected_drop and full["degraded"] == 1, full
        stale = d.wait(lambda s: s["frames"] == count + 2 and s["stale_events"] >= 3
                       and source(s, 1, 0)["freshness"] == "stale")
        assert source(stale, 1, 0)["high"] == int(overflow)
        timeout_age = stale["now_ms"] - source(stale, 1, 0)["last_valid_ms"]
        assert 500 <= timeout_age <= 1000, timeout_age
        alarm_age = min(e["now_ms"] - source(stale, 1, 0)["last_valid_ms"]
                        for e in d.events if e["event"] == "alarm" and e["pod"] == 1
                        and e["channel"] == 0 and e["kind"] == "communication_lost"
                        and e["state"] == "active" and e["now_ms"] >= source(stale, 1, 0)["last_valid_ms"])
        assert 500 <= alarm_age <= 720, alarm_age
        pg.start()
        done = wait_p(d, lambda s: s["readings_written"] == 2 + min(count, cap)
                      and caught_up(s))
        assert readings(pg, rid) == [2800, 2801] + list(range(base + 2 + expected_drop, base + 2 + count))
        gaps = pg.sql(f"SELECT coalesce(sum(reading_drops),0),coalesce(sum(event_drops),0) "
                      f"FROM vigil.checkpoints WHERE run_id='{rid}'")
        assert gaps == f"{expected_drop}|0", gaps
        assert (done["gap_markers"] > 0) == overflow, done
        assert_current(pg, rid, high=overflow)
        times = pg.sql(f"SELECT (detail->>'outage_ended_ms')::bigint-"
                       f"(detail->>'outage_started_ms')::bigint FROM vigil.checkpoints "
                       f"WHERE run_id='{rid}' ORDER BY batch_id DESC LIMIT 1")
        assert int(times) == done["last_outage_ms"], (times, done)
        lifecycle = pg.sql(f"SELECT count(*) FROM vigil.history WHERE run_id='{rid}' "
                          "AND kind='alarm' AND detail->>'kind'='persistence_degraded'")
        assert int(lifecycle) >= 2
        d.stop()
        final = latest(d)
        assert final["abandoned_readings"] == final["abandoned_events"] == 0, final
        print(f"PASS {'overflow' if overflow else 'backfill'}: capacity={cap} outage_readings={count} "
              f"dropped={expected_drop} retained={min(count,cap)} alarm_age_ms={alarm_age} "
              f"outage_ms={done['last_outage_ms']} reconnects={done['reconnects']} gaps={done['gap_markers']}")


def slow_query(executable, pg):
    """An actual table lock times out server-side while CAN and timers continue."""
    with daemon(executable, pg, 16) as d:
        wait_p(d, lambda s: caught_up(s))
        locker = subprocess.Popen(pg.command(), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True,
                                  env=dict(os.environ, PGAPPNAME="vigil-test-lock"))
        locker.stdin.write("BEGIN; LOCK TABLE vigil.history IN ACCESS EXCLUSIVE MODE; SELECT pg_sleep(30);\n")
        locker.stdin.close()
        try:
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                if pg.sql("SELECT count(*) FROM pg_locks l JOIN pg_stat_activity a USING(pid) "
                          "WHERE a.application_name='vigil-test-lock' AND l.mode='AccessExclusiveLock' AND granted") == "1":
                    break
                d.pump(0.03)
            else:
                raise AssertionError("lock acquisition timeout")
            inject(0, 8)
            failed = wait_p(d, lambda s: s["query_failures"] >= 1)
            snap = d.wait(lambda s: s["valid_readings"] == 8 and s["stale_events"] == 3)
            assert source(snap, 1, 0)["current"] == 0
            assert failed["readings_dropped"] == 0 and str(failed["last_error"]) == "55P03"
        finally:
            pg.sql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE application_name='vigil-test-lock'")
            locker.wait(timeout=3)
            locker.stdout.close()
            locker.stderr.close()
        done = wait_p(d, lambda s: s["readings_written"] == 8 and caught_up(s))
        assert readings(pg, done["run_id"]) == list(range(2800, 2808))
        d.stop()
        print(f"PASS locked PostgreSQL: query_failures={done['query_failures']}, 8 readings backfilled, 3 live timeouts")


class LostResponseProxy:
    """Forward real PostgreSQL bytes, dropping exactly one committed reading response."""
    def __init__(self, pg):
        self.target = ("127.0.0.1", pg.port)
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(4)
        self.port = self.listener.getsockname()[1]
        self.stop = threading.Event()
        self.dropped = threading.Event()
        self.error = None
        self.thread = threading.Thread(target=self.forward)
        self.thread.start()

    def forward(self):
        try:
            while not self.stop.is_set():
                if not select.select([self.listener], [], [], 0.05)[0]:
                    continue
                client, _ = self.listener.accept()
                with client, socket.create_connection(self.target, timeout=2) as server:
                    request = b""
                    response = b""
                    discard = False
                    while not self.stop.is_set():
                        ready = select.select([client, server], [], [], 0.05)[0]
                        closed = False
                        for src in ready:
                            data = src.recv(262144)
                            if not data:
                                closed = True
                                break
                            if src is client:
                                request = (request + data)[-262144:]
                                if not self.dropped.is_set() and b'"kind":"reading"' in request:
                                    discard = True
                                server.sendall(data)
                            elif discard:
                                response += data
                                # ReadyForQuery idle follows COMMIT of the single statement.
                                if b"Z\x00\x00\x00\x05I" in response:
                                    self.dropped.set()
                                    closed = True
                                    break
                            else:
                                client.sendall(data)
                        if closed:
                            break
        except BaseException as error:
            if not self.stop.is_set():
                self.error = error

    def close(self):
        self.stop.set()
        self.thread.join(timeout=3)
        self.listener.close()
        assert not self.thread.is_alive()
        assert self.error is None, self.error


def lost_response(executable, pg):
    """PostgreSQL commits; client loses acknowledgement; retry writes no duplicate rows."""
    proxy = LostResponseProxy(pg)
    try:
        env = dict(pg.env(), VIGIL_DB_HOST="127.0.0.1", VIGIL_DB_PORT=str(proxy.port), PGSSLMODE="disable")
        with daemon(executable, pg, env=env) as d:
            wait_p(d, lambda s: caught_up(s))
            inject(0, 5)
            failed = wait_p(d, lambda s: s["query_failures"] >= 1)
            assert proxy.dropped.is_set()
            assert len(readings(pg, failed["run_id"])) > 0  # committed despite missing ack
            done = wait_p(d, lambda s: s["readings_written"] == 5 and caught_up(s))
            assert readings(pg, done["run_id"]) == list(range(2800, 2805))
            assert done["readings_dropped"] == 0
            d.stop()
        print("PASS lost COMMIT response: real DB committed, same batch retried, exactly 5 rows and 5 acknowledged readings")
    finally:
        proxy.close()


def statement_timeout(executable, pg):
    """A real sleeping server trigger is cancelled at the configured 1000 ms."""
    with daemon(executable, pg) as d:
        wait_p(d, lambda s: caught_up(s))
        pg.sql("CREATE FUNCTION vigil.test_sleep() RETURNS trigger LANGUAGE plpgsql AS $$ "
               "BEGIN RAISE NOTICE 'test notice ignored'; PERFORM pg_sleep(5); RETURN NEW; END $$;"
               "CREATE TRIGGER test_slow BEFORE INSERT ON vigil.history "
               "FOR EACH ROW EXECUTE FUNCTION vigil.test_sleep()")
        try:
            inject(0, 3)
            failed = wait_p(d, lambda s: s["query_failures"] >= 1)
            assert str(failed["last_error"]) == "57014", failed
            d.wait(lambda s: s["valid_readings"] == 3 and s["stale_events"] == 3)
        finally:
            pg.sql("DROP TRIGGER test_slow ON vigil.history; DROP FUNCTION vigil.test_sleep()")
        done = wait_p(d, lambda s: s["readings_written"] == 3 and caught_up(s))
        assert readings(pg, done["run_id"]) == [2800, 2801, 2802]
        d.stop()
        print("PASS server statement_timeout: SQLSTATE=57014, monitoring/timeouts continued, 3 rows backfilled")


def event_overflow(executable, pg):
    """Reserved event lane remains bounded; lost alarm history gets a gap + current state."""
    with daemon(executable, pg, capacity=4) as d:
        wait_p(d, caught_up)
        pg.stop()
        wait_p(d, lambda s: s["connection_failures"] >= 1 and not s["connected"])
        with bus() as raw:
            for i in range(300):
                send(raw, reading(1, 1, i % 2, 0, i % 256))
                if i % 20 == 19:
                    # Capacity test, not a CAN load benchmark: wait for each
                    # bounded burst so sanitizer overhead cannot overflow RX.
                    d.wait(lambda s: s["valid_readings"] >= i + 1)
        full = wait_p(d, lambda s: s["readings_queued"] == 300)
        assert full["readings_dropped"] == 296 and full["event_depth"] == 256, full
        assert full["events_dropped"] > 0 and full["degraded"] == 1, full
        assert full["readings_queued"] == full["readings_written"] + full["readings_dropped"] + full["reading_depth"] + full["in_flight_readings"]
        pg.start()
        done = wait_p(d, lambda s: caught_up(s))
        rid = done["run_id"]
        lost = pg.sql(f"SELECT sum(reading_drops),sum(event_drops) FROM vigil.checkpoints WHERE run_id='{rid}'")
        assert lost == f"296|{done['events_dropped']}", (lost, done)
        alarms = current_alarms(pg, rid)
        assert any(a["pod"] == 1 and a["channel"] == 1 and a["detail"]["kind"] == "high"
                   and a["detail"]["active"] for a in alarms), alarms
        d.stop()
        print(f"PASS event overflow: 300 readings, reading_drops=296, exact event_drops={done['events_dropped']}, current door alarm recovered")


def blackhole(executable, pg):
    """A TCP listener that never speaks PostgreSQL cannot hang shutdown."""
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        sock.listen(10)
        env = dict(pg.env(), VIGIL_DB_HOST="127.0.0.1", VIGIL_DB_PORT=str(sock.getsockname()[1]))
        with daemon(executable, pg, env=env) as d:
            inject(0, 3)
            d.wait(lambda s: s["valid_readings"] == 3 and s["stale_events"] == 3)
            failed = wait_p(d, lambda s: s["connection_failures"] >= 1)
            assert failed["connection_failures"] <= 2, failed
            start = time.monotonic()
            d.stop()
            elapsed = time.monotonic() - start
            assert elapsed < 1.5, elapsed
            assert latest(d)["abandoned_readings"] == 3
            print(f"PASS unresponsive TCP: connect deadline, live monitoring, shutdown_s={elapsed:.3f}, abandoned=3")


def crash(executable, pg):
    """Only committed pre-outage readings survive SIGKILL, identified by run UUID."""
    with daemon(executable, pg) as d:
        wait_p(d, lambda s: caught_up(s))
        inject(0, 2)
        baseline = wait_p(d, lambda s: s["readings_written"] == 2 and s["in_flight_readings"] == 0)
        rid = baseline["run_id"]
        pg.stop()
        wait_p(d, lambda s: not s["connected"] and s["connection_failures"] >= 1 and s["in_flight_readings"] == 0)
        inject(2, 3)
        pending = wait_p(d, lambda s: s["reading_depth"] == 3)
        assert pending["readings_dropped"] == 0 and pending["readings_written"] == 2
        d.child.kill()
        assert d.child.wait(timeout=2) == -signal.SIGKILL
    pg.start()
    assert readings(pg, rid) == [2800, 2801]
    with daemon(executable, pg) as d:
        new = wait_p(d, lambda s: caught_up(s))
        assert new["run_id"] != rid and new["readings_queued"] == 0
        assert readings(pg, new["run_id"]) == []
        d.stop()
    print("PASS SIGKILL outage: exactly 3 known unpersisted readings lost; 2 prior rows remain; new run has no invented backfill")


def systemd_gate(executable, pg):
    """Exercise actual unit sandbox/User/notify/watchdog/restart on Linux PID 1."""
    unit = "vigil-checkpoint3.service"
    dest = Path("/usr/local/libexec/vigil-checkpoint3")
    assert not dest.exists(), "test service directory already exists"
    unit_path = Path("/run/systemd/system") / unit
    assert not unit_path.exists(), "test unit already exists"
    run(["sudo", "install", "-d", "-m", "0755", str(dest)])
    try:
        if subprocess.run(["id", "vigil"], capture_output=True).returncode != 0:
            run(["sudo", "useradd", "--system", "--home-dir", "/nonexistent", "--shell", "/usr/sbin/nologin", "vigil"])
        run(["sudo", "install", "-m", "0755", executable, str(dest / "vigild")])
        with tempfile.TemporaryDirectory(prefix="vigil-unit-") as directory:
            conf = Path(directory) / "vigil.conf"
            conf.write_text(config_text(pods=(1,), timeout=500, status=200, rate=0))
            run(["sudo", "install", "-m", "0644", str(conf), str(dest / "vigil.conf")])
            text = (ROOT / "systemd/vigild.service").read_text()
            text = text.replace("/usr/local/bin/vigild --config /etc/vigil/vigil.conf",
                                f"{dest}/vigild --config {dest}/vigil.conf")
            text = text.replace("EnvironmentFile=-/etc/vigil/database.env",
                                f"Environment=VIGIL_DB_HOST=127.0.0.1 VIGIL_DB_PORT={pg.port} "
                                "VIGIL_DB_NAME=vigil VIGIL_DB_USER=vigil")
            temp = Path(directory) / unit
            temp.write_text(text)
            run(["sudo", "install", "-m", "0644", str(temp), str(unit_path)])
        run(["sudo", "systemctl", "daemon-reload"])
        run(["sudo", "systemctl", "start", unit])
        def prop(name):
            return run(["systemctl", "show", unit, f"--property={name}", "--value"]).stdout.strip()
        assert prop("ActiveState") == "active" and prop("SubState") == "running"
        pid = int(prop("MainPID"))
        uid = run(["id", "-u", "vigil"]).stdout.strip()
        assert f"Uid:\t{uid}\t{uid}\t{uid}\t{uid}" in Path(f"/proc/{pid}/status").read_text()
        assert int(prop("WatchdogTimestampMonotonic")) > 0
        # Enough time to exceed one watchdog interval: healthy main keeps it alive.
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            time.sleep(0.2)
            assert int(prop("MainPID")) == pid and prop("SubState") == "running"
        pg.stop()
        run(["sudo", "systemctl", "kill", "--kill-who=main", "--signal=SIGKILL", unit])
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            if prop("SubState") == "running" and int(prop("MainPID")) not in (0, pid):
                break
            time.sleep(0.1)
        else:
            raise AssertionError("systemd did not restart after SIGKILL")
        pid2 = int(prop("MainPID"))
        run(["sudo", "systemctl", "kill", "--kill-who=main", "--signal=SIGSTOP", unit])
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            if prop("SubState") == "running" and int(prop("MainPID")) not in (0, pid2):
                break
            time.sleep(0.1)
        else:
            raise AssertionError("watchdog did not restart stopped main")
        restarts = int(prop("NRestarts"))
        assert restarts >= 2
        start = time.monotonic()
        run(["sudo", "systemctl", "stop", unit])
        elapsed = time.monotonic() - start
        assert prop("ActiveState") == "inactive" and elapsed < 2
        journal = run(["sudo", "journalctl", "-u", unit, "--no-pager", "-n", "200"]).stdout
        assert "Watchdog timeout" in journal
        pg.start()
        print(f"PASS systemd: unprivileged uid={uid}, Type=notify ready, healthy watchdog, "
              f"SIGKILL + watchdog restarts={restarts}, outage_stop_s={elapsed:.3f}")
    finally:
        subprocess.run(["sudo", "systemctl", "stop", unit], capture_output=True, timeout=10)
        run(["sudo", "rm", "-f", str(unit_path)])
        run(["sudo", "rm", "-rf", str(dest)])
        run(["sudo", "systemctl", "daemon-reload"])
        subprocess.run(["sudo", "systemctl", "reset-failed", unit], capture_output=True)
        if not pg.running:
            pg.start()


def main():
    if sys.platform != "linux" or sys.flags.optimize:
        raise RuntimeError("real Linux required and assertions must remain enabled")
    parser = argparse.ArgumentParser()
    parser.add_argument("--daemon", required=True)
    parser.add_argument("--systemd", action="store_true")
    args = parser.parse_args()
    executable = str(Path(args.daemon).resolve())
    pg = Postgres()
    try:
        history_fields(executable, pg)
        outage(executable, pg, False)
        outage(executable, pg, True)
        slow_query(executable, pg)
        statement_timeout(executable, pg)
        lost_response(executable, pg)
        event_overflow(executable, pg)
        blackhole(executable, pg)
        crash(executable, pg)
        if args.systemd:
            systemd_gate(executable, pg)
        print("PASS Checkpoint 3 integration:", executable)
    except BaseException:
        print((pg.root / "postgres.log").read_text()[-4000:], file=sys.stderr)
        raise
    finally:
        pg.close()


if __name__ == "__main__":
    main()
