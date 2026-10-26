#!/usr/bin/env python3
"""Stage 2: real SocketCAN/SQLite/PostgreSQL with handshaked failure boundaries.
All state native to Linux. Inventories only after daemon death/stop.
"""
from verification import require  # also refuses optimized execution before fixture imports
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import select
import socket
import subprocess
import tempfile
import threading
import time
from monitor_integration import bus, send, source, config_text
from persistence_integration import wait_p, caught_up
from resilience_integration import Hook, daemon, create, inventory, wait_local, committed_records
from resilience_database_contract import ReplayPostgres
from simulator.protocol import reading, heartbeat


def wait(d, predicate, timeout=10):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        d.pump(0.01)
        value=predicate()
        if value: return value
    raise AssertionError(('deadline',d.events[-8:]))


def kill(d):
    d.child.kill(); d.child.wait(timeout=3)


def seed(exe, init, root):
    print('EXPECT A: local commit/directory sync, no remote submission, kill; exact original bytes survive',flush=True)
    path=create(root/'state',init)
    hook=Hook('after_local_commit_before_publish',hit=1,repeat=2)
    try:
        with daemon(exe,path,hook) as d, bus() as raw:
            tokens=[hook.token()]
            for n in range(8): send(raw,reading(1,0,2200,0,n))
            d.wait(lambda s:s['valid_readings']==8)
            hook.release(); tokens.append(hook.token())
            kill(d)
        inv=inventory(path,root/'seed')
        require((len([e for e in committed_records(inv).values() if e['kind']=='reading'])==8))
        require((inv['meta']['retired_records']==0))
        return path,inv,tokens
    finally: hook.close()


def assert_remote(pg, inv, expected_batches=None):
    dump=pg.dump()
    spool=inv['meta']['spool_id']
    receipts=[r for r in dump['replay_receipts'] if json.loads(bytes.fromhex(r['payload'][2:]))['origin']['spool_id']==spool]
    batches={}
    expected={}
    for r in receipts:
        raw=bytes.fromhex(r['payload'][2:]); b=json.loads(raw)
        key=(r['run_id'],r['batch_id'])
        require((key not in batches))
        batches[key]=raw.decode()
        require((len(b['records'])==r['record_count']))
        cp=next(c for c in dump['checkpoints'] if (c['run_id'],c['batch_id'])==key)
        require((cp['kind']==b['kind'] and cp['reading_drops']==b['reading_drops'] and cp['event_drops']==b['event_drops']))
        require((cp['detail']==b['health'] and cp['alarms']==b['alarms']))
        origin=next(o for o in dump['run_origins'] if o['run_id']==b['run_id'])
        require((all(origin[k]==b['origin'][k] for k in ('spool_id','run_no','boot_id','started_ms','started_utc_ms'))))
        require((origin['run_token']=='\\x'+b['origin']['run_token']))
        for e in b['records']:
            event_key=(b['run_id'],e['seq'])
            require((event_key not in expected), lambda: ('duplicate logical history across daemon batches'))
            expected[event_key]=e
    runs={r['run_id'] for r in receipts}
    actual={(h['run_id'],h['seq']):h for h in dump['history'] if h['run_id'] in runs}
    require((set(actual)==set(expected)))
    times={(r[0],r[1]):r[2] for r in json.loads(pg.sql("SELECT coalesce(json_agg(json_build_array(run_id,seq,(extract(epoch FROM observed_at)*1000)::bigint)),'[]') FROM vigil.history"))}
    for key,e in expected.items():
        h=actual[key]
        require((h['monotonic_ms']==e['mono_ms']))
        require((all(h[k]==e[k] for k in ('kind','pod','channel','detail'))))
        require((times[key]==e['utc_ms']))
    if expected_batches:
        require((all(batches.get((r['run_id'],r['batch_id']))==r['payload'] for r in expected_batches)))
    require((inv['meta']['pending_records']==inv['meta']['pending_batches']==inv['meta']['pending_payload_bytes']==0))
    require((len(actual)==inv['meta']['admitted_records']==inv['meta']['retired_records']))
    require((len(batches)==inv['meta']['last_retired_order']==inv['meta']['next_order_id']-1))
    require((sum(e['kind']=='reading' for e in expected.values())==inv['meta']['admitted_readings']==inv['meta']['retired_readings']))
    return dict(history=len(actual),receipts=len(batches),readings=inv['meta']['retired_readings'],
                pending=0,remote=dump,local=inv['meta'])


