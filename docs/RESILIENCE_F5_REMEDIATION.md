# Re-audit finding F5 remediation — 2026-10-27

The independent re-audit of `be41eb1b8d4472a740c19e312d1a1314202a20d9` concluded
**FAIL — REMEDIATION REQUIRED**. It confirmed F1, F2 and F4 resolved and the 539
F3 assertion conversions real, but found **F5 (MEDIUM, release blocking)**: the
SQLite source-integrity check embedded in `scripts/setup-sqlite` disappeared under
Python optimization. Its report and evidence remain unchanged under
`artifacts/independent-reaudit-20261026/`.

This remediation began at that exact commit with a clean tracked tree. It was
carried out separately from the independent audit that followed. This record is
the remediation's own verification, not independent approval. No deployment,
service installation or production-data change was performed.

**Remediation checkpoint PASS — ready for a fresh independent audit.** That
audit approved the result; see [RESILIENCE_AUDIT.md](RESILIENCE_AUDIT.md).
The complete clean Linux gate passed 72/72 commands on the exact final
implementation digest. New evidence: `artifacts/remediation-f5-20261027/`
(ignored by Git, like earlier raw evidence).

## Root cause

`scripts/setup-sqlite` lines 13–17 (`be41eb1`, script SHA-256
`64f84356645993f901fe60e82a76c8b95a212290bafed0a65515b376dd4e8faf`) computed and
compared the pinned SHA3-256 entirely inside a Python `assert`. Under `-O`, `-OO`
or an inherited `PYTHONOPTIMIZE`, CPython removes assert statements, including
the file read; the heredoc exited 0, so `set -e` could not stop the shell, and
`./configure`, `make -j2` and `sudo make install` followed. Nothing refused an
optimized interpreter before the download.

The F3 remediation missed it because it defined the verification surface by
location and extension: its refusal test and AST scan both enumerated
`tests/*.py`. Python embedded in a shell heredoc is neither. The gate's `bash -n`
step proves syntax only. In addition, the Linux VM already has the pinned SQLite
installed, so `setup-sqlite` exits at its install check and no gate run ever
executed the verification path; the defect was latent rather than observed.

## Correction

`scripts/setup-sqlite` is the only changed production/provisioning file:

- Immediately after the Linux check, and before the install check, `mktemp`,
  download, extraction or build, `python3 -c` exits nonzero with
  `setup-sqlite refuses Python optimization (-O/PYTHONOPTIMIZE)` whenever the
  interpreter that will run the check is optimized. It uses Python's own
  resolution of `-O`/`PYTHONOPTIMIZE` (`PYTHONOPTIMIZE=0` is not optimization; a
  `python3` wrapper adding `-O` is refused).
- The embedded verifier repeats that refusal, then compares the digest with an
  explicit `if` and exits 1 with both values on mismatch. `set -e` stops before
  configure/build/install and the EXIT trap removes the temporary directory.
