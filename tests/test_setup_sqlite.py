"""F5 controls: SQLite source verification cannot be disabled by Python optimization.

Commands are stubbed in a disposable directory; nothing is downloaded, compiled
or installed. Set VIGIL_SQLITE_GENUINE_SOURCE to an official
sqlite-autoconf-3510300.tar.gz to also run the genuine-source positive cases.
"""
from verification import require  # also refuses optimized execution before fixture imports
import hashlib
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'scripts' / 'setup-sqlite'
PINNED = '32d5424f97e0a7fc5ed2f6335afbb58be4e0298bd7117a34e39d345ff13d859e'
PREFIX_LINE = 'prefix=/opt/vigil-sqlite-3.51.3\n'
GENUINE = os.environ.get('VIGIL_SQLITE_GENUINE_SOURCE')
OPTIMIZED = {'-O': (['-O'], {}), '-OO': (['-OO'], {}),
             'PYTHONOPTIMIZE=1': ([], {'PYTHONOPTIMIZE': '1'}), 'PYTHONOPTIMIZE=2': ([], {'PYTHONOPTIMIZE': '2'})}
REFUSAL = 'refuses Python optimization'
STUBS = {
    'uname': 'echo Linux',
    'mktemp': 'mkdir -p -- "$STUB_ROOT/build" && echo "$STUB_ROOT/build"',
    'curl': 'while [ $# -gt 0 ]; do [ "$1" = -o ] && cp -- "$STUB_TARBALL" "$2"; shift; done',
    'tar': 'exec "$STUB_TAR" "$@"',
    'make': ':', 'sudo': ':',
    'python3': 'exec "$STUB_PYTHON" $STUB_PYFLAGS "$@"',
}


def oracle_body():
    text = SCRIPT.read_text()
    require(text.count("<<'PY'\n") == 1, lambda: 'expected exactly one embedded Python oracle')
    return text.split("<<'PY'\n", 1)[1].split('\nPY\n', 1)[0] + '\n'


def source_tarball(path, sqlite3_c):
    """A disposable autoconf-shaped archive whose configure only records that it ran."""
    configure = b'#!/bin/sh\nprintf \'configure %s\\n\' "$*" >> "$STUB_LOG"\n'
    with tarfile.open(path, 'w:gz') as archive:
        for name, data, mode in (('sqlite3.c', sqlite3_c, 0o644), ('configure', configure, 0o755)):
            info = tarfile.TarInfo('sqlite-autoconf-3510300/' + name)
            info.size, info.mode = len(data), mode
            archive.addfile(info, io.BytesIO(data))


def genuine_sqlite3_c():
    with tarfile.open(GENUINE) as archive:
        data = archive.extractfile('sqlite-autoconf-3510300/sqlite3.c').read()
    require(hashlib.sha3_256(data).hexdigest() == PINNED,
            lambda: f'{GENUINE} is not the genuine pinned SQLite source')
    return data