def finish(exe,path,pg,root,old):
    with daemon(exe,path,env=pg.env()) as d:
        done=wait_p(d,caught_up,timeout=40)
        snap=d.wait(lambda s:s['frames']==0)
        require((source(snap,1,0)['has_value']==0))
        require((done['run_id'] not in {r['run_id'] for r in old}))
        d.stop()
        local=next(e for e in reversed(d.events) if e['event']=='outbox')
        final=next(e for e in reversed(d.events) if e['event']=='persistence')
        require((final['abandoned_readings']==final['abandoned_events']==0))
        require((local['local_committed']==final['records_written']))
    inv=inventory(path,root/'finished')
    return assert_remote(pg,inv,old)


def cuts(exe,init,root,name,repeats):
    print(f'EXPECT D/E/F: {name}; {repeats} precise kills; absent local row only after retirement commit; remote effects once',flush=True)
    pg=ReplayPostgres()
    try:
        path,inv,tokens=seed(exe,init,root)
        known={(r['run_id'],r['batch_id']):r for r in inv['rows']}
        for i in range(repeats):
            hook=Hook(name,hit=1); hook.env.update(pg.env())
            before=inventory(path,root/f'before-{i}')
            if not before['rows']:
                with daemon(exe,path) as refill:
                    wait_local(refill,lambda s:s['pending_batches']>0)
                    refill.stop()
                before=inventory(path,root/f'refill-{i}')
                known.update({(r['run_id'],r['batch_id']):r for r in before['rows']})
            head=before['rows'][0]
            try:
                with daemon(exe,path,hook) as d:
                    tokens.append(hook.token())
                    require((pg.sql(f"SELECT encode(payload,'hex') FROM vigil.replay_receipts WHERE run_id='{head['run_id']}' AND batch_id={head['batch_id']}")==head['payload'].encode().hex()))
                    kill(d)
                after=inventory(path,root/f'after-{i}')
                if name=='after_retire_commit_before_publish':
                    require((after['meta']['last_retired_order']==head['order_id']))
                    require((head['order_id'] not in [r['order_id'] for r in after['rows']]))
                    require((after['meta']['retired_records']==before['meta']['retired_records']+head['record_count']))
                else:
                    require((after['meta']['last_retired_order']==before['meta']['last_retired_order']))
                    require((next(r for r in after['rows'] if r['order_id']==head['order_id'])['payload']==head['payload']))
                known.update({(r['run_id'],r['batch_id']):r for r in after['rows']})
            finally: hook.close()
        result=finish(exe,path,pg,root,list(known.values()))
        result.update(tokens=tokens,kills=repeats)
        return result
    finally: pg.close()


