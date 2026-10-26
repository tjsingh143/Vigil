#!/usr/bin/env python3
"""F1/F2 production-coordinator schedules on real Linux SQLite/PG and SocketCAN.

Independent worker pipes force remote A confirmation to overlap uncertain B.
Only offline copies of stopped/frozen processes are queried. No supplied targets.
"""
from verification import require
import argparse
import hashlib
import json
import os
from pathlib import Path
import select
import signal
import tempfile
import time
from evidence import Evidence
from monitor_integration import bus, send, source
from persistence_integration import wait_p, caught_up
from resilience_integration import Hook, create, daemon, inventory
from resilience_adversarial import cluster, oracle
from resilience_replay import seed, kill, finish, assert_remote, wait
from simulator.protocol import reading


class Hooks:
    def __init__(self, pg, selections):
        self.env = pg.env()
        self.items = {}
        self.pass_fds = ()
        self.tokens = []
        require(len(selections) <= 8)
        for index, (name, hits) in enumerate(selections.items(), 1):
            h = Hook(name, hit=1, repeat=hits)
            self.items[name] = h
            self.pass_fds += h.pass_fds
            for key, value in h.env.items():
                if key.startswith('VIGIL_TEST_'):
                    self.env[f'{key}_{index}'] = value

    def token(self, name, d):
        end = time.monotonic() + 8
        h = self.items[name]
        while not select.select([h.nr], [], [], 0)[0]:
            require(time.monotonic() < end, lambda: ('progress deadline', name, d.events[-8:]))
            d.pump(.01)
        token = h.token(.01)
        self.tokens.append(token)
        return token

    def release(self, name, command=b'D'):
        self.items[name].release(command)

    def close(self):
        for h in self.items.values(): h.close()


def frozen(d, path, output):
    """Freeze every daemon thread; copy DB/WAL, then resume the same PID."""
    d.child.send_signal(signal.SIGSTOP)
    try:
        end = time.monotonic() + 3
        while True:
            states = [p.read_text().rpartition(')')[2].split()[0]
                      for p in Path(f'/proc/{d.child.pid}/task').glob('*/stat')]
            if states and all(s in ('T', 't') for s in states): break
            require(time.monotonic() < end, lambda: ('freeze deadline', states))
            time.sleep(.001)
        return inventory(path, output)
    finally:
        d.child.send_signal(signal.SIGCONT)


def publication(d, inv):
    """Compare one fresh atomic status pair with the paused owner's actual DB."""
    marker = int(time.monotonic() * 1000) + 1
    d.child.send_signal(signal.SIGUSR1)
    def pair():
        for i, p in enumerate(d.events[:-1]):
            o = d.events[i+1]
            if p['event'] == 'persistence' and o['event'] == 'outbox' and p['now_ms'] >= marker:
                return p, o
        return None
    p, o = wait(d, pair, timeout=3)
    m = inv['meta']
    current = next(r for r in inv['runs'] if r['run_id'] == p['run_id'])
    require(o['local_state'] == 'ready')
    for status, stored in (('durable_pending','pending_records'), ('pending_batches','pending_batches'),
                           ('pending_bytes','pending_payload_bytes'), ('spool_admitted','admitted_records'),
                           ('spool_retired','retired_records')):
        require(o[status] == m[stored], lambda: (status, o, m))
    require(o['local_committed'] == current['admitted_records'])
    require(o['local_readings'] == current['admitted_readings'])
    require(p['records_written'] == current['retired_records'])
    require(p['readings_written'] == current['retired_readings'])
    pending = [r for r in inv['rows'] if r['run_id'] == p['run_id']]
    require(o['local_committed'] == p['records_written'] + sum(r['record_count'] for r in pending))
    require(o['local_readings'] == p['readings_written'] + sum(r['reading_count'] for r in pending))
    return dict(persistence=p, outbox=o)


def all_counters(d):
    pairs = 0
    for i, p in enumerate(d.events[:-1]):
        o = d.events[i+1]
        if p['event'] != 'persistence' or o['event'] != 'outbox': continue
        require(o['spool_admitted'] == o['spool_retired'] + o['durable_pending'])
        require(p['readings_queued'] + p['events_queued'] ==
                o['reading_evicted'] + o['event_evicted'] + p['reading_depth'] + p['event_depth'] +
                p['in_flight'] + o['local_committed'])
        require(p['records_written'] <= o['local_committed'])
        pairs += 1
    require(pairs > 0)
    return pairs


