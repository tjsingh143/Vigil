#!/usr/bin/env python3
"""Narrated production-binary demo on disposable native SQLite and PostgreSQL."""
from verification import require  # also refuses optimized execution before fixture imports
import argparse
import json
import os
import signal
from pathlib import Path
import sys
import tempfile
from evidence import Evidence, utc
from monitor_integration import bus, send, source
from persistence_integration import wait_p, caught_up, latest, current_alarms
from resilience_database_contract import ReplayPostgres
from resilience_integration import create, daemon, inventory, wait_local
from resilience_replay import Withhold, wait, kill, assert_remote
from simulator.pod import Pod
from simulator.protocol import reading


def say(message):
    print(f'[{utc()}] {message}', flush=True)


def same_rows(old, new):
    rows={r['order_id']:r['payload'] for r in new['rows']}
    require((all(rows.get(r['order_id'])==r['payload'] for r in old['rows'])))


def story(build, output):
    pg=ReplayPostgres()
    proxy=None
    try:
        with tempfile.TemporaryDirectory(prefix='vigil-rc-demo-') as tmp:
            root=Path(tmp)
            path=create(root/'state',build/'vigil-outbox-init')
            exe=build/'vigild'
            with daemon(exe,path,env=pg.env(),capacity=1024,pipesize=1024*1024) as d, bus() as raw:
                wait_p(d,caught_up)
                say('1. HEALTHY: temperature, door and heartbeat on real Linux vcan0.')
                pod=Pod(1)
                for tick in range(3):
                    for frame in pod.cycle(tick,0): send(raw,frame)
                    healthy=d.wait(lambda s:s['valid_readings']==2*(tick+1))
                require((all(source(healthy,1,c)['current'] for c in (0,1))))
                wait_local(d,lambda s:s['local_readings']==6)
                wait_p(d,lambda p:p['readings_written']==6 and caught_up(p))
                say('2. SQLite accepted six readings; remote retirement confirmed. Queue publication alone is volatile.')
                say('3. STOP PostgreSQL: only the fixture cluster is stopped.')
                pg.stop()
                wait_p(d,lambda p:not p['connected'] and p['degraded'])
                say('4. Monitoring continues: twelve high readings accumulate as durable local backlog.')
                for n in range(6,18):
                    send(raw,reading(1,0,2800+n,0,n))
                    d.wait(lambda s:s['valid_readings']==n+1)
                wait_local(d,lambda s:s['local_readings']==18)
                stale=d.wait(lambda s:s['valid_readings']==18 and source(s,1,0)['freshness']=='stale')
                require((source(stale,1,0)['current']==0 and source(stale,1,0)['high']==1))
                before=wait_p(d,lambda p:p['reading_depth']+p['event_depth']+p['in_flight']==0)
                require((before['readings_dropped']==before['events_dropped']==0))
                say('5. SIGKILL the daemon after local completion; no shutdown handler runs.')
                kill(d)
            committed=inventory(path,output/'after-kill')
            require((committed['meta']['admitted_readings']==18))
            require((sum(r['reading_count'] for r in committed['rows'])==12))
            say('6. RESTART while PostgreSQL remains stopped; current measurements start unknown.')
            with daemon(exe,path,env=pg.env(),pipesize=1024*1024) as d:
                unknown=d.wait(lambda s:s['frames']==0)
                require((source(unknown,1,0)['has_value']==0))
                wait_local(d,lambda s:s['durable_pending']>=committed['meta']['pending_records'])
                # Stop for supported offline inspection, then reopen the same spool.
                d.stop()
            recovered=inventory(path,output/'restarted-offline-audit')
            same_rows(committed,recovered)
            say('7. Offline restarted-store audit: all original unretired bytes/identities still present. Restart preserved 18 total local readings.')
            say('8. RESTORE PostgreSQL, routing this demo through a response-withholding proxy.')
            pg.start()
            proxy=Withhold(pg)
            head=recovered['rows'][0]
            say('9. Replay an old-run head; observer confirms its PostgreSQL receipt while the client receives no success.')
            with daemon(exe,path,env=proxy.env(),pipesize=1024*1024) as d:
                wait(d,lambda:proxy.count()>=2,timeout=15)
                require((all(p==head['payload'].encode() for p in proxy.requests)))
                require((pg.sql(f"SELECT encode(payload,'hex') FROM vigil.replay_receipts WHERE run_id='{head['run_id']}' AND batch_id={head['batch_id']}")==head['payload'].encode().hex()))
                ambiguous=wait_local(d,lambda s:s['remote_state']=='unknown')
                say(f'10. AMBIGUOUS COMMIT: {proxy.count()} identical submissions, one receipt/history effect; local head retained. SIGKILL again.')
                kill(d)
            ambiguous_store=inventory(path,output/'ambiguous-kill')
            same_rows(recovered,ambiguous_store)
            attempts=proxy.count()
            proxy.close(); proxy=None
            with daemon(exe,path,env=pg.env(),pipesize=1024*1024) as d, bus() as raw:
                done=wait_p(d,caught_up,timeout=40)
                rid=done['run_id']
                require((source(d.wait(lambda s:s['frames']==0),1,0)['has_value']==0))
                # New current evidence, not old replay, raises then clears high.
                send(raw,reading(1,0,2800,0,0))
                live=d.wait(lambda s:s['valid_readings']==1)
                require((source(live,1,0)['high']==1))
                send(raw,reading(1,0,2200,0,1))
                fresh=d.wait(lambda s:s['valid_readings']==2)
                require((source(fresh,1,0)['current']==1 and source(fresh,1,0)['high']==0))
                wait_p(d,lambda p:p['readings_written']==2 and caught_up(p),timeout=20)
                alarms=current_alarms(pg,rid)
                require((not next(a['detail']['active'] for a in alarms if a['pod']==1 and a['channel']==0 and a['detail']['kind']=='high')))
                say('11. Current high alarm raised/cleared by new readings; old history and snapshots remain scoped to original runs.')
                d.stop(); final=latest(d)
                require((final['abandoned_readings']==final['abandoned_events']==0))
            inv=inventory(path,output/'final')
            exact=assert_remote(pg,inv,recovered['rows'])
            require((exact['readings']==20))
            require((pg.sql('SELECT coalesce(sum(reading_drops+event_drops),0) FROM vigil.checkpoints')=='0'))
            # Full receipts/history are retained in evidence, not printed inline.
            (output/'remote.json').write_text(json.dumps(exact['remote'],indent=2)+'\n')
            say(f"12. FINAL: 20/20 offered readings remote, {exact['history']} history records, {exact['receipts']} receipts; pending records/batches/bytes=0, known reading/event losses=0, final volatile abandonment=0.")
            say('All offered demo readings reconciled. Arbitrary unsaved crash-time event/counter loss is not reconstructable; SIGKILL is not a power cut.')
            return dict(offered_readings=20,remote_readings=exact['readings'],history=exact['history'],receipts=exact['receipts'],
                        ambiguous_identical_submissions=attempts,pending_records=0,pending_batches=0,pending_bytes=0,
                        reading_losses=0,event_gap_losses=0,final=final,ambiguous_status=ambiguous,
                        local=inv['meta'],power_cut_qualified=False)
    finally:
        try:
            if proxy: proxy.close()
        finally:
            try:
                (output/'postgres.log').write_text((pg.root/'postgres.log').read_text())
            finally:
                pg.close()


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build',type=Path,default=Path('build-resilience-release'))
    ap.add_argument('--output',type=Path,required=True)
    args=ap.parse_args()
    require((sys.platform=='linux' and not sys.flags.optimize))
    def terminate(signum, _frame):
        raise SystemExit(128+signum)
    signal.signal(signal.SIGTERM,terminate)
    for k in list(os.environ):
        if k.startswith(('PG','VIGIL_DB_','VIGIL_TEST_')): os.environ.pop(k)
    evidence=Evidence(args.output)
    evidence.run('resilience-interview','production binary: healthy -> PG outage -> local durable backlog -> SIGKILL -> restart before PG -> exact old bytes -> PG restore -> ambiguous duplicate retry -> current alarms -> exact zero-pending reconciliation',
                 lambda:story(args.build.resolve(),evidence.directory/'resilience-interview'))
    evidence.finish()


if __name__=='__main__': main()
