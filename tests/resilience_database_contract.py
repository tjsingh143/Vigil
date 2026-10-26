#!/usr/bin/env python3
"""Real PostgreSQL v2 contract, independent of daemon and SocketCAN."""
from verification import require  # also refuses optimized execution before fixture imports
import argparse
from concurrent.futures import ThreadPoolExecutor
import copy
import json
from pathlib import Path
import subprocess
import uuid
from persistence_integration import Postgres, ROOT


class ReplayPostgres(Postgres):
    def __init__(self, resilience=True):
        super().__init__()
        if not resilience: return
        try:
            migration = (ROOT / 'migrations/002_resilience.sql').read_text()
            self.sql(migration)
            self.sql(migration)
            self.sql('GRANT SELECT,INSERT ON ALL TABLES IN SCHEMA vigil TO vigil;'
                     'GRANT EXECUTE ON FUNCTION vigil.validate_replay(bytea),vigil.replay_batch(bytea) TO vigil')
        except BaseException:
            self.close()
            raise

    def submit(self, raw, code=None):
        if isinstance(raw, dict):
            raw = encode(raw)
        cmd = self.command()
        cmd[cmd.index('-U')+1] = 'vigil'
        result = subprocess.run(cmd, input="\\set VERBOSITY verbose\nSELECT * FROM vigil.replay_batch(decode('" + raw.hex() + "','hex'));",
                                text=True, capture_output=True, timeout=8)
        if code:
            require((result.returncode != 0 and code in result.stderr), lambda: ((code, result.stderr)))
        else:
            require((result.returncode == 0), lambda: (result.stderr))
        return result.stdout.strip()

    def dump(self):
        # One statement snapshot prevents an asynchronous server commit from
        # producing a mixed before/after inventory across individual tables.
        tables = ('runs', 'run_origins', 'history', 'checkpoints', 'replay_receipts')
        fields = ','.join(
            f"'{table}',(SELECT coalesce(json_agg(t),'[]'::json) FROM "
            f"(SELECT * FROM vigil.{table} ORDER BY 1,2) t)" for table in tables)
        try:
            # Bulk evidence rendering has a separate budget from daemon SQL.
            # One coherent backlog snapshot can exceed the small-query 2s
            # fixture default; preserve the subprocess's 20s outer deadline.
            return json.loads(self.sql('SET statement_timeout=15000; SELECT json_build_object(' + fields + ')'))
        except subprocess.CalledProcessError as error:
            raise AssertionError('disposable PG snapshot export: ' + error.stderr) from error


def encode(b):
    return json.dumps(b, separators=(',', ':'), ensure_ascii=False).encode()


