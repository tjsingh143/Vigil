# Verification

Vigil has three Linux gates of increasing scope. A passing subset does not show
that a wider gate, physical CAN hardware or power-loss behaviour was exercised.

| Gate | Runs on | Adds |
| --- | --- | --- |
| [`scripts/check-ci`](../scripts/check-ci) | GitHub Actions or any Linux host; no sudo, `vcan` or systemd | Strict GCC and Clang Debug builds, 67 C cases per build, Python unit tests and negative controls, decoder CLI controls, five PostgreSQL v1 and ten v2 contracts in disposable clusters, outbox units on native storage, ASan/UBSan, GCC `-fanalyzer`, a 10-second fuzz run |
| [`scripts/check-release`](../scripts/check-release) | Linux with `vcan` | Optimized Release build, real `vcan` decoder and monitor integration, local durability restarts, replay regressions |
| [`scripts/check-resilience`](../scripts/check-resilience) | Linux with sudo, `vcan`, systemd, native ext4 | The complete 72-command release gate (below) |

## Hosted CI

The [workflow](../.github/workflows/ci.yml) builds SQLite 3.51.3 with
`scripts/setup-sqlite`, then runs `scripts/check-ci --clean` on `ubuntu-24.04`
(x86-64) for every push and pull request. Logs and fuzz artifacts are uploaded
whether the run passes or fails. Results for each commit are on the
[Actions page](https://github.com/tjsingh143/Vigil/actions/workflows/ci.yml),
and the 2.0.0 release notes link the run for the release commit.

## Release gate

```sh
./scripts/check-resilience --clean --output artifacts/<new-dir>   # scripts/verify is an alias
```

The gate takes about 35 minutes on the 2-vCPU VM. It runs, in order:

1. Python unit tests and the PostgreSQL v1 and v2 contracts.
2. For Debug, Release, ASan/UBSan and TSan builds: `ctest`, monitor
   integration over real `vcan` (plus decoder integration outside TSan), outbox
   crash/restart integration, PostgreSQL replay with SIGKILL cuts, the
   recovery-coordinator remediation cases, and adversarial fault schedules
   (all seeds in Debug, four in the others).
3. Real storage and service faults: loop-mounted ext4 `ENOSPC`, read-only
   startup and temporary hardened systemd units. Then the original fault
   scenarios.
4. Helgrind on the queue and outbox handoff, the GCC analyzer build, manual
   `can-utils` commands, and a 120-second libFuzzer run.
5. `scripts/check-ci`, the production demo, and the matched 1.0 versus 2.0
   benchmark.
6. Shell syntax checks and `git diff --check`.

Each command's expectation is printed before it runs. `gate.json` records each
command's expectation, argv, exit code, start time, duration and log. It also
records the commit, the kernel and the implementation digest `source_sha256`. Because the digest covers source, tests, scripts and the
workflow but not documentation, evidence can be matched to the code it tested
even when documentation changed afterwards. Native SQLite and PostgreSQL
inventories (DB/WAL/SHM files and exports) are written next to the logs.

To produce release evidence:

- run from a fresh clone on the VM's native filesystem, with `TMPDIR` inside it;
- keep `vcan0` otherwise idle and the host awake (`caffeinate -i` on macOS);
- use a new output directory for every run.

[RESILIENCE_RELEASE.md](RESILIENCE_RELEASE.md#reproduce-from-a-fresh-linux-checkout)
has the exact clone commands.

## Results for 2.0.0

The independent release audit ran the gate on the audited commit on 2026-10-27:
72 of 72 commands passed. Its counts, reconciliation and remaining findings are
in [RESILIENCE_AUDIT.md](RESILIENCE_AUDIT.md). The complete output is attached
to the [2.0.0 release](https://github.com/tjsingh143/Vigil/releases/tag/vigil-v2.0.0)
as `vigil-v2.0.0-release-gate.tar.gz`. Command logs cite local `artifacts/` paths
from the machine that ran them.

Fault coverage is described in [RESILIENCE_FAULT_TESTS.md](RESILIENCE_FAULT_TESTS.md),
and measurements in [RESILIENCE_PERFORMANCE.md](RESILIENCE_PERFORMANCE.md).

## Fuzzing

The [fuzz target](../fuzz/README.md) is the decoder, not the full daemon.
`./scripts/fuzz [seconds] [new-output-directory]` saves its exact invocation, log,
corpus and any failure input, by default under `artifacts/fuzz-*`. A clean finite run is not a proof of
correctness.

## Outside every gate

Physical CAN timing, wiring, arbitration and bus-off behaviour; hardware boards;
abrupt power loss and host reboots; XFS; remote PostgreSQL failover. See the
[limitations](../README.md#limitations).

## 1.0 records

- The 1.0 source at `c66b28d705e44e957279dc3c168979d11b16f7a3` passed
  [the hosted workflow](https://github.com/tjsingh143/Vigil/actions/runs/36601359319):
  45 C cases per tested build, seven Python cases, five PostgreSQL contracts,
  ASan/UBSan, GCC analysis and a short libFuzzer budget.
- October 11, 2026 documentation audit: the seven Python unit checks passed.
  The protocol and decoder compiled with GCC strict warnings, and the decoder CLI
  vectors and invalid-argument controls passed. Hosted CI was checked on GitHub.
  PostgreSQL, `vcan`, systemd, sanitizers and the full gates were not re-run for
  that documentation update.
- `tests/verify_checkpoint.py` is the 1.0 checkpoint runner. It regenerates a
  1.0-format report at this path and is not part of the 2.0 gates.
