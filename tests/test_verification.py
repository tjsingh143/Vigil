"""Negative controls for F3: a verifier must fail before it can bless bad evidence."""
from verification import require
import ast
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch, Mock

ROOT = Path(__file__).resolve().parents[1]


class VerificationTests(unittest.TestCase):
    def test_all_entries_refuse_optimization_before_side_effects(self):
        print("EXPECT: every Python test entry refuses -O and PYTHONOPTIMIZE=1 before fixture imports/actions", flush=True)
        # A child audit hook rejects filesystem mutation, process launch and
        # sockets. Refusal must come from our guard, never this tripwire.
        probe = """
import sys, runpy, os
sys.path.insert(0, sys.argv[1])
def audit(event, args):
    if event in ('subprocess.Popen','os.mkdir','os.remove','os.rename','os.rmdir','socket.__new__','os.system'):
        raise RuntimeError('SIDE_EFFECT_TRIPWIRE: '+event)
    if event == 'open':
        mode, flags = args[1], args[2]
        if (isinstance(mode,str) and any(c in mode for c in 'wax+')) or (isinstance(flags,int) and flags & (os.O_WRONLY|os.O_RDWR|os.O_CREAT|os.O_TRUNC)):
            raise RuntimeError('SIDE_EFFECT_TRIPWIRE: write')
sys.addaudithook(audit)
runpy.run_path(sys.argv[2], run_name='__main__')
"""
        for path in sorted((ROOT / 'tests').glob('*.py')):
            for flag in ('-O', 'env'):
                with self.subTest(entry=path.name, mode=flag):
                    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
                    env.pop('PYTHONOPTIMIZE', None)
                    if flag == 'env': env['PYTHONOPTIMIZE'] = '1'
                    result = subprocess.run([sys.executable, '-B', *(['-O'] if flag == '-O' else []),
                                             '-c', probe, str(ROOT/'tests'), str(path)],
                                            env=env, capture_output=True, text=True, timeout=10)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn('verification refuses Python optimization', result.stderr)
                    self.assertNotIn('SIDE_EFFECT_TRIPWIRE', result.stderr)
                    self.assertEqual(result.stdout, '')

    def test_no_optimization_sensitive_checks(self):
        for path in (ROOT/'tests').glob('*.py'):
            self.assertFalse(any(isinstance(n, ast.Assert) for n in ast.walk(ast.parse(path.read_text()))), path)
        called = []
        require(True, lambda: called.append('unexpected diagnostic'))
        self.assertEqual(called, [])
        with self.assertRaisesRegex(AssertionError, 'invalid expected result'):
            require(False, lambda: 'invalid expected result')

    def test_inconsistent_inventory(self):
        from resilience_integration import inventory
        print('EXPECT: inconsistent SQLite inventory cannot pass', flush=True)
        with tempfile.TemporaryDirectory(prefix='vigil-oracle-negative-') as directory:
            path = Path(directory)/'outbox.sqlite3'
            with sqlite3.connect(path) as db:
                db.executescript('''
                    CREATE TABLE spool_meta(pending_records,pending_payload_bytes,pending_batches,
                      admitted_records,retired_records,retired_readings,last_retired_order,next_order_id);
                    INSERT INTO spool_meta VALUES(1,0,0,1,0,0,0,1);
                    CREATE TABLE outbox_batches(order_id);
                    CREATE TABLE origin_runs(run_no);
                ''')
            with self.assertRaises(AssertionError):
                inventory(path, Path(directory)/'copy')

    def test_nonzero_child_is_recorded_and_raises(self):
        from verify_resilience import run_command
        print('EXPECT: actual child exit 17 is retained and fails the release runner', flush=True)
        with tempfile.TemporaryDirectory(prefix='vigil-runner-negative-') as directory:
            out=Path(directory); report={'commands':[]}
            with self.assertRaisesRegex(AssertionError, 'failed'):
                run_command(out, report, os.environ.copy(), 'negative child must fail',
                            [sys.executable, '-c', 'raise SystemExit(17)'])
            self.assertEqual(json.loads((out/'gate.json').read_text())['commands'][0]['exit_code'], 17)

    def test_wrong_fixture_identity(self):
        from resilience_adversarial import cluster
        print('EXPECT: wrong PG fixture identity rejects before the destructive body', flush=True)
        fake=Mock(data=Path('/tmp/vigil-pg-fixture/data'))
        fake.sql.return_value='/tmp/not-the-fixture/data'
        entered=[]
        with patch('resilience_adversarial.ReplayPostgres', return_value=fake):
            with self.assertRaises(AssertionError):
                with cluster(): entered.append(True)
        self.assertEqual(entered, [])

    def test_invalid_expected_sql_result(self):
        from resilience_database_contract import ReplayPostgres
        print('EXPECT: unexpected SQL success cannot satisfy an expected VG001 failure', flush=True)
        pg=object.__new__(ReplayPostgres)
        with patch.object(pg, 'command', return_value=['psql','-U','fixture']), \
             patch('resilience_database_contract.subprocess.run', return_value=subprocess.CompletedProcess([],0,'inserted','')):
            with self.assertRaises(AssertionError): pg.submit(b'{}', 'VG001')


if __name__ == '__main__':
    unittest.main()