def overlap(build, root, fault, old=False, cut=None):
    exe, init = build/'vigild-resilience-test', build/'vigil-outbox-init'
    with cluster() as pg:
        if old:
            path, prior, _ = seed(exe, init, root/'old')
        else:
            path = create(root/'state', init)
            prior = None
        selections = {'after_take_before_begin':1 if old else 2,
                      'after_append_failed':1, 'after_remote_confirm_before_result':1,
                      'after_remote_result':1, 'after_append_reconciled':1,
                      'after_append_recovery_published':1,
                      'after_retire_commit_before_publish':1, 'after_retirement_published':1}
        hooks = Hooks(pg, selections)
        known = {} if prior is None else {r['order_id']:r['payload'] for r in prior['rows']}
        observations = []
        killed = False
        try:
            with daemon(exe, path, hooks, pipesize=1024*1024) as d, bus() as raw:
                pid = d.child.pid
                hooks.token('after_take_before_begin', d)
                # A's capture is already immutable. The reading belongs to B
                # (or queues behind the new-run capture when A is an old run).
                send(raw, reading(1,0,2200,0,0))
                d.wait(lambda s:s['valid_readings']==1)
                if not old:
                    hooks.release('after_take_before_begin', b'x')
                    hooks.token('after_take_before_begin', d)
                hooks.release('after_take_before_begin', fault.encode())
                hooks.token('after_append_failed', d)
                hooks.token('after_remote_confirm_before_result', d)
                before = frozen(d, path, root/'uncertain')
                a = before['rows'][0]
                b = before['rows'][-1]
                require(a['order_id'] < b['order_id'])
                require(any(e['kind']=='reading' for e in json.loads(b['payload'])['records']) or old)
                require(pg.sql(f"SELECT encode(payload,'hex') FROM vigil.replay_receipts WHERE run_id='{a['run_id']}' AND batch_id={a['batch_id']}") == a['payload'].encode().hex())
                known.update({r['order_id']:r['payload'] for r in before['rows']})
                # Both workers are held: B's append returned a real error, and
                # A has a validated real PG success. Publish RESULT first.
                hooks.release('after_remote_confirm_before_result')
                hooks.token('after_remote_result', d)
                hooks.release('after_remote_result')
                if cut == 'after_append_failed':
                    kill(d); killed=True
                else:
                    started = time.monotonic()
                    hooks.release('after_append_failed')
                    for point in ('after_append_reconciled', 'after_append_recovery_published',
                                  'after_retire_commit_before_publish', 'after_retirement_published'):
                        hooks.token(point, d)
                        inv = frozen(d, path, root/point)
                        oracle(pg, inv, known, root/(point+'-union.json'))
                        if point in ('after_append_recovery_published','after_retirement_published'):
                            observations.append(publication(d, inv))
                        if point == 'after_append_recovery_published':
                            require(inv['meta']['last_retired_order'] == a['order_id']-1)
                            require(next(r['payload'] for r in inv['rows'] if r['order_id']==b['order_id']) == b['payload'])
                        if point == 'after_retirement_published':
                            require(inv['meta']['last_retired_order'] == a['order_id'])
                            require(inv['meta']['retired_records'] == before['meta']['retired_records']+a['record_count'])
                        if cut == point:
                            kill(d); killed=True; break
                        hooks.release(point)
                    if not killed:
                        # No additional sensor traffic has arrived since B was
                        # queued. All original work must retire in this process.
                        done = wait_p(d, lambda s:caught_up(s) and s['readings_written']==1, timeout=12)
                        require(d.child.pid == pid and d.child.poll() is None)
                        elapsed = time.monotonic()-started
                        require(elapsed < 12)
                        # New admission is exercised only AFTER quiet recovery.
                        send(raw, reading(1,0,2201,0,1))
                        wait_p(d, lambda s:caught_up(s) and s['readings_written']==2, timeout=12)
                        d.stop()
                pairs = all_counters(d)
            final = inventory(path, root/'stopped')
            oracle(pg, final, known, root/'stopped-union.json')
            if killed:
                result = finish(exe, path, pg, root, [dict(run_id=json.loads(v)['run_id'], batch_id=json.loads(v)['batch_id'],payload=v) for v in known.values()])
            else:
                result = assert_remote(pg, final, [a,b])
                result.update(recovery_s=elapsed, same_pid=pid, incoming_before_recovery=1, incoming_after_recovery=1)
            (root/"final-remote.json").write_text(json.dumps(result.pop("remote"),indent=2))
            return dict(fault=fault, old_run=old, kill_cut=cut, tokens=hooks.tokens,
                        status_pairs=pairs, publications=observations, result=result)
        finally: hooks.close()


