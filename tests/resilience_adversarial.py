#!/usr/bin/env python3
"""Stage 3 adversarial combinations on real Linux, using existing fixtures.
No endpoint/store argument: every destructive target is allocated here. Seed
chooses a boundary combination, not a sleep at which to guess a transaction.
"""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import traceback

from evidence import Evidence
from monitor_integration import bus, send, source
from persistence_integration import caught_up, wait_p
from resilience_database_contract import ReplayPostgres
from resilience_integration import Hook, create, daemon, inventory, wait_local, committed_records
from resilience_replay import Withhold, assert_remote, kill, wait
from simulator.protocol import reading

LOCAL = ('after_take_before_begin', 'after_local_begin',
         'after_local_insert_before_commit', 'after_local_commit_before_publish')
REMOTE = ('before_remote_send', 'after_remote_send', 'after_remote_confirm_before_result',
          'before_retire_begin', 'after_retire_delete_before_commit', 'after_retire_commit_before_publish')
INVARIANTS = [
    'Every previously inventoried durable order remains byte-identical in SQLite or an exact PG receipt',
    'SQLite order prefix, usage, origins, sequences and counters reconcile at each stopped cut',
    'No remote duplicate logical effects; full origin/history/checkpoint/receipt projection matches',
    'Precommit candidate absent; postcommit candidate present; no retirement before verified commit',
    'Old replay cannot initialize live values; PG pause permits admission; stale detection age is 500..720 ms',
]


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


@contextmanager
def cluster():
    pg = ReplayPostgres()
    try:
        # Verify identity using the server itself before stopping or mutating it.
        assert Path(pg.sql('SHOW data_directory')).resolve() == pg.data.resolve()
        assert pg.root.name.startswith('vigil-pg-') and pg.root.parent == Path(tempfile.gettempdir())
        assert pg.sql('SHOW fsync') == pg.sql('SHOW full_page_writes') == 'on'
        yield pg
    finally:
        pg.close()


def counters(d):
    return dict(pid=d.child.pid, exit_status=d.child.poll(),
                persistence=next((e for e in reversed(d.events) if e['event']=='persistence'), {}),
                outbox=next((e for e in reversed(d.events) if e['event']=='outbox'), {}),
                monitor=d.snapshots[-1] if d.snapshots else {})


def oracle(pg, inv, known, output):
    """Check union at intermediate cuts, including already retired local payloads."""
    dump = pg.dump()
    remote = {}
    for r in dump['replay_receipts']:
        raw = bytes.fromhex(r['payload'][2:]).decode()
        b = json.loads(raw)
        assert b['origin']['spool_id'] == inv['meta']['spool_id']
        assert b['order_id'] not in remote
        remote[b['order_id']] = raw
    local = {r['order_id']: r['payload'] for r in inv['rows']}
    for order, raw in known.items():
        assert local.get(order) == raw or remote.get(order) == raw, ('lost durable order', order)
    for order in local.keys() & remote.keys():
        assert local[order] == remote[order], ('changed retry', order)
    for order in range(1, inv['meta']['last_retired_order'] + 1):
        assert order in remote, ('retired without receipt', order)
    for order, raw in {**remote, **local}.items():
        assert order not in known or known[order] == raw
        known[order] = raw
    save(output, dict(sqlite=inv, postgres=dump, checked_durable_orders=len(known)))
    return dump


def stale_age(snapshot):
    alarm=next(e for e in snapshot['rows'] if e['event']=='alarm_status' and
               e['pod']==1 and e['channel']==0 and e['kind']=='communication_lost')
    age=alarm['raised_ms']-source(snapshot,1,0)['last_valid_ms']
    assert alarm['active']==1 and 500<=age<=720, age
    return age


