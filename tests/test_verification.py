"""Negative controls for F3: a verifier must fail before it can bless bad evidence."""
from verification import require
import ast
import json
import os
from pathlib import Path
import re
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch, Mock

ROOT = Path(__file__).resolve().parents[1]
OPTIMIZED = (('-O', ['-O'], None), ('-OO', ['-OO'], None),
             ('PYTHONOPTIMIZE=1', [], '1'), ('PYTHONOPTIMIZE=2', [], '2'))
# A child audit hook rejects filesystem mutation, process launch and
# sockets. Refusal must come from our guard, never this tripwire.
PROBE = """
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
# Python launched from shell scripts, workflows or build files. `python3-venv`
# (a package name) and paths ending in .../python are not invocations.
INTERPRETER = re.compile(r'(?<![\w./-])(?:\.venv/bin/python|python3?)(?![\w.-])')
EMBEDDED = re.compile(r"(?<![\w./-])(?:\.venv/bin/python|python3?) (?:-c '([^']*)'|- [^\n]*<<'(\w+)'\n(.*?)\n\2\n)", re.S)
# Invocations whose Python is protected elsewhere (test entries, discovery) or
# that run no verification oracle (environment/corpus tools, AST-checked).
CLASSIFIED = (r' tests/\w+\.py\b', r' -m unittest discover -s tests\b', r' -c \'', r' - .*<<\'\w+\'$',
              r' -m venv \.venv$', r' -m pip install -r simulator/requirements\.txt$', r' fuzz/seed\.py ')


def tracked(*suffixes, exclude=()):
    """The digest-covered implementation files (scripts, workflows, build/service files, tests...)."""
    from evidence import implementation_files
    return [ROOT / n for n in implementation_files() if (ROOT / n).is_file()
            and (not suffixes or n.endswith(suffixes)) and not n.endswith(exclude)]


def embedded_programs():
    """Every Python program text embedded in a non-Python file, keyed by location."""
    programs = {}
    for path in tracked(exclude=('.py', '.c', '.h', '.md', '.pdf')):
        for n, match in enumerate(EMBEDDED.finditer(path.read_text())):
            programs[f'{path.relative_to(ROOT)}#{n}'] = match.group(1) if match.group(1) is not None else match.group(3) + '\n'
    return programs


class VerificationTests(unittest.TestCase):
    def test_all_entries_refuse_optimization_before_side_effects(self):
        print("EXPECT: every Python test entry refuses -O, -OO and PYTHONOPTIMIZE=1/2 before fixture imports/actions", flush=True)
        for path in sorted((ROOT / 'tests').glob('*.py')):
            for mode, flags, value in OPTIMIZED:
                with self.subTest(entry=path.name, mode=mode):
                    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
                    env.pop('PYTHONOPTIMIZE', None)
                    if value: env['PYTHONOPTIMIZE'] = value
                    result = subprocess.run([sys.executable, '-B', *flags,
                                             '-c', PROBE, str(ROOT/'tests'), str(path)],
                                            env=env, capture_output=True, text=True, timeout=10)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn('verification refuses Python optimization', result.stderr)
                    self.assertNotIn('SIDE_EFFECT_TRIPWIRE', result.stderr)
                    self.assertEqual(result.stdout, '')

    def test_embedded_python_refuses_optimization_before_side_effects(self):
        print("EXPECT: every Python invocation in shell/workflow files is classified; every embedded program refuses optimization before side effects", flush=True)
        unclassified, program_invocations = [], 0
        for path in tracked(exclude=('.py', '.c', '.h', '.md', '.pdf')):
            for line in path.read_text().splitlines():
                if line.lstrip().startswith('#'):
                    continue  # shell/YAML/CMake comment, never executed
                for match in INTERPRETER.finditer(line):
                    rest = line[match.end():]
                    if not any(re.match(p, rest) for p in CLASSIFIED):
                        unclassified.append(f'{path.relative_to(ROOT)}: {line.strip()}')
                    program_invocations += bool(re.match(r' -c \'| - .*<<\'\w+\'$', rest))
        self.assertEqual(unclassified, [])
        programs = embedded_programs()
        # Every inline program found by the line scan must also be extracted and run below.
        self.assertEqual(program_invocations, len(programs), programs)
        self.assertTrue([k for k in programs if k.startswith('scripts/setup-sqlite#')], programs)
        with tempfile.TemporaryDirectory(prefix='vigil-embedded-python-') as directory:
            for location, program in programs.items():
                source = Path(directory) / (location.replace('/', '_').replace('#', '_') + '.py')
                source.write_text(program)
                for mode, flags, value in OPTIMIZED:
                    with self.subTest(program=location, mode=mode):
                        env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
                        env.pop('PYTHONOPTIMIZE', None)
                        if value: env['PYTHONOPTIMIZE'] = value
                        result = subprocess.run([sys.executable, '-B', *flags, '-c', PROBE, str(ROOT/'tests'), str(source)],
                                                cwd=ROOT, env=env, capture_output=True, text=True, timeout=10)
                        self.assertNotEqual(result.returncode, 0)
                        self.assertIn('refuses Python optimization', result.stderr)
                        self.assertNotIn('SIDE_EFFECT_TRIPWIRE', result.stderr)
                        self.assertEqual(result.stdout, '')

    def test_no_optimization_sensitive_checks(self):
        sources = {str(p.relative_to(ROOT)): p.read_text() for p in tracked('.py')}
        sources.update(embedded_programs())
        self.assertTrue([k for k in sources if k.startswith('scripts/setup-sqlite#')], sources.keys())
        for name, text in sources.items():
            self.assertFalse(any(isinstance(n, ast.Assert) for n in ast.walk(ast.parse(text))), name)
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