def retirement_nomem(build, root, fault, old=False, cut=None):
    exe, init = build/'vigild-resilience-test', build/'vigil-outbox-init'
    with cluster() as pg:
        if old: path, prior, _ = seed(exe,init,root/'old')
        else:
            path=create(root/'state',init)
            prior=None
        hooks=Hooks(pg, {'after_retire_commit_before_publish':1,
                        'after_retirement_failed':1, 'after_retirement_published':1})
        known={} if prior is None else {r['order_id']:r['payload'] for r in prior['rows']}
        killed=False
        try:
            with daemon(exe,path,hooks,pipesize=1024*1024) as d:
                pid=d.child.pid
                hooks.token('after_retire_commit_before_publish',d)
                committed=frozen(d,path,root/'committed')
                oracle(pg,committed,known,root/'committed-union.json')
                prefix=committed['meta']['last_retired_order']
                hooks.release('after_retire_commit_before_publish',fault.encode())
                hooks.token('after_retirement_failed',d)
                # Actual commit exists, but failed reload has not published success.
                failure_start=len(d.events)
                wait(d,lambda:any(e['event']=='outbox' and e['local_state']=='nomem' for e in d.events[failure_start:]),timeout=3)
                if cut=='failed': kill(d); killed=True
                else:
                    hooks.release('after_retirement_failed')
                    hooks.token('after_retirement_published',d)
                    reconciled=frozen(d,path,root/'prefix-reconciled')
                    require(reconciled['meta']==committed['meta'])
                    status=publication(d,reconciled)
                    require(reconciled['meta']['last_retired_order']==prefix)
                    if cut=='published': kill(d); killed=True
                    else:
                        hooks.release('after_retirement_published')
                        wait_p(d,caught_up,timeout=12)
                        require(d.child.pid==pid and d.child.poll() is None)
                        require(all(s['frames']==0 for s in d.snapshots))
                        d.stop()
                pairs=all_counters(d)
            final=inventory(path,root/'stopped')
            oracle(pg,final,known,root/'stopped-union.json')
            expected=[dict(run_id=json.loads(v)['run_id'],batch_id=json.loads(v)['batch_id'],payload=v) for v in known.values()]
            result=finish(exe,path,pg,root,expected) if killed else assert_remote(pg,final,expected)
            (root/"final-remote.json").write_text(json.dumps(result.pop("remote"),indent=2))
            return dict(fault=fault,old_run=old,kill_cut=cut,tokens=hooks.tokens,status_pairs=pairs,
                        publication=None if cut=='failed' else status,result=result)
        finally: hooks.close()


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--build',type=Path,required=True)
    ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--case',choices=('overlap','retirement'))
    args=ap.parse_args()
    build=args.build.resolve()
    evidence=Evidence(args.output)
    native=Path(tempfile.mkdtemp(prefix='vigil-remediation-evidence-'))
    evidence.report.update(native=str(native),binary_sha256=hashlib.sha256((build/'vigild-resilience-test').read_bytes()).hexdigest())
    evidence.save()
    if args.case in (None,'overlap'):
        for old in (False,True):
            for fault in ('N','P','T','U'):
                name=f'overlap-{fault}-old{int(old)}'
                evidence.run(name,'confirmed remote A and uncertain B recover without restart/new traffic; exact bytes, publication counts, one retirement and continued admission',
                             lambda n=name,f=fault,o=old:overlap(build,native/n,f,o))
        for fault in ('N','P'):
            for point in ('after_append_failed','after_append_reconciled','after_append_recovery_published',
                          'after_retire_commit_before_publish','after_retirement_published'):
                name=f'kill-overlap-{fault}-{point}'
                evidence.run(name,'SIGKILL at exact overlap reconciliation cut preserves local/PG union and original bytes; restart retires each logical batch once',
                             lambda n=name,f=fault,p=point:overlap(build,native/n,f,cut=p))
    if args.case in (None,'retirement'):
        for old in (False,True):
            for fault in ('P','T','U'):
                name=f'retirement-{fault}-old{int(old)}'
                evidence.run(name,'postcommit usage prepare/first-step/final-step NOMEM; quiet same-process prefix reconciliation matches SQLite and PG exactly',
                             lambda n=name,f=fault,o=old:retirement_nomem(build,native/n,f,o))
        for fault in ('P','T','U'):
            for cut in ('failed','published'):
                name=f'kill-retirement-{fault}-{cut}'
                evidence.run(name,'SIGKILL around NOMEM retired-prefix publication preserves exact remote effects and committed retirement',
                             lambda n=name,f=fault,c=cut:retirement_nomem(build,native/n,f,cut=c))
    evidence.finish()


if __name__=='__main__': main()