- The pinned SQLite 3.51.3 `sqlite3.c` SHA3-256
  `32d5424f97e0a7fc5ed2f6335afbb58be4e0298bd7117a34e39d345ff13d859e` is unchanged.
  It is the value published in the official
  [3.51.3 release log](https://www.sqlite.org/releaselog/3_51_3.html), and the
  official `sqlite-autoconf-3510300.tar.gz` downloaded during this work (SHA-256
  `81f5be397049b0cae1b167f2225af7646fc0f82e4a9b3c48c9ea3a533e21d77a`) contains a
  `sqlite3.c` with exactly that digest.

No C source, header, SQL migration, service unit, configuration, CMake or workflow
file changed (`git diff be41eb1 -- src include migrations systemd config
CMakeLists.txt tools simulator fuzz env .github` is empty), so the F1/F2 runtime
repairs and F4 installer guidance are untouched.

## Regression coverage

`tests/test_setup_sqlite.py` (new, five unittest cases) runs the tracked installer
through stub `uname`, `mktemp`, `curl`, `tar`, `make`, `sudo` and `python3` commands
in a disposable directory. Stubs only log their arguments (`tar` delegates to the
real tar on a disposable archive whose `configure` merely logs). Nothing is
downloaded, compiled or installed; the temporary build directory must be gone
afterwards. Oracles are exit codes, stderr and the logged command sequence.

| Case | Required observable result |
| --- | --- |
| Pinned digest unchanged | Exactly one occurrence; the oracle's `expected` line is the legitimate value |
| Exact embedded oracle, wrong source | Normal: exit 1, `SQLite source mismatch`. `-O`, `-OO`, `PYTHONOPTIMIZE=1`, `PYTHONOPTIMIZE=2`: nonzero refusal |
| Installer, wrong source, every mode | Never reaches `configure`, `make` or `sudo`; normal mode reaches `curl` (so the verification path really ran) and reports the mismatch |
| Unmodified installer, every optimized mode | Refuses after only `uname` and the `python3` guard: no `mktemp`, download, extraction or build |
| Genuine source (when `VIGIL_SQLITE_GENUINE_SOURCE` names the official tarball) | Normal: oracle exit 0 and installer reaches `configure`, `make -j2`, `sudo make install` in order; every optimized mode refuses the oracle and the installer before side effects |

Wrong-source installer cases replace exactly one line, the install prefix, with a
disposable path; otherwise an existing `/opt` install would end the script before
the download path under test. The optimized-refusal case runs the unmodified file.
The genuine case is skipped, visibly, when no official tarball is supplied; the
release gate does not download it. It was run separately on macOS and Linux.

`tests/test_verification.py` extends the F3 controls instead of replacing them:

- every `tests/*.py` entry (24, including the new module) must refuse `-O`, `-OO`,
  `PYTHONOPTIMIZE=1` and `PYTHONOPTIMIZE=2` before the audit-hook tripwire sees a
  write, process or socket;
- a new inventory scans every digest-covered non-Python implementation file
  (scripts, workflow, CMake, service and configuration files). Every Python
  invocation must be a tests entry point, unittest discovery, an inline program,
  or an allowlisted non-verification tool (`venv`, `pip`, `fuzz/seed.py`). Every
  inline program (`-c` or heredoc) must be extracted, their counts must agree, and
  each must refuse all four optimization modes before side effects;
- the no-`assert` AST scan covers every tracked Python file and every inline program.

`tests/evidence.py` exposes its implementation file list so the inventory scans
exactly the files the digest covers; the digest itself is byte-identical before
and after this refactor.

## Before and after

All installer runs are stubbed. "Before" runs the original `be41eb1` script copy.

| Control | Original `be41eb1` | Corrected |
| --- | --- | --- |
| Embedded oracle, wrong source, normal | Exit 1, mismatch | Exit 1, mismatch |
| Embedded oracle, wrong source, 4 optimized modes | **Exit 0: wrong source accepted** (4/4) | Exit 1, refusal (4/4) |
| Prefix-isolated installer, wrong source, normal | Exit 1 before configure | Exit 1 before configure |
| Prefix-isolated installer, wrong source, 4 optimized modes | **Exit 0; configure, `make -j2`, `sudo make install` reached** (4/4) | Exit 1, refused before `mktemp`/download; no build (4/4) |
| Unmodified installer in the VM (SQLite installed), 4 optimized modes | Exit 0 at the install check: the defect is hidden, not fixed | Exit 1, refused before the install check |
| Unmodified installer in the VM, normal | Exit 0 at the install check | Exit 0 at the install check (intended no-op) |
| Re-audit probe `check_setup_guard.py` (unmodified copy) | Exit 1 (re-audit evidence) | Exit 0: normal, `-O` and `PYTHONOPTIMIZE=1` all reject |
| New and extended unittest suite, Linux | 17 failures in 5 tests: 16 optimized-mode subtests and the no-`assert` scan | 19 tests OK, 1 genuine skip |

The macOS host gave the same before/after pattern except that, with no `/opt`
install there, the unmodified original script also reached the stubbed build in
every optimized mode. Three mutation controls (an unextracted heredoc, an
unclassified `python3 -B -c`, and an embedded `assert` digest check) each failed
the new scans; removing them restored a passing tree.

## Embedded-Python and related verification coverage

The final scan covers 26 implementation files, classifies 31 Python invocations,
extracts four inline programs (the installer's guard and verifier, and the two
`check-ci` decoder probes, which refuse through `tests/verification.py`), and
AST-checks 28 tracked Python files. `fuzz/seed.py` writes the fuzz corpus and
`simulator/` produces traffic; neither is a verification oracle, and neither
contains `assert`. The only C `assert` use is the libFuzzer target, which CMake
compiles with `-UNDEBUG` regardless of build type, so the C analogue of `-O`
cannot remove those checks. Local Semgrep (`cc-sast --changed`, 330 rules) scanned
the four changed files and reported no findings; ShellCheck is not installed and
was not run.

## Full clean Linux qualification

Following the established procedure, the gate ran from a fresh persistent
native-ext4 clone (`/var/tmp/vigil-f5-hc0nggdq/source`) containing every tracked
file plus the untracked digest-covered implementation files of the working tree;
its only other untracked item is the declared `.venv` symlink. Its digest equalled
the working tree's before the gate and after it. `TMPDIR` was inside that root; the
host held a process-scoped `caffeinate -i`. Expectations were recorded first.

```sh
caffeinate -i ./scripts/dev bash artifacts/remediation-f5-20261027/run_final_gate.sh
# runs, in the native clone:
./scripts/check-resilience --clean --output artifacts/remediation-f5-20261027/linux-full
```

**PASS: 72/72 commands exited 0**, on `Linux-6.8.0-147-generic-aarch64-with-glibc2.39`,
from 2026-10-27T05:01:18.160805+00:00 through 2026-10-27T05:33:26.635601+00:00,
with `python_optimize` 0 and implementation SHA-256
`e755cbdd42ad02b96dff060bb66b0ccf59eb5c0491f6cbad91137238ee966250` at start and end.
The gate records parent HEAD `be41eb1` because, as in the previous remediation, it
ran before the local commit; the content digest identifies the tested tree. Results
below were read from the logs and result files, not inferred from the aggregate exit.

| Required verification | Observed result |
| --- | --- |
| Python | 19 unittest cases in the gate and again in its `check-ci` step: OK, 1 skipped (genuine source not supplied to the gate); all F5 and F3 controls pass |
| C | 67 cmocka cases (13 protocol, 26 monitor, 22 outbox, 6 queue) in each of Debug, Release, ASan/UBSan, TSan and both CI builds; 409 passes including Helgrind runs, 0 failures |
| F1/F2 coordinator | 120/120 scenarios (30 per build), including 64 reconciliation SIGKILL cuts |
| Local and remote durability | Stage 1: 13 cases per build; Stage 2: 14 Debug / 12 per other build; Debug repeats 20 restarts |
| Adversarial | 75/75: 60 Debug seeds plus server-transaction kill, damaged copies and the large backlog, and 4 seeds in each of Release, ASan/UBSan and TSan |
| Large backlog | 6,128 exact remote readings (6,000 offered plus 128 during replay), 189 batches, startup 0.324 s, drain 4.582 s, stale age 506 ms |
| SQL | PostgreSQL v1 5/5 contracts; v2 10 contracts plus 18 malformed inputs |
| Monitoring | Real SocketCAN/vcan decoder regression in Debug, Release and ASan/UBSan and live monitoring regression in all four builds; silent, malformed, non-advancing, counter-jump, hysteresis, high-rate and receive-overflow scenarios; can-utils exchange |
| Storage/service | Real ext4 ENOSPC and read-only, unprivileged hardened systemd readiness, watchdog and SIGKILL/restart passed |
| Dynamic/static | No ASan/UBSan/LSan/TSan report in any log; Helgrind queue and handoff 0 errors; zero compiler warnings in all five strict builds, including the GCC analyzer |
| Fuzz | Decoder libFuzzer 91,109,714 inputs in 121 s; CI smoke 8,047,832 in 11 s; no finding |
| Demo | 20/20 offered readings remote, 45 history rows, 29 receipts; after an ambiguous commit, 2 identical submissions produced one receipt/history effect; zero pending or known losses |
| Benchmark | Six paced trials and six bursts passed with exact counts; see [performance](RESILIENCE_PERFORMANCE.md) |
| Shell/diff | All 15 scripts pass `bash -n`; `git diff --check` passed |

The only skips are the genuine-source case and CMake's cached compiler checks.
Native evidence was archived automatically: 17 roots, 15,414,635 bytes, SHA-256
`31cb5716310a95933b348650813edcb9d41f80f5a461c354694576aa79e09284`. Path-only copies
of the previous remediation's archive checkers then reopened **1,339** stopped
SQLite inventories and reconciled **607** PostgreSQL exports and **1,165** distinct
spool/order identities with **no inconsistency**, recomputed every benchmark
summary from its raw trials, and confirmed the host working tree's digest equals
the gate's. This is author evidence review, not an independent audit.

The benchmarked production binaries are byte-identical to the previous gates'
(Resilience `5c423d2f...`, original `8e60df7d...`). This run measured a lower remote
window median (74.21/s versus 83.86/s in the previous final gate) and higher daemon
CPU (19.31% versus 12.86%); with an unchanged executable, that is run-to-run
variation on the shared VM, and it is reported rather than replaced.

Post-gate Linux review: no test process, fixture mount, loop device or transient
unit; default `vigild` inactive and disabled; PostgreSQL 16/main online; ASLR 2;
the benchmark's baseline worktree removed. The first review attempt reported
"not clean" only because its `/mnt` filter matched Lima's own read-only cloud-init
disk (`/dev/vdb` on `/mnt/lima-cidata`); that attempt is retained and the corrected
review excludes exactly that mount. The persistent native root (407 MB) is
retained for audit inspection, as in previous checkpoints. All 13,996 pre-existing
audit/remediation evidence files still match the re-audit's own manifest, and the
re-audit's 15,735 files are unchanged since this work began.

## Identity and reproduction

Implementation SHA-256 (repository convention, `tests/evidence.py`):
`e755cbdd42ad02b96dff060bb66b0ccf59eb5c0491f6cbad91137238ee966250`.
The gate ran from a native-ext4 clone whose digest matched the working tree; the
committed tree is checked against the same digest. Reproduce from the repository:

```sh
./scripts/dev .venv/bin/python -m unittest discover -s tests -p 'test_*.py' -v
./scripts/dev env VIGIL_SQLITE_GENUINE_SOURCE=/path/to/sqlite-autoconf-3510300.tar.gz \
    .venv/bin/python -m unittest discover -s tests -p 'test_setup_sqlite.py' -v
./scripts/dev ./scripts/check-resilience --clean --output artifacts/my-gate
```

## What the independent audit should inspect

| Area | Files |
| --- | --- |
| Correction | `scripts/setup-sqlite` (only provisioning/production change) |
| Regression tests | `tests/test_setup_sqlite.py` (new), `tests/test_verification.py`, `tests/evidence.py` |
| Records | This report, `docs/OUTBOX.md`, `docs/RESILIENCE_RELEASE.md`, `docs/RESILIENCE_PERFORMANCE.md`, forward note in `docs/RESILIENCE_REMEDIATION.md`, and the project's state, decision and study records (not part of the published tree) |
| Expectations and controls | `artifacts/remediation-f5-20261027/EXPECTATIONS.md`; `before/` (original script copy, macOS RED and controls); `after/`; `linux-focused/` (Linux RED against the original script, before/after drivers, unit and genuine runs, re-audit probe); `mutation/`; `negative_controls.py`; the re-audit probe copy `check_setup_guard.py` and its outputs |
| Pin provenance | `genuine-source/` (official archive and 3.51.3 release log) |
| Full gate | `linux-full/` (`gate.json`, 72 logs, result files, `native-inventories.tar.gz` and manifest), `gate-summary.json`, `final-inspection/`, `native-identity.json`, `run_final_gate.sh`, `prepare_native.py` |
| Closeout | `linux-cleanup-review*.json`, `integrity-start.json`, `integrity-after-gate.json`, `integrity-final.json`, `static/`, `commit-receipt.json` |

The re-audit's own probe can be rerun unchanged from a copy outside its directory,
so the historical audit evidence is not rewritten. It finds the repository as its
directory's parent's parent, so the copy must sit directly under `artifacts/`:

```sh
mkdir artifacts/my-f5-probe
cp artifacts/independent-reaudit-20261026/check_setup_guard.py artifacts/my-f5-probe/
./scripts/dev python3 "$PWD/artifacts/my-f5-probe/check_setup_guard.py"   # exit 0 expected
```

## Limitations and residual risks

- The installer still verifies `sqlite3.c` only, the file whose hash SQLite
  publishes in its release log. The archive is extracted before verification, and
  the archive's `configure`/Makefile are executed without a pinned hash; HTTPS is
  their only authenticity check. An archive with a genuine `sqlite3.c` but
  altered build files would pass. This scope predates F5 and is unrelated to
  interpreter optimization. Closing it needs a separately sourced pin for the
  archive (or the build files) and is recorded as a follow-up, not silently
  expanded here.
- The installer trusts the invoking user's environment: `PATH` selects `curl`,
  `tar`, `python3` and `make`, and `PYTHONPATH` or the current directory could
  shadow `hashlib`. The refusal covers optimization only.
- Hosted CI was not run for this change at the time of this record; local CI
  commands are not a hosted result.
- Existing limits are unchanged: SIGKILL is not a power cut, vcan is not physical
  CAN, XFS/failover/power-loss are unqualified, and the measured remote delivery
  deficit remains.
