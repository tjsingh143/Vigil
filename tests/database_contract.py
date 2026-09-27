#!/usr/bin/env python3
"""Non-vcan PostgreSQL integration: actual schema, runtime grants and transactions."""
import copy
import json
import subprocess
import sys
import uuid
from persistence_integration import Postgres


def main():
    if sys.platform != 'linux' or sys.flags.optimize:
        raise RuntimeError('real Linux required; assertions must remain enabled')
    print('EXPECT schema: initialize twice, fsync/synchronous_commit on, version=1', flush=True)
    pg = Postgres()
    try:
        assert pg.sql('SHOW fsync') == 'on'
        assert pg.sql('SHOW synchronous_commit') == 'on'
        assert pg.sql('SELECT version FROM vigil.schema_version') == '1'
        print('PASS schema initialization and database durability settings')
        rid = str(uuid.uuid4())
        batch = dict(run_id=rid, batch_id=1, started_utc_ms=1700000000000,
                     started_ms=100, utc_ms=1700000001000, mono_ms=1100, kind='gap',
                     reading_drops=7, event_drops=0, health={'connected': True},
                     alarms=[{'detail': {'kind': 'high', 'active': True}}],
                     records=[dict(seq=1, utc_ms=1700000000500, mono_ms=600,
                                   kind='reading', pod=1, channel=0, detail={'value': 2800})])
        command = pg.command()
        command[command.index('-U') + 1] = 'vigil'
        def submit(payload, expected_ok=True):
            sql = "SELECT vigil.write_batch($payload$" + json.dumps(payload) + "$payload$::jsonb);"
            result = subprocess.run(command, input=sql, text=True, capture_output=True, timeout=5)
            assert (result.returncode == 0) == expected_ok, result.stderr
            return result
        print('EXPECT runtime writer: minimal role writes one reading, timestamped gap and alarm snapshot', flush=True)
        submit(batch)
        assert pg.sql(f"SELECT pod,channel,detail->>'value',monotonic_ms,"
                      f"(extract(epoch FROM observed_at)*1000)::bigint FROM vigil.history WHERE run_id='{rid}'") == '1|0|2800|600|1700000000500'
        assert pg.sql(f"SELECT kind,reading_drops,event_drops,alarms->0->'detail'->>'active' "
                      f"FROM vigil.checkpoints WHERE run_id='{rid}'") == 'gap|7|0|true'
        print('PASS runtime write, UTC/monotonic timestamp, exact gap and snapshot')
        print('EXPECT retry: identical committed batch replay leaves one history row/checkpoint and seven drops', flush=True)
        submit(batch)
        assert pg.sql(f"SELECT count(*) FROM vigil.history WHERE run_id='{rid}'") == '1'
        assert pg.sql(f"SELECT count(*),sum(reading_drops) FROM vigil.checkpoints WHERE run_id='{rid}'") == '1|7'
        print('PASS idempotent committed-batch replay')
        print('EXPECT rollback: invalid checkpoint rolls back its otherwise valid new run and reading', flush=True)
        invalid = copy.deepcopy(batch)
        invalid['run_id'] = str(uuid.uuid4())
        invalid['reading_drops'] = -1
        failed = submit(invalid, False)
        assert 'check constraint' in failed.stderr, failed.stderr
        assert pg.sql(f"SELECT count(*) FROM vigil.runs WHERE run_id='{invalid['run_id']}'") == '0'
        assert pg.sql(f"SELECT count(*) FROM vigil.history WHERE run_id='{invalid['run_id']}'") == '0'
        print('PASS whole-batch rollback')
        print('EXPECT runtime permissions: no UPDATE/DELETE/CREATE rights, invoker function', flush=True)
        assert pg.sql("SELECT has_table_privilege('vigil','vigil.history','UPDATE'),"
                      "has_table_privilege('vigil','vigil.history','DELETE'),"
                      "has_schema_privilege('vigil','vigil','CREATE'),"
                      "prosecdef FROM pg_proc WHERE oid='vigil.write_batch(jsonb)'::regprocedure") == 'f|f|f|f'
        print('PASS restricted runtime permissions; database contract 5/5')
    finally:
        pg.close()


if __name__ == '__main__':
    main()