def payload():
    rid = str(uuid.uuid4())
    return dict(format=2, order_id=1, run_id=rid, batch_id=1, started_ms=100,
        started_utc_ms=1700000000000, mono_ms=1100, utc_ms=1700000001000,
        origin=dict(run_id=rid, spool_id=str(uuid.uuid4()), run_no=1, run_token=uuid.uuid4().hex+uuid.uuid4().hex,
                    boot_id=str(uuid.uuid4()), started_ms=100, started_utc_ms=1700000000000),
        capture_ticket=1, revision=3, kind='gap', reading_drops=7, event_drops=2,
        local_loss=dict(reading_evicted=6, reading_rejected=1, event_evicted=2, event_rejected=0),
        health=dict(readings_queued=8, events_queued=2, readings_dropped=7, events_dropped=2,
            first_drop_ms=200, last_drop_ms=300, revision=3, outage_started_ms=0, outage_ms=0,
            outage_total_ms=0, outage_ended_ms=0, connection_failures=0, query_failures=0, reconnects=0),
        records=[dict(seq=1, utc_ms=1700000000500, mono_ms=600, kind='reading', pod=1, channel=0, detail=dict(value=2800))],
        alarms=[dict(seq=0, utc_ms=1700000000600, mono_ms=700, kind='alarm', pod=1, channel=0, detail=dict(kind='high', active=True))])


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--output',type=Path)
    args=ap.parse_args()
    pg=ReplayPostgres()
    cases=[]
    def case(name):
        print('EXPECT:',name,flush=True)
        cases.append(name)
    try:
        case('001+002 applied twice; versions 1,2; fsync/full_page_writes/synchronous_commit on')
        require((pg.sql('SELECT string_agg(version::text,\',\' ORDER BY version) FROM vigil.schema_version')=='1,2'))
        for setting in ('fsync','full_page_writes','synchronous_commit'):
            require((pg.sql('SHOW '+setting)=='on'))
        case('restricted invoker runtime; no UPDATE/DELETE/DDL, PUBLIC has no new function execution')
        require((pg.sql("SELECT has_table_privilege('vigil','vigil.replay_receipts','UPDATE'),has_table_privilege('vigil','vigil.run_origins','DELETE'),has_schema_privilege('vigil','vigil','CREATE'),prosecdef FROM pg_proc WHERE oid='vigil.replay_batch(bytea)'::regprocedure")=='f|f|f|f'))
        require((pg.sql("SELECT count(*) FROM pg_proc p CROSS JOIN LATERAL aclexplode(coalesce(p.proacl,acldefault('f',p.proowner))) a WHERE p.oid IN ('vigil.replay_batch(bytea)'::regprocedure,'vigil.validate_replay(bytea)'::regprocedure) AND a.grantee=0 AND a.privilege_type='EXECUTE'")=='0'))
        case('exact receipt bytes, one run/origin/history/checkpoint; duplicate is equal with identical dump')
        b=payload(); raw=encode(b)
        require((pg.submit(raw).endswith('|1|1|inserted')))
        before=pg.dump()
        require((before['replay_receipts'][0]['payload']=='\\x'+raw.hex()))
        require(([len(before[t]) for t in before]==[1,1,1,1,1]))
        require((pg.submit(raw).endswith('|1|1|equal') and pg.dump()==before))
        case('same batch changed content or origin is VG001; no partial mutation')
        for field in ('mono_ms','origin'):
            changed=copy.deepcopy(b)
            if field=='origin': changed['origin']['run_token']='a'*64
            else: changed[field]+=1
            pg.submit(changed,'VG001')
            require((pg.dump()==before))
        case('different batch/equal history is inserted; unequal history rolls back checkpoint and receipt')
        equal=copy.deepcopy(b); equal['batch_id']=2; equal['order_id']=2
        require((pg.submit(equal).endswith('|2|1|inserted')))
        before=pg.dump()
        bad=copy.deepcopy(equal); bad['batch_id']=3; bad['records'][0]['detail']['value']=2900
        pg.submit(bad,'VG001'); require((pg.dump()==before))
        case('legacy run without origin and checkpoint without receipt are never adopted')
        legacy=payload(); rid=legacy['run_id']
        pg.sql(f"INSERT INTO vigil.runs VALUES('{rid}',to_timestamp(1700000000),100)")
        pg.submit(legacy,'VG001')
        pg.sql(f"DELETE FROM vigil.runs WHERE run_id='{rid}'")
        pg.sql(f"DELETE FROM vigil.replay_receipts WHERE run_id='{b['run_id']}' AND batch_id=2")
        pg.submit(equal,'VG001')
        case('spool/run number and token collisions roll back new runs')
        for key in ('run_no','run_token'):
            other=payload()
            if key=='run_no': other['origin']['spool_id']=b['origin']['spool_id']
            else: other['origin']['run_token']=b['origin']['run_token']
            before=pg.dump(); pg.submit(other,'VG001'); require((pg.dump()==before))
        case('malformed/missing/null/range/duplicate-key/invalid UTF8/integer syntax payloads are VG002 and write nothing')
        malformed=[encode(payload()).replace(b'\"mono_ms\":1100',b'\"mono_ms\":11e2'),b'{"x":1,"x":2}',b'\xff',b'{}',b'null',b'x',b' '*160001,
                   encode(payload()).replace(b'"format":2',b'"format":"2"')]
        for key in ('origin','records','alarms','mono_ms','health'):
            x=payload(); del x[key]; malformed.append(encode(x))
        x=payload(); x['records'][0]['seq']=0; malformed.append(encode(x))
        x=payload(); x['records'][0]['channel']=2; malformed.append(encode(x))
        x=payload(); x['records'][0]['detail']={'pad':'x'*384}; malformed.append(encode(x))
        x=payload(); x['reading_drops']=-1; malformed.append(encode(x))
        x=payload(); x['mono_ms']=9223372036854775807; malformed.append(encode(x))
        before=pg.dump()
        for raw in malformed: pg.submit(raw,'VG002'); require((pg.dump()==before))
        case('two concurrent equal callers return inserted/equal; conflicting batch and event callers have one winner')
        with ThreadPoolExecutor(max_workers=2) as pool:
            c=payload()
            responses=list(pool.map(pg.submit,[c,c]))
            require((sorted(x.rsplit('|',1)[1] for x in responses)==['equal','inserted']))
            c=payload(); d=copy.deepcopy(c); d['mono_ms']+=1
            def attempt(x):
                cmd=pg.command(); cmd[cmd.index('-U')+1]='vigil'
                return subprocess.run(cmd,input="\\set VERBOSITY verbose\nSELECT * FROM vigil.replay_batch(decode('"+encode(x).hex()+"','hex'));",text=True,capture_output=True,timeout=8)
            for event in (False,True):
                if event:
                    c=payload(); d=copy.deepcopy(c); d['batch_id']=2; d['records'][0]['detail']['value']=2900
                results=list(pool.map(attempt,[c,d]))
                require((sum(r.returncode==0 for r in results)==1))
                require(('VG001' in next(r.stderr for r in results if r.returncode)))
        case('late PostgreSQL constraint failure rolls back origin/history/checkpoint/receipt as one transaction')
        pg.sql("ALTER TABLE vigil.checkpoints ADD CONSTRAINT test_reject CHECK(kind!='gap') NOT VALID")
        before=pg.dump(); pg.submit(payload(),'23514'); require((pg.dump()==before))
        pg.sql('ALTER TABLE vigil.checkpoints DROP CONSTRAINT test_reject')
        result=dict(cases=cases,remote=pg.dump(),malformed_cases=len(malformed))
        if args.output:
            args.output.mkdir(parents=True,exist_ok=False)
            (args.output/'result.json').write_text(json.dumps(result,indent=2))
        print(f'PASS PostgreSQL v2: {len(cases)} contracts; {len(malformed)} malformed cases',flush=True)
    finally:
        pg.close()


if __name__=='__main__': main()