class SetupSqliteTests(unittest.TestCase):
    def run_oracle(self, directory, source, flags=(), extra_env=None):
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
        env.pop('PYTHONOPTIMIZE', None)
        env.update(extra_env or {})
        return subprocess.run([sys.executable, '-B', *flags, '-', str(source)], input=oracle_body(),
                              cwd=directory, env=env, capture_output=True, text=True, timeout=60)

    def run_installer(self, directory, sqlite3_c, flags=(), extra_env=None, isolate_prefix=True):
        """Run the tracked installer with stubbed commands; return (result, logged commands)."""
        root = Path(directory)
        bin_dir, log = root / 'bin', root / 'commands.log'
        bin_dir.mkdir()
        for name, body in STUBS.items():
            stub = bin_dir / name
            stub.write_text(f'#!/bin/sh\nprintf \'{name} %s\\n\' "$*" >> "$STUB_LOG"\n{body}\n')
            stub.chmod(0o755)
        source_tarball(root / 'source.tar.gz', sqlite3_c)
        script = SCRIPT
        if isolate_prefix:
            # Only the install prefix differs, so an existing /opt install cannot
            # short-circuit the download/verification path under test.
            text = SCRIPT.read_text()
            require(text.count(PREFIX_LINE) == 1, lambda: 'expected exactly one install prefix line')
            script = root / 'setup-sqlite'
            script.write_text(text.replace(PREFIX_LINE, f'prefix={root / "prefix"}\n'))
        env = dict(os.environ, PATH=f'{bin_dir}{os.pathsep}{os.environ["PATH"]}', STUB_LOG=str(log),
                   STUB_ROOT=str(root), STUB_TARBALL=str(root / 'source.tar.gz'), STUB_TAR=shutil.which('tar'),
                   STUB_PYTHON=sys.executable, STUB_PYFLAGS=' '.join(flags), PYTHONDONTWRITEBYTECODE='1')
        env.pop('PYTHONOPTIMIZE', None)
        env.update(extra_env or {})
        result = subprocess.run(['bash', str(script)], cwd=root, env=env, capture_output=True, text=True, timeout=60)
        commands = log.read_text().splitlines() if log.exists() else []
        self.assertFalse((root / 'build').exists(), 'temporary build directory must be removed')
        return result, commands

    def assert_no_build(self, commands):
        for forbidden in ('configure', 'make', 'sudo'):
            self.assertFalse([c for c in commands if c.split()[0] == forbidden], (forbidden, commands))

    def test_pinned_digest_is_unchanged(self):
        self.assertEqual(SCRIPT.read_text().count(PINNED), 1)
        self.assertIn(f"expected = '{PINNED}'\n", oracle_body())

    def test_embedded_oracle_rejects_wrong_source_in_every_mode(self):
        print('EXPECT: the exact embedded oracle rejects wrong source in normal and all optimized modes', flush=True)
        with tempfile.TemporaryDirectory(prefix='vigil-sqlite-oracle-') as directory:
            bad = Path(directory) / 'sqlite3.c'
            bad.write_bytes(b'/* Deliberately invalid SQLite source. Never compiled. */\n')
            result = self.run_oracle(directory, bad)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn('SQLite source mismatch', result.stderr)
            for mode, (flags, extra_env) in OPTIMIZED.items():
                with self.subTest(mode=mode):
                    result = self.run_oracle(directory, bad, flags, extra_env)
                    self.assertNotEqual(result.returncode, 0, result.stderr)
                    self.assertIn(REFUSAL, result.stderr)

    def test_wrong_source_never_reaches_configure_build_or_install(self):
        print('EXPECT: wrong source stops before configure/make/sudo in every interpreter mode', flush=True)
        bad = b'/* Deliberately invalid SQLite source. Never compiled. */\n'
        with tempfile.TemporaryDirectory(prefix='vigil-sqlite-installer-') as directory:
            result, commands = self.run_installer(directory, bad)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('SQLite source mismatch', result.stderr)
            self.assertTrue([c for c in commands if c.startswith('curl ')], commands)
            self.assert_no_build(commands)
        for mode, (flags, extra_env) in OPTIMIZED.items():
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(prefix='vigil-sqlite-installer-') as directory:
                result, commands = self.run_installer(directory, bad, flags, extra_env)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(REFUSAL, result.stderr)
                self.assert_no_build(commands)

    def test_optimized_installer_refuses_before_side_effects(self):
        print('EXPECT: the unmodified installer refuses optimization before mktemp/download/extract/build', flush=True)
        for mode, (flags, extra_env) in OPTIMIZED.items():
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(prefix='vigil-sqlite-refusal-') as directory:
                result, commands = self.run_installer(directory, b'unused', flags, extra_env, isolate_prefix=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(REFUSAL, result.stderr)
                self.assertEqual([c.split()[0] for c in commands], ['uname', 'python3'])

    @unittest.skipUnless(GENUINE, 'set VIGIL_SQLITE_GENUINE_SOURCE to the official sqlite-autoconf-3510300.tar.gz')
    def test_genuine_source_passes_only_without_optimization(self):
        print('EXPECT: genuine source passes normally and reaches configure, make, install; optimization refuses', flush=True)
        genuine = genuine_sqlite3_c()
        with tempfile.TemporaryDirectory(prefix='vigil-sqlite-genuine-') as directory:
            source = Path(directory) / 'sqlite3.c'
            source.write_bytes(genuine)
            result = self.run_oracle(directory, source)
            self.assertEqual((result.returncode, result.stdout, result.stderr), (0, '', ''))
            for mode, (flags, extra_env) in OPTIMIZED.items():
                with self.subTest(oracle=mode):
                    result = self.run_oracle(directory, source, flags, extra_env)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(REFUSAL, result.stderr)
        with tempfile.TemporaryDirectory(prefix='vigil-sqlite-genuine-') as directory:
            result, commands = self.run_installer(directory, genuine)
            self.assertEqual(result.returncode, 0, result.stderr)
            build = [c for c in commands if c.split()[0] in ('configure', 'make', 'sudo')]
            self.assertEqual(build, [f'configure --prefix={Path(directory) / "prefix"} --disable-readline',
                                     'make -j2', 'sudo make install'])
        for mode, (flags, extra_env) in OPTIMIZED.items():
            with self.subTest(installer=mode), tempfile.TemporaryDirectory(prefix='vigil-sqlite-genuine-') as directory:
                result, commands = self.run_installer(directory, genuine, flags, extra_env)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(REFUSAL, result.stderr)
                self.assertEqual([c.split()[0] for c in commands], ['uname', 'python3'])


if __name__ == '__main__':
    unittest.main()