def schedule(build, native, evidence_dir, seed):
    rng = random.Random(seed)
    local = LOCAL[(seed // 12) % 4] if seed < 48 else LOCAL[(seed - 48) // 3]
    remote = REMOTE[(seed // 2) % 6] if seed < 48 else 'committed_response_withheld'
    concurrent = bool(seed % 2) if seed < 48 else True
    count, extra = rng.randint(3, 12), rng.randint(3, 16)
    replay_crashes = 1 if seed < 48 else 1 + (seed - 48) % 3
    root = native / f'seed-{seed:02d}'
    root.mkdir(mode=0o700)
    record = dict(seed=seed, local_failpoint=local, remote_failpoint=remote,
                  concurrent=concurrent, replay_crashes=replay_crashes, candidate_readings=count, new_readings=extra if concurrent else 0,
                  expected_invariants=INVARIANTS, actual_assertions=[], cuts=[], exit_status=1, native_evidence=str(root),
                  reproduction=f'.venv/bin/python tests/resilience_adversarial.py --build {build} --seed {seed} --output artifacts/repro-seed-{seed}')
    report_path = evidence_dir / 'run.json'
    save(report_path, record)
    exe, init = build/'vigild-resilience-test', build/'vigil-outbox-init'
    known = {}
    proxy = None
    try:
        with cluster() as pg:
            pg.stop()
            path = create(root/'state', init)
            # Independently confirmed durable baseline exists before every cut.
            with daemon(exe, path, env=pg.env()) as d, bus() as raw:
                for n in range(4):
                    send(raw, reading(1, 0, 2200, 0, n))
                wait_local(d, lambda s:s['local_readings']==4)
                d.stop()
                record['cuts'].append(dict(point='offline baseline stop', **counters(d)))
            before = inventory(path, root/'baseline')
            known.update({r['order_id']:r['payload'] for r in before['rows']})
            hook = Hook(local, hit=1, repeat=2)
            hook.env.update(pg.env())
            try:
                with daemon(exe, path, hook, capacity=128) as d, bus() as raw:
                    tokens = [hook.token()]
                    snap = d.wait(lambda s:s['frames']==0)
                    assert source(snap, 1, 0)['has_value']==0
                    for n in range(count):
                        send(raw, reading(1, 0, 2800 if n % 2 else 2200, 0, n))
                    d.wait(lambda s:s['valid_readings']==count)
                    hook.release()
                    tokens.append(hook.token())
                    kill(d)
                    record['cuts'].append(dict(point=local, tokens=tokens, **counters(d)))
                    assert d.child.returncode == -9
            finally:
                hook.close()
            assert d.errors.decode().startswith('ready ') and len(d.errors.decode().splitlines())==1, d.errors
            after = inventory(path, root/'local-cut')
            assert all(next(r['payload'] for r in after['rows'] if r['order_id']==k)==v for k,v in known.items())
            total = sum(e['kind']=='reading' for e in committed_records(after).values())
            assert total == 4 + (count if local == LOCAL[-1] else 0), total
            record['actual_assertions'].append('Exact pre/post-local-commit candidate and baseline survival')
            pg.start()
            oracle(pg, after, known, root/'local-union.json')
            head = after['rows'][0]
            if remote == 'committed_response_withheld':
                proxy = Withhold(pg)
                env = proxy.env()
                hook = None
            else:
                hook = Hook(remote, hit=1)
                hook.env.update(pg.env())
                env = None
            try:
                with daemon(exe, path, hook, env=env, capacity=128) as d, bus() as raw:
                    if proxy:
                        wait(d, lambda:proxy.count()==1)
                        token = 'committed_response_withheld:ReadyForQuery'
                        assert proxy.requests[0] == head['payload'].encode()
                    else:
                        token = hook.token()
                    admits_during_pause = remote in REMOTE[:3] or proxy is not None
                    if concurrent:
                        for n in range(extra):
                            send(raw, reading(1, 0, 2800 if n % 2 else 2200, 0, n))
                            d.pump(0.002)
                        if admits_during_pause:
                            wait_local(d, lambda s:s['local_readings']==extra)
                        s = d.wait(lambda s:s['valid_readings']==extra and source(s,1,0)['freshness']=='stale')
                        assert source(s,1,0)['current']==0
                        record['stale_age_ms']=stale_age(s)
                    # PG observer distinguishes possible send from confirmed server commit.
                    receipt = pg.sql(f"SELECT encode(payload,'hex') FROM vigil.replay_receipts WHERE run_id='{head['run_id']}' AND batch_id={head['batch_id']}")
                    if remote not in REMOTE[:2]:
                        assert receipt == head['payload'].encode().hex()
                    kill(d)
                    record['cuts'].append(dict(point=remote, token=token, head_receipt_before_kill=receipt, **counters(d)))
                    assert d.child.returncode == -9
            finally:
                if hook: hook.close()
                if proxy: proxy.close(); proxy = None
            assert d.errors.decode().startswith('ready ') and len(d.errors.decode().splitlines())==1, d.errors
            cut = inventory(path, root/'remote-cut')
            if remote == REMOTE[-1]:
                assert cut['meta']['last_retired_order']==head['order_id']
            else:
                assert cut['meta']['last_retired_order']==0
                assert cut['rows'][0]['payload']==head['payload']
            oracle(pg, cut, known, root/'remote-union.json')
            # Proxy schedules exercise one/two/three consecutive lost acknowledgements,
            # preserving the same remote head while each process admits another run.
            for repeat in range(1, replay_crashes):
                proxy = Withhold(pg)
                try:
                    with daemon(exe, path, env=proxy.env()) as d:
                        wait(d, lambda:proxy.count()==1)
                        assert proxy.requests[0] == head['payload'].encode()
                        kill(d)
                        record['cuts'].append(dict(point=remote, repeat=repeat, **counters(d)))
                    repeated=inventory(path,root/f'repeated-{repeat}')
                    assert repeated['meta']['last_retired_order']==0
                    oracle(pg,repeated,known,root/f'repeated-{repeat}-union.json')
                finally:
                    proxy.close(); proxy=None
            record['actual_assertions'].append('Remote uncertainty and retirement prefix obey selected cut')
            # Third process replays every outstanding original identity to equality.
            with daemon(exe, path, env=pg.env()) as d:
                wait_p(d, caught_up, timeout=30)
                s = d.wait(lambda s:s['frames']==0)
                assert source(s,1,0)['has_value']==0
                d.stop()
                record['cuts'].append(dict(point='final healthy stop', **counters(d)))
            final = inventory(path, root/'finished')
            oracle(pg, final, known, root/'final-union.json')
            exact = assert_remote(pg, final)
            assert exact['readings']==total+(extra if concurrent and admits_during_pause else 0)
            record['actual_assertions'].extend(['All durable orders represented by exact remote receipts',
                'Full remote history/origin/snapshot/gap equality and zero local pending',
                'Original order/run identities and fresh-process unknown live state'])
            record.update(exit_status=0, durable_orders=len(known), readings=exact['readings'],
                          native_evidence=str(root), sqlite=final['meta'])
    except BaseException:
        record['traceback'] = traceback.format_exc()
        raise
    finally:
        if proxy: proxy.close()
        save(report_path, record)
    return dict(seed=seed, local=local, remote=remote, concurrent=concurrent,
                durable_orders=record['durable_orders'], readings=record['readings'], replay_crashes=replay_crashes, exit_status=0)


def server_transaction_kill(build, root, evidence_dir):
    """Kill daemon with real server paused between history and receipt."""
    from resilience_replay import seed as seed_store
    root.mkdir(mode=0o700)
    with cluster() as pg:
        path, initial, tokens=seed_store(build/'vigild-resilience-test',build/'vigil-outbox-init',root)
        known={r['order_id']:r['payload'] for r in initial['rows']}
        pg.sql("CREATE FUNCTION vigil.test_stage3_pause() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN PERFORM pg_advisory_xact_lock(762193); RETURN NEW; END $$;"
               "CREATE TRIGGER test_stage3_pause BEFORE INSERT ON vigil.checkpoints FOR EACH ROW EXECUTE FUNCTION vigil.test_stage3_pause()")
        holder=subprocess.Popen(pg.command(),stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        try:
            holder.stdin.write("SELECT pg_advisory_lock(762193); SELECT 'locked';\n"); holder.stdin.flush()
            # Bounded read handshake; no sleep guesses the remote transaction.
            import select
            assert select.select([holder.stdout],[],[],3)[0]
            while holder.stdout.readline().strip()!='locked': assert holder.poll() is None
            with daemon(build/'vigild-resilience-test',path,env=pg.env()) as d:
                pid=wait(d,lambda:pg.sql("SELECT pid FROM pg_stat_activity WHERE application_name='vigild' AND wait_event='advisory'"))
                kill(d)
                observed=counters(d)
                # Terminate only that backend in this verified disposable server;
                # force rollback after proving SIGKILL occurred in its transaction.
                assert pg.sql('SELECT pg_terminate_backend('+pid+')')=='t'
            for table in ('runs','run_origins','history','checkpoints','replay_receipts'):
                assert pg.sql('SELECT count(*) FROM vigil.'+table)=='0'
        finally:
            holder.communicate('SELECT pg_advisory_unlock(762193);\n',timeout=3)
        pg.sql('DROP TRIGGER test_stage3_pause ON vigil.checkpoints; DROP FUNCTION vigil.test_stage3_pause()')
        cut=inventory(path,root/'killed')
        assert cut['meta']['last_retired_order']==0
        oracle(pg,cut,known,root/'killed-union.json')
        with daemon(build/'vigild-resilience-test',path,env=pg.env()) as d:
            wait_p(d,caught_up,timeout=20); d.stop()
        final=inventory(path,root/'finished')
        oracle(pg,final,known,root/'final-union.json')
        exact=assert_remote(pg,final,initial['rows'])
        save(evidence_dir/'run.json',dict(sqlite=final,postgres=exact['remote'],
             monitor=observed,tokens=tokens,backend=int(pid),actual_kill_point='server advisory wait before checkpoint',exit_status=0))
        return dict(remote_rollback=True,preserved_readings=exact['readings'],daemon_exit=-9)


def resources(d, path):
    status = Path(f'/proc/{d.child.pid}/status').read_text().splitlines()
    rss = int(next(x.split()[1] for x in status if x.startswith('VmRSS:')))*1024
    return dict(at_s=time.monotonic(), rss_bytes=rss,
                files={suffix or 'db':Path(str(path)+suffix).stat().st_size
                       for suffix in ('','-wal','-shm') if Path(str(path)+suffix).exists()})


def backlog(build, root, evidence_dir):
    """6000 readings over >=60s, plus new readings during replay, actual PG."""
    root.mkdir(mode=0o700)
    with cluster() as pg:
        path = create(root/'state', build/'vigil-outbox-init')
        pg.stop()
        samples = []
        admission=Hook('after_take_before_begin',hit=1,repeat=10000)
        admission.env.update(pg.env())
        admission_tokens=[]
        try:
            with daemon(build/'vigild-resilience-test', path, admission, capacity=256, pipesize=1024*1024) as d, bus() as raw:
                admission_tokens.append(admission.token())
                start = time.monotonic()
                for n in range(6000):
                    target = start+n/100
                    while time.monotonic()<target:
                        d.pump(max(0, min(.005, target-time.monotonic())))
                    send(raw, reading(1, 0, 2200, 0, n%256))
                    d.pump(0)
                    if n%100==0: samples.append(resources(d,path))
                    # Pin one candidate while exactly 32 more readings arrive.
                    # Otherwise 6000 one-record batches may legitimately hit the
                    # 4096-batch ceiling before the record ceiling. No cap changes.
                    if (n+1)%32==0:
                        d.wait(lambda s:s['valid_readings']==n+1)
                        admission.release()
                        admission_tokens.append(admission.token())
                admission.release(b'D')
                local = wait_local(d, lambda s:s['local_readings']==6000, timeout=15)
                pending = wait_p(d, lambda s:s['readings_queued']==6000)
                assert pending['readings_dropped']==pending['events_dropped']==0
                assert local['durable_pending']>=6000 and local['spool_retired']==0
                d.stop()
                offline = counters(d)
        finally:
            admission.close()
        inv = inventory(path, root/'backlog')
        assert inv['meta']['admitted_readings']==6000
        known={r['order_id']:r['payload'] for r in inv['rows']}
        pg.start()
        hook=Hook('before_remote_send',hit=1); hook.env.update(pg.env())
        try:
            startup=time.monotonic()
            with daemon(build/'vigild-resilience-test',path,hook,capacity=256,pipesize=1024*1024,startup_timeout=30) as d,bus() as raw:
                startup_s=time.monotonic()-startup
                token=hook.token()
                for n in range(128): send(raw,reading(1,0,2800,0,n)); d.pump(.002)
                wait_local(d,lambda s:s['local_readings']==128)
                s=d.wait(lambda s:s['valid_readings']==128 and source(s,1,0)['freshness']=='stale')
                assert source(s,1,0)['high']==1
                age_ms=stale_age(s)
                samples.append(resources(d,path))
                drain_start=time.monotonic(); hook.release()
                done=wait_p(d,lambda s:caught_up(s) and s['readings_written']==128,timeout=120)
                drain=time.monotonic()-drain_start
                assert done['readings_dropped']==done['events_dropped']==0
                samples.append(resources(d,path)); d.stop(); final_counters=counters(d)
        finally: hook.close()
        final=inventory(path,root/'finished')
        oracle(pg,final,known,root/'union.json')
        result=assert_remote(pg,final,inv['rows'])
        assert result['readings']==6128
        for sample in samples:
            assert sample['files']['db']<=512*1024**2
            assert sample['files'].get('-wal',0)<520*1024**2
            assert sample['files'].get('-shm',0)<=2*1024**2
        save(evidence_dir/'run.json',dict(samples=samples,offline=offline,final=final_counters,admission_tokens=admission_tokens,
             sqlite=final,postgres=result['remote'],token=token,drain_s=drain,startup_s=startup_s,stale_age_ms=age_ms,exit_status=0))
        return dict(offered=6000,new_during_replay=128,remote_readings=6128,
                    backlog_batches=inv['meta']['pending_batches'],drain_s=drain,startup_s=startup_s,stale_age_ms=age_ms,
                    max_rss_bytes=max(s['rss_bytes'] for s in samples),max_wal_bytes=max(s['files'].get('-wal',0) for s in samples))


def damaged_copies(build, root, evidence_dir):
    """Corruption is outside survival assumptions; WAL tail loss may be valid rollback."""
    from resilience_replay import seed as seed_store
    root.mkdir(mode=0o700)
    path, original, tokens = seed_store(build/'vigild-resilience-test', build/'vigil-outbox-init', root)
    template = root/'template'; template.mkdir(mode=0o700)
    for suffix in ('','-wal'):
        shutil.copy2(Path(str(path)+suffix),template/('outbox.sqlite3'+suffix))
    wal=(template/'outbox.sqlite3-wal').read_bytes()
    page=struct.unpack('>I',wal[8:12])[0]; frame=24+page
    assert len(wal)>32 and (len(wal)-32)%frame==0
    commits=[i for i in range((len(wal)-32)//frame) if struct.unpack('>I',wal[32+i*frame+4:40+i*frame])[0]]
    assert len(commits)>=3
    results={}
    for mode in ('bad-header','db-truncated','partial-uncommitted-tail','wal-truncated-commit','wal-bad-commit-checksum'):
        case=root/mode; case.mkdir(mode=0o700)
        for src in template.iterdir(): shutil.copy2(src,case/src.name)
        db=case/'outbox.sqlite3'; wf=case/'outbox.sqlite3-wal'
        expected='startup exit 2 without READY; DB bytes preserved' if mode in ('bad-header','db-truncated') else 'recover valid WAL prefix; corruption exclusion permits last transaction loss'
        print('EXPECT damaged copy:',mode,expected,flush=True)
        if mode in ('bad-header','db-truncated'):
            # Materialize a consistent offline clone first. An intact WAL can
            # legitimately restore a damaged database page, including its header.
            from resilience_integration import SQLITE
            subprocess.run([SQLITE,str(db),'PRAGMA wal_checkpoint(TRUNCATE)'],check=True,capture_output=True,timeout=10)
            if wf.exists(): wf.unlink()
        if mode=='bad-header':
            with db.open('r+b') as f: f.write(b'not a sqlite database')
        elif mode=='db-truncated':
            # With no WAL, incomplete schema cannot be mistaken for a valid fresh store.
            with db.open('r+b') as f: f.truncate(100)
        elif mode=='partial-uncommitted-tail':
            with wf.open('ab') as f: f.write(b'\x00'*77)
        elif mode=='wal-truncated-commit':
            with wf.open('r+b') as f: f.truncate(32+(commits[-1]+1)*frame-1)
        else:
            b=bytearray(wal); b[32+commits[-1]*frame+16]^=1; wf.write_bytes(b)
        before=hashlib.sha256(db.read_bytes()).hexdigest()
        if mode in ('bad-header','db-truncated'):
            child=subprocess.run([str(build/'vigild'),'--outbox-path',str(db)],capture_output=True,timeout=5,
                                 env=dict(os.environ,VIGIL_DB_HOST='/tmp/vigil-test-no-postgres'))
            assert child.returncode==2 and b'ready ' not in child.stderr
            assert hashlib.sha256(db.read_bytes()).hexdigest()==before
            (evidence_dir/(mode+'.stderr')).write_bytes(child.stderr)
            results[mode]=dict(expected=expected,exit_status=2,preserved_db_sha256=before)
        else:
            recovered=inventory(db,root/(mode+'-recovered'))
            if mode=='partial-uncommitted-tail': assert recovered['rows']==original['rows']
            else:
                assert recovered['rows']==original['rows'][:-1]
                assert recovered['meta']['admitted_readings']==0
            with daemon(build/'vigild',db) as d:
                wait_local(d,lambda s:s['pending_batches']>=len(recovered['rows'])); d.stop()
            results[mode]=dict(expected=expected,retained_orders=[r['order_id'] for r in recovered['rows']],exit_status=0)
    save(evidence_dir/'run.json',dict(tokens=tokens,cases=results,original=original,exit_status=0))
    return dict(cases=len(results),rejected=2,valid_prefix_recovery=3)


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--build',type=Path,required=True)
    ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--seed',type=int,action='append',help='repeat to select exact schedules (0..59); default all 60')
    ap.add_argument('--extras-only',action='store_true')
    args=ap.parse_args()
    assert sys.platform=='linux' and not sys.flags.optimize
    assert not (args.extras_only and args.seed)
    seeds=args.seed if args.seed is not None else list(range(60))
    assert len(set(seeds))==len(seeds) and all(0<=s<60 for s in seeds)
    # Do not inherit libpq service, credential, options or test-control routing.
    for k in list(os.environ):
        if k.startswith(('PG','VIGIL_DB_','VIGIL_TEST_')): os.environ.pop(k)
    build=args.build.resolve()
    native=Path(tempfile.mkdtemp(prefix='vigil-stage3-evidence-'))
    symbols=subprocess.check_output(['nm',str(build/'vigild')],text=True)
    assert 'vigil_test_' not in symbols
    evidence=Evidence(args.output)
    evidence.report.update(native_evidence=str(native),seeds=[] if args.extras_only else seeds,
        binary_sha256=hashlib.sha256((build/'vigild-resilience-test').read_bytes()).hexdigest())
    evidence.save()
    for s in ([] if args.extras_only else seeds):
        name=f'seed-{s:02d}'
        evidence.run(name,'; '.join(INVARIANTS),lambda s=s,name=name:schedule(build,native,evidence.directory/name,s))
    if args.seed is None:
        evidence.run('server-transaction-kill','SIGKILL daemon while PG waits after history/before checkpoint; isolated backend termination rolls back all remote effects; original local bytes replay once',
                     lambda:server_transaction_kill(build,native/'server-kill',evidence.directory/'server-transaction-kill'))
        evidence.run('damaged-copies','Invalid DB refuses READY; incomplete WAL suffix rolls back atomically; last committed WAL destruction is outside durability assumption',
                     lambda:damaged_copies(build,native/'damaged',evidence.directory/'damaged-copies'))
        evidence.run('sustained-backlog','6000 at 100/s accumulate locally during PG outage; 128 new readings commit during paused replay; all 6128 drain once with bounded files and zero history drops',
                     lambda:backlog(build,native/'backlog',evidence.directory/'sustained-backlog'))
    evidence.finish()


if __name__=='__main__': main()