class Withhold:
    """Forward real PG, suppress every replay result through ReadyForQuery.
    Record complete parameter bytes; leave client waiting until kill/deadline.
    No commit is inferred from timing: independent SQL plus protocol handshake.
    """
    def __init__(self,pg):
        self.pg=pg; self.stop=threading.Event(); self.lock=threading.Lock()
        self.requests=[]; self.error=None
        self.listener=socket.socket(); self.listener.bind(('127.0.0.1',0)); self.listener.listen(4)
        self.port=self.listener.getsockname()[1]
        self.thread=threading.Thread(target=self.forward); self.thread.start()
    def count(self):
        with self.lock: return len(self.requests)
    def forward(self):
        try:
            while not self.stop.is_set():
                if not select.select([self.listener],[],[],0.02)[0]: continue
                client,_=self.listener.accept()
                with client,socket.create_connection(('127.0.0.1',self.pg.port),timeout=2) as server:
                    request=b''; response=b''; discard=False; recorded=False
                    while not self.stop.is_set():
                        closed=False
                        for src in select.select([client,server],[],[],0.02)[0]:
                            try: data=src.recv(262144)
                            except ConnectionResetError: closed=True; break
                            if not data: closed=True; break
                            if src is client:
                                request=(request+data)[-262144:]
                                discard=discard or b'SELECT * FROM vigil.replay_batch' in request
                                try: server.sendall(data)
                                except (BrokenPipeError,ConnectionResetError): closed=True; break
                            elif discard:
                                response=(response+data)[-262144:]
                                if b'Z\x00\x00\x00\x05I' in response and not recorded:
                                    start=request.index(b'{"format":2')
                                    # Binary parameter ends immediately before
                                    # Bind/Execute framing; JSONDecoder finds exact end.
                                    text=request[start:].decode('utf-8',errors='surrogateescape')
                                    _,end=json.JSONDecoder().raw_decode(text)
                                    payload=text[:end].encode()
                                    with self.lock: self.requests.append(payload)
                                    recorded=True
                            else:
                                try: client.sendall(data)
                                except (BrokenPipeError,ConnectionResetError): closed=True; break
                        if closed: break
        except BaseException as e:
            if not self.stop.is_set(): self.error=repr(e)
    def env(self):
        return dict(self.pg.env(),VIGIL_DB_HOST='127.0.0.1',VIGIL_DB_PORT=str(self.port),PGSSLMODE='disable')
    def close(self):
        self.stop.set(); self.thread.join(timeout=3); self.listener.close()
        require((not self.thread.is_alive() and self.error is None), lambda: (self.error))


def ambiguity(exe,init,root,repeats):
    print(f'EXPECT C/G/H: {repeats} observer-confirmed commits with suppressed success and SIGKILL; then repeated same-process retries; original identity/bytes retained',flush=True)
    pg=ReplayPostgres(); proxy=None
    try:
        path,inv,tokens=seed(exe,init,root)
        old=inv['rows']; head=old[0]
        proxy=Withhold(pg)
        known={(r['run_id'],r['batch_id']):r for r in old}
        for i in range(repeats):
            count=proxy.count()
            with daemon(exe,path,env=proxy.env()) as d:
                wait(d,lambda:proxy.count()>count)
                require((pg.sql(f"SELECT encode(payload,'hex') FROM vigil.replay_receipts WHERE run_id='{head['run_id']}' AND batch_id={head['batch_id']}")==head['payload'].encode().hex()))
                kill(d)
            after=inventory(path,root/f'kill-{i}')
            require((after['meta']['retired_records']==0 and after['meta']['last_retired_order']==0))
            require((all(next(r for r in after['rows'] if r['order_id']==b['order_id'])['payload']==b['payload'] for b in old)))
            known.update({(r['run_id'],r['batch_id']):r for r in after['rows']})
        count=proxy.count()
        with daemon(exe,path,env=proxy.env()) as d:
            wait(d,lambda:proxy.count()>=count+3,timeout=15)
            status=wait_local(d,lambda s:s['remote_state']=='unknown',timeout=5)
            require((status['spool_retired']==0))
            kill(d)
        after=inventory(path,root/'same-process')
        known.update({(r['run_id'],r['batch_id']):r for r in after['rows']})
        require((len(proxy.requests)>=repeats+3))
        require((all(raw==head['payload'].encode() for raw in proxy.requests)))
        result=finish(exe,path,pg,root,list(known.values()))
        result.update(kills=repeats,identical_attempts=len(proxy.requests),tokens=tokens)
        return result
    finally:
        if proxy: proxy.close()
        pg.close()


