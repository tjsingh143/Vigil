#!/usr/bin/env python3
"""An asserted, narrated interview story. Uses only its own disposable database."""
from verification import require  # also refuses optimized execution before fixture imports
import argparse
import json
import os
from pathlib import Path
import sys
from evidence import Evidence, utc
from monitor_integration import bus, send, source, pod_status
from persistence_integration import Postgres, daemon, wait_p, caught_up, readings, latest, current_alarms
from simulator.pod import Pod
from simulator.protocol import reading


def say(text):
    print(f"[{utc()}] {text}", flush=True)


def view(label, snap):
    sensor = source(snap, 1, 0)
    say(f"{label}: mono_ms={snap['now_ms']} frames={snap['frames']} "
        f"accepted={snap['valid_readings']} rejected={snap['rejected']} "
        f"temperature={sensor['value']/100:.2f}C freshness={sensor['freshness']} "
        f"current={sensor['current']} high={sensor['high']} last_valid_ms={sensor['last_valid_ms']}")


def story(executable):
    pg = Postgres()
    directory = Path(os.environ["VIGIL_EVIDENCE_DIR"])
    try:
        with daemon(executable, pg, capacity=4) as d, bus() as raw:
            wait_p(d, caught_up)
            say("1. HEALTHY: simulated temperature, door and heartbeat arrive on real vcan0.")
            pod = Pod(1)
            for tick in range(3):
                for frame in pod.cycle(tick, 0):
                    send(raw, frame)
                healthy = d.wait(lambda s: s['frames'] == 3 * (tick + 1))
                view("healthy", healthy)
            require((all(source(healthy, 1, c)['current'] for c in (0, 1))))
            say("2. POD STOPS: leave the bus silent; timeout=500 ms, tick=20 ms.")
            stale = d.wait(lambda s: s['now_ms'] > healthy['now_ms'] and
                           all(source(s, 1, c)['freshness'] == 'stale' for c in (0, 1))
                           and pod_status(s, 1)['heartbeat'] == 'stale')
            require((source(stale, 1, 0)['current'] == 0))
            alarm = next(e for e in d.events if e['event'] == 'alarm'
                         and e['kind'] == 'communication_lost' and e['pod'] == 1
                         and e['channel'] == 0 and e['state'] == 'active'
                         and e['now_ms'] > healthy['now_ms'])
            age = alarm['now_ms'] - source(stale, 1, 0)['last_valid_ms']
            require((500 <= age <= 720))
            view("stale", stale)
            say(f"communication_lost raised_ms={alarm['now_ms']} detection_age_ms={age}; "
                "last value is retained with current=0.")
            say("3. POD RETURNS: restart counters after silence; fresh data clears communication loss.")
            for frame in Pod(1).cycle(0, 0):
                send(raw, frame)
            returned = d.wait(lambda s: s['frames'] == 12 and s['recoveries'] >= 3)
            require((all(source(returned, 1, c)['current'] for c in (0, 1))))
            view("returned", returned)
            before = wait_p(d, lambda p: p['readings_written'] == 8 and caught_up(p))
            rid = before['run_id']
            require((before['gap_markers'] == 0))
            prior_values = readings(pg, rid)
            require((prior_values == [2200, 0, 2201, 0, 2202, 0, 2200, 0]))
            say(f"SQL before outage: run={rid} readings=8 gaps=0.")
            say("4. DATABASE STOPS: stop only this disposable PostgreSQL cluster; monitoring stays running.")
            pg.stop()
            failed = wait_p(d, lambda p: not p['connected'] and p['connection_failures'] >= 1
                            and p['in_flight_readings'] == 0 and p['degraded'])
            say(f"persistence_degraded=1 observed_ms={failed['now_ms']} "
                f"connection_failures={failed['connection_failures']} capacity=4 in_flight_readings=0.")
            say("5. ELEVEN NEW READINGS: main raises the high alarm while the database is down.")
            for counter in range(2, 13):
                send(raw, reading(1, 0, 2800 + counter, 0, counter))
                live = d.wait(lambda s: s['frames'] == 13 + counter - 2)
                require((source(live, 1, 0)['current'] == 1 and source(live, 1, 0)['high'] == 1))
                if counter in (2, 5, 12):
                    view("database down, monitoring live", live)
            queued = wait_p(d, lambda p: p['readings_queued'] == 19)
            require((queued['reading_depth'] == 4 and queued['readings_dropped'] == 7))
            require((queued['in_flight_readings'] == 0 and queued['degraded'] == 1))
            say(f"Queue observed_ms={queued['now_ms']}: total_queued=19 already_written=8 "
                "pending=4 dropped=7 in_flight_readings=0. Newest four retained.")
            down_stale = d.wait(lambda s: s['frames'] == 23 and source(s, 1, 0)['freshness'] == 'stale')
            require((source(down_stale, 1, 0)['current'] == 0 and source(down_stale, 1, 0)['high'] == 1))
            view("timeout still works during database outage", down_stale)
            say("6. DATABASE RETURNS: backfill the four retained readings and record the seven-record gap.")
            pg.start()
            done = wait_p(d, lambda p: p['readings_written'] == 12 and caught_up(p))
            sql_values = readings(pg, rid)
            require((sql_values == prior_values + [2809, 2810, 2811, 2812]), lambda: (sql_values))
            gaps = json.loads(pg.sql(f"SELECT json_agg(x ORDER BY batch_id) FROM "
                                    f"(SELECT * FROM vigil.checkpoints WHERE run_id='{rid}' AND kind='gap') x"))
            require((sum(g['reading_drops'] for g in gaps) == 7))
            require((sum(g['event_drops'] for g in gaps) == 0))
            alarms = current_alarms(pg, rid)
            high = next(a for a in alarms if a['pod'] == 1 and a['channel'] == 0 and a['detail']['kind'] == 'high')
            require((high['detail']['active']))
            say(f"RECOVERED observed_ms={done['now_ms']} outage_ms={done['last_outage_ms']} "
                f"written=12 dropped=7 pending=0 gaps={done['gap_markers']} degraded=0.")
            say(f"SQL retained outage values={sql_values[-4:]}; gap reading_drops=7 event_drops=0; "
                "high alarm remains active until a cooler valid reading arrives.")
            d.stop()
            final = latest(d)
            require((final['abandoned_readings'] == final['abandoned_events'] == 0))
            for name, query in (
                ('history', f"SELECT * FROM vigil.history WHERE run_id='{rid}' ORDER BY seq"),
                ('checkpoints', f"SELECT * FROM vigil.checkpoints WHERE run_id='{rid}' ORDER BY batch_id")):
                data = pg.sql(f"SELECT coalesce(json_agg(x),'[]') FROM ({query}) x")
                (directory / f'{name}.json').write_text(json.dumps(json.loads(data), indent=2) + '\n')
            say("Stopped cleanly: no abandoned records. SQL history, gaps and daemon logs are saved with this run.")
            return dict(run_id=rid, detection_age_ms=age, last_valid_ms=source(stale, 1, 0)['last_valid_ms'],
                        alarm_ms=alarm['now_ms'], before_outage_readings=8, outage_arrivals=11,
                        retained=4, dropped=7, total_written=12, gap_markers=done['gap_markers'],
                        outage_ms=done['last_outage_ms'], sql_values=sql_values, final=final)
    finally:
        (directory / 'postgres.log').write_text((pg.root / 'postgres.log').read_text())
        pg.close()


def main():
    if sys.platform != 'linux' or sys.flags.optimize:
        raise RuntimeError('real Linux required; assertions must remain enabled')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--daemon', default='build/vigild')
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    evidence = Evidence(args.output)
    evidence.run('interview', 'healthy -> silent/stale -> returned; database down -> live alarms; '
                 'capacity four, eleven arrivals -> seven drops; recovery writes four and an exact gap; clean shutdown',
                 lambda: story(str(Path(args.daemon).resolve())))
    evidence.finish()


if __name__ == '__main__':
    main()