def rollback(exe,init,root):
    print('EXPECT B: PG trigger waits after history before checkpoint; terminate that backend; all remote effects absent, local head survives and retries',flush=True)
    pg=ReplayPostgres(); holder=None
    try:
        path,inv,tokens=seed(exe,init,root)
        pg.sql('CREATE FUNCTION vigil.test_pause() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN PERFORM pg_advisory_xact_lock(762190); RETURN NEW; END $$;'
               'CREATE TRIGGER test_pause BEFORE INSERT ON vigil.checkpoints FOR EACH ROW EXECUTE FUNCTION vigil.test_pause()')
        holder=subprocess.Popen(pg.command(),stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        holder.stdin.write("SELECT pg_advisory_lock(762190); SELECT 'locked';\n"); holder.stdin.flush()
        while holder.stdout.readline().strip()!='locked': require((holder.poll() is None))
        with daemon(exe,path,env=pg.env()) as d:
            pid=wait(d,lambda:pg.sql("SELECT pid FROM pg_stat_activity WHERE application_name='vigild' AND wait_event='advisory'"))
            require((pg.sql("SELECT pg_terminate_backend("+pid+")")=='t'))
            wait_p(d,lambda s:s['query_failures']>=1)
            require((pg.sql('SELECT count(*) FROM vigil.history')=='0'))
            require((pg.sql('SELECT count(*) FROM vigil.runs')=='0'))
            require((pg.sql('SELECT count(*) FROM vigil.checkpoints')=='0'))
            require((pg.sql('SELECT count(*) FROM vigil.replay_receipts')=='0'))
            kill(d)
        holder.communicate('SELECT pg_advisory_unlock(762190);\n',timeout=3); holder=None
        pg.sql('DROP TRIGGER test_pause ON vigil.checkpoints; DROP FUNCTION vigil.test_pause()')
        after=inventory(path,root/'rollback')
        require((after['meta']['last_retired_order']==0))
        result=finish(exe,path,pg,root,after['rows'])
        result.update(terminated_backend=int(pid),tokens=tokens)
        return result
    finally:
        if holder: holder.communicate(timeout=3)
        pg.close()


def fairness(exe,init,root):
    print('EXPECT I: recovered FIFO head paused in PG while 64 new arrivals commit locally; old success cannot acknowledge new revision; drain with exact conservation and lifecycle',flush=True)
    pg=ReplayPostgres()
    try:
        path,inv,tokens=seed(exe,init,root)
        hook=Hook('before_remote_send',hit=1); hook.env.update(pg.env())
        try:
            with daemon(exe,path,hook,capacity=128) as d,bus() as raw:
                tokens.append(hook.token())
                initial=wait_local(d,lambda s:s['pending_batches']>=3)
                for n in range(64):
                    send(raw,reading(1,0,2800,0,n))
                    d.pump(0.002)
                local=wait_local(d,lambda s:s['local_readings']==64)
                require((local['spool_retired']==0 and local['durable_pending']>=initial['durable_pending']+64))
                snap=d.wait(lambda s:s['valid_readings']==64 and source(s,1,0)['freshness']=='stale')
                require((source(snap,1,0)['high']==1 and source(snap,1,0)['current']==0))
                pending=wait_p(d,lambda s:s['degraded']==1)
                require((pending['persisted_revision']==0))
                hook.release()
                done=wait_p(d,lambda s:caught_up(s) and s['readings_written']==64,timeout=20)
                require((done['readings_written']==64 and done['readings_dropped']==0))
                require((done['degraded_transitions']>=2))
                d.stop()
            final=inventory(path,root/'finished')
            result=assert_remote(pg,final,inv['rows'])
            require((result['readings']==72))
            result.update(tokens=tokens,new_readings=64)
            return result
        finally: hook.close()
    finally: pg.close()


def poison(exe,init,root):
    print('EXPECT J: permanent VG001/23514 poison retains exact head, one submission, admission continues to bounded capacity; explicit repair+restart drains unchanged',flush=True)
    results={}
    for mode in ('conflict','check'):
        pg=ReplayPostgres()
        try:
            case=root/mode; case.mkdir()
            path,inv,tokens=seed(exe,init,case)
            head=inv['rows'][0]; b=json.loads(head['payload'])
            if mode=='conflict':
                changed=json.loads(head['payload']); changed['mono_ms']+=1; pg.submit(changed)
            else: pg.sql("ALTER TABLE vigil.checkpoints ADD CONSTRAINT test_poison CHECK(kind!='recovery') NOT VALID")
            env=dict(pg.env(),VIGIL_TEST_RECORD_LIMIT='20')
            with daemon(exe,path,env=env,capacity=4) as d,bus() as raw:
                wait_local(d,lambda s:s['remote_state']=='poison')
                for n in range(80):
                    send(raw,reading(1,0,2800,0,n)); d.pump(0.002)
                local=wait_local(d,lambda s:s['local_state']=='full')
                require((local['replay_attempts']==1 and local['durable_pending']<=20))
                snap=d.wait(lambda s:s['valid_readings']==80 and source(s,1,0)['freshness']=='stale')
                require((source(snap,1,0)['high']==1))
                d.stop()
                local=next(e for e in reversed(d.events) if e['event']=='outbox')
                s=next(e for e in reversed(d.events) if e['event']=='persistence')
                require((local['replay_attempts']==1 and s['readings_dropped']>0))
                require((s['readings_queued']==local['local_readings']+s['readings_dropped']+s['reading_depth']+s['in_flight_readings']))
            after=inventory(path,case/'poisoned')
            require((after['meta']['retired_records']==0))
            require((after['rows'][0]['payload']==head['payload']))
            # Repair only disposable PG evidence as owner; immutable local bytes stay unchanged.
            if mode=='conflict':
                pg.sql(f"DELETE FROM vigil.replay_receipts WHERE run_id='{b['run_id']}'; DELETE FROM vigil.checkpoints WHERE run_id='{b['run_id']}'; DELETE FROM vigil.history WHERE run_id='{b['run_id']}'; DELETE FROM vigil.run_origins WHERE run_id='{b['run_id']}'; DELETE FROM vigil.runs WHERE run_id='{b['run_id']}'")
            else: pg.sql('ALTER TABLE vigil.checkpoints DROP CONSTRAINT test_poison')
            results[mode]=finish(exe,path,pg,case,after['rows'])
        finally: pg.close()
    return results


def outage(exe,init,root):
    print('EXPECT real PG stop/full/return: local cap never exceeded; held candidate resumes after retirement; gaps equal volatile drops and degraded recovers',flush=True)
    pg=ReplayPostgres()
    try:
        path=create(root/'state',init)
        env=dict(pg.env(),VIGIL_TEST_RECORD_LIMIT='40')
        with daemon(exe,path,env=env,capacity=4) as d,bus() as raw:
            wait_p(d,caught_up)
            pg.stop()
            wait_p(d,lambda s:not s['connected'] and s['connection_failures']+s['query_failures']>0)
            for n in range(100):
                send(raw,reading(1,0,2800,0,n)); d.pump(0.002)
            failed=wait_local(d,lambda s:s['local_state']=='full')
            require((failed['durable_pending']<=40))
            snap=d.wait(lambda s:s['valid_readings']==100 and source(s,1,0)['freshness']=='stale')
            require((source(snap,1,0)['high']==1))
            pg.start()
            done=wait_p(d,lambda s:caught_up(s) and s['readings_written']+s['readings_dropped']==100,timeout=45)
            require((done['readings_queued']==done['readings_written']+done['readings_dropped']==100))
            require((done['degraded_transitions']>=4))
            rid=done['run_id']
            require((int(pg.sql(f"SELECT coalesce(sum(reading_drops),0) FROM vigil.checkpoints WHERE run_id='{rid}'"))==done['readings_dropped']))
            require((int(pg.sql(f"SELECT coalesce(sum(event_drops),0) FROM vigil.checkpoints WHERE run_id='{rid}'"))==done['events_dropped']))
            d.stop()
        inv=inventory(path,root/'finished')
        result=assert_remote(pg,inv)
        result.update(dropped=done['readings_dropped'],events_dropped=done['events_dropped'],outage_ms=done['last_outage_ms'])
        return result
    finally: pg.close()


def deadlines(exe,init,root):
    print('EXPECT lock/statement errors remain transient, blackhole stop <=1.25s, local admissions continue; complete PG recovery exact',flush=True)
    results={}
    for mode in ('lock','statement','blackhole'):
        pg=ReplayPostgres(); holder=None; listener=None; client=None
        try:
            case=root/mode; case.mkdir(); path=create(case/'state',init)
            env=pg.env()
            if mode=='lock':
                holder=subprocess.Popen(pg.command(),stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
                holder.stdin.write("BEGIN; LOCK vigil.history IN ACCESS EXCLUSIVE MODE; SELECT 'locked';\n"); holder.stdin.flush()
                while holder.stdout.readline().strip()!='locked': require((holder.poll() is None))
            elif mode=='statement':
                pg.sql('CREATE FUNCTION vigil.test_sleep() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN PERFORM pg_sleep(3); RETURN NEW; END $$; CREATE TRIGGER test_sleep BEFORE INSERT ON vigil.history FOR EACH ROW EXECUTE FUNCTION vigil.test_sleep()')
            else:
                listener=socket.socket(); listener.bind(('127.0.0.1',0)); listener.listen(2)
                env=dict(pg.env(),VIGIL_DB_HOST='127.0.0.1',VIGIL_DB_PORT=str(listener.getsockname()[1]),PGSSLMODE='disable')
            with daemon(exe,path,env=env) as d,bus() as raw:
                for n in range(5): send(raw,reading(1,0,2800,0,n))
                local=wait_local(d,lambda s:s['local_readings']==5)
                if mode!='blackhole':
                    code='55P03' if mode=='lock' else '57014'
                    wait_p(d,lambda s:s['last_error']==code)
                snap=d.wait(lambda s:s['valid_readings']==5 and source(s,1,0)['freshness']=='stale')
                require((source(snap,1,0)['high']==1))
                start=time.monotonic(); d.stop(); elapsed=time.monotonic()-start
                require((elapsed<1.25), lambda: (elapsed))
            inv=inventory(path,case/'deferred')
            require((inv['meta']['pending_records']>=5 and inv['meta']['retired_records']==0))
            if holder: holder.communicate('ROLLBACK;\n',timeout=3); holder=None
            if mode=='statement': pg.sql('DROP TRIGGER test_sleep ON vigil.history; DROP FUNCTION vigil.test_sleep()')
            result=finish(exe,path,pg,case,inv['rows']); result['shutdown_s']=elapsed
            results[mode]=result
        finally:
            if holder: holder.communicate(timeout=3)
            if client: client.close()
            if listener: listener.close()
            pg.close()
    return results


def retry_timing(exe,init,root):
    print('EXPECT transient retries >=1/2/4/8/16/30/30 seconds despite continuous enqueues; permission retry >=30 seconds; no poisoned/rewritten work',flush=True)
    results={}
    for mode in ('transient','permission'):
        pg=ReplayPostgres()
        try:
            case=root/mode; case.mkdir(); path=create(case/'state',init)
            count=8 if mode=='transient' else 2
            hook=Hook('before_remote_send',hit=1,repeat=count); hook.env.update(pg.env())
            if mode=='transient':
                pg.sql("CREATE FUNCTION vigil.test_retry() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION USING ERRCODE='40001',MESSAGE='test retry'; END $$; CREATE TRIGGER test_retry BEFORE INSERT ON vigil.checkpoints FOR EACH ROW EXECUTE FUNCTION vigil.test_retry()")
            else: pg.sql('REVOKE EXECUTE ON FUNCTION vigil.replay_batch(bytea) FROM vigil')
            stamps=[]; tokens=[]; counter=0; next_send=0.0
            try:
                with daemon(exe,path,hook,capacity=128) as d,bus() as raw:
                    for i in range(count):
                        end=time.monotonic()+35
                        while not select.select([hook.nr],[],[],0)[0]:
                            require((time.monotonic()<end), lambda: ((mode,i,d.events[-4:])))
                            if time.monotonic()>=next_send:
                                send(raw,reading(1,0,2200,0,counter%256)); counter+=1
                                next_send=time.monotonic()+0.2
                            d.pump(0.02)
                        tokens.append(hook.token(timeout=1)); stamps.append(time.monotonic())
                        hook.release()
                    expected=[1,2,4,8,16,30,30] if mode=='transient' else [30]
                    intervals=[b-a for a,b in zip(stamps,stamps[1:])]
                    require((all(want<=got<want+1.5 for want,got in zip(expected,intervals))), lambda: (intervals))
                    kill(d)
                inv=inventory(path,case/'pending')
                require((inv['meta']['retired_records']==0))
                if mode=='transient': pg.sql('DROP TRIGGER test_retry ON vigil.checkpoints; DROP FUNCTION vigil.test_retry()')
                else: pg.sql('GRANT EXECUTE ON FUNCTION vigil.replay_batch(bytea) TO vigil')
                result=finish(exe,path,pg,case,inv['rows'])
                result.update(intervals_s=intervals,tokens=tokens)
                results[mode]=result
            finally: hook.close()
        finally: pg.close()
    return results

def maximum_snapshot(exe,init,root):
    print('EXPECT 16 pods: 193 unique snapshot entries, 64 readings/32 heartbeats and exactly 32 high raises/32 clears remotely; no dropped history',flush=True)
    pg=ReplayPostgres()
    try:
        path=create(root/'state',init)
        config=config_text(pods=tuple(range(1,17)),timeout=3000,status=100,rate=0)
        with daemon(exe,path,env=pg.env(),capacity=128,pipesize=1024*1024,config=config) as d,bus() as raw:
            wait_p(d,caught_up)
            for cycle in range(2):
                for pod in range(1,17):
                    send(raw,reading(pod,0,2800 if cycle==0 else 2200,0,cycle*2))
                    send(raw,reading(pod,1,1 if cycle==0 else 0,0,cycle*2+1))
                    send(raw,heartbeat(pod,1,cycle,cycle))
                d.wait(lambda s:s['valid_readings']==(cycle+1)*32 and s['heartbeats']==(cycle+1)*16)
            done=wait_p(d,lambda s:caught_up(s) and s['readings_written']==64)
            require((done['readings_written']==64 and done['readings_dropped']==done['events_dropped']==0))
            d.stop()
        inv=inventory(path,root/'finished')
        result=assert_remote(pg,inv)
        dump=result['remote']
        require((sum(h['kind']=='heartbeat' for h in dump['history'])==32))
        high=[h for h in dump['history'] if h['kind']=='alarm' and h['detail']['kind']=='high']
        require((len(high)==64 and sum(h['detail']['active'] for h in high)==32))
        snapshots=[c['alarms'] for c in dump['checkpoints'] if c['alarms'] is not None]
        require((all(len(s)==193 and len({(e['pod'],e['channel'],e['detail']['kind']) for e in s})==193 for s in snapshots)))
        result.update(snapshot_entries=193,high_raises=32,high_clears=32)
        return result
    finally: pg.close()


def capabilities(exe,init,root):
    print('EXPECT missing 002 then fsync/full_page_writes off: zero remote attempts/effects, admission continues; healthy capability restoration retries after >=30 s and drains exact bytes',flush=True)
    pg=ReplayPostgres(resilience=False)
    try:
        path=create(root/'state',init)
        with daemon(exe,path,env=pg.env()) as d,bus() as raw:
            first=wait_p(d,lambda s:s['last_error']=='55000')
            for n in range(5): send(raw,reading(1,0,2200,0,n))
            wait_local(d,lambda s:s['local_readings']==5 and s['replay_attempts']==0)
            require((pg.sql('SELECT count(*) FROM vigil.history')=='0'))
            pg.sql((Path(__file__).resolve().parents[1]/'migrations/002_resilience.sql').read_text())
            pg.sql('GRANT SELECT,INSERT ON ALL TABLES IN SCHEMA vigil TO vigil; GRANT EXECUTE ON FUNCTION vigil.validate_replay(bytea),vigil.replay_batch(bytea) TO vigil')
            for setting in ('fsync','full_page_writes'): pg.sql('ALTER SYSTEM SET '+setting+'=off')
            pg.sql('SELECT pg_reload_conf()')
            wait(d,lambda:pg.sql('SHOW fsync')=='off' and pg.sql('SHOW full_page_writes')=='off')
            second=wait_p(d,lambda s:s['connection_failures']>first['connection_failures'],timeout=35)
            require((second['last_error']=='55000'))
            local=wait_local(d,lambda s:s['replay_attempts']==0)
            require((pg.sql('SELECT count(*) FROM vigil.history')=='0'))
            require((pg.sql('SELECT count(*) FROM vigil.replay_receipts')=='0'))
            for setting in ('fsync','full_page_writes'): pg.sql('ALTER SYSTEM SET '+setting+'=on')
            pg.sql('SELECT pg_reload_conf()')
            wait(d,lambda:pg.sql('SHOW fsync')=='on' and pg.sql('SHOW full_page_writes')=='on')
            pg.sql('CHECKPOINT')
            done=wait_p(d,lambda s:caught_up(s) and s['readings_written']==5,timeout=35)
            require((done['readings_written']==5))
            d.stop()
        inv=inventory(path,root/'finished')
        result=assert_remote(pg,inv)
        result.update(first_failures=first['connection_failures'],unsafe_failures=second['connection_failures'],attempts_while_unsafe=local['replay_attempts'])
        return result
    finally: pg.close()


def retirement_accounting(exe,init,root):
    print('EXPECT lost local retirement confirmation: reconcile before new mutation/publication; every current-run/spool conservation equation holds while new readings arrive',flush=True)
    pg=ReplayPostgres()
    try:
        path=create(root/'state',init)
        env=dict(pg.env(),VIGIL_TEST_FAULT='retire_commit_unknown')
        with daemon(exe,path,env=env,capacity=64) as d,bus() as raw:
            wait_local(d,lambda s:s['local_state']=='recovering')
            for n in range(12):
                send(raw,reading(1,0,2200,0,n)); d.pump(0.02)
            wait_p(d,lambda s:s['readings_written']==12,timeout=20)
            d.stop()
            previous=None; pairs=0; uncertainty=0
            for e in d.events:
                if e['event']=='persistence': previous=e
                if e['event']=='outbox' and previous:
                    pairs+=1; uncertainty+=e['local_state']=='recovering'
                    require((e['spool_admitted']==e['durable_pending']+e['spool_retired']))
                    require((e['spool_retired']==previous['records_written']))
                    require((e['local_committed']==e['durable_pending']+previous['records_written']))
                    require((previous['readings_queued']==e['local_readings']+previous['readings_dropped']+previous['reading_depth']+previous['in_flight_readings']))
            require((pairs>1 and uncertainty>0))
        inv=inventory(path,root/'uncertain')
        result=finish(exe,path,pg,root,inv['rows'])
        require((result['readings']==12))
        result.update(consistent_status_pairs=pairs,uncertainty_samples=uncertainty)
        return result
    finally: pg.close()


def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--build',type=Path,required=True); ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--restarts',type=int,default=20)
    ap.add_argument('--timing',action='store_true')
    args=ap.parse_args(); require((args.restarts>=1))
    build=args.build.resolve(); args.output.mkdir(parents=True,exist_ok=False)
    native=Path(tempfile.mkdtemp(prefix='vigil-stage2-evidence-'))
    os.environ['VIGIL_EVIDENCE_DIR']=str(args.output.resolve())
    report=dict(binary_sha256=hashlib.sha256((build/'vigild-resilience-test').read_bytes()).hexdigest(),native_evidence=str(native),cases={})
    exe,init=build/'vigild-resilience-test',build/'vigil-outbox-init'
    def run(name,fn):
        root=native/name; root.mkdir()
        report['cases'][name]=fn(root)
        (args.output/'result.json').write_text(json.dumps(report,indent=2))
    for hook in ('after_remote_confirm_before_result','before_retire_begin','after_retire_delete_before_commit','after_retire_commit_before_publish'):
        run(hook,lambda root:cuts(exe,init,root,hook,3))
    run('rollback',lambda root:rollback(exe,init,root))
    run('ambiguity',lambda root:ambiguity(exe,init,root,args.restarts))
    run('fairness',lambda root:fairness(exe,init,root))
    run('poison',lambda root:poison(exe,init,root))
    run('outage',lambda root:outage(exe,init,root))
    run('deadlines',lambda root:deadlines(exe,init,root))
    run('maximum',lambda root:maximum_snapshot(exe,init,root))
    run('retirement_accounting',lambda root:retirement_accounting(exe,init,root))
    if args.timing:
        run('timing',lambda root:retry_timing(exe,init,root))
        run('capabilities',lambda root:capabilities(exe,init,root))
    print(f'PASS Stage 2: {len(report["cases"])} cases; {native}',flush=True)


if __name__=='__main__': main()
