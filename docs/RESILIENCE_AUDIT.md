# Independent review of Vigil 2.0.0

The 2.0.0 implementation was reviewed by three independent audits. Each audit
judged an exact commit, re-ran or inspected the evidence itself, and treated the
implementation's own reports as claims rather than results. The first two
audits rejected their candidates. The third approved the remediated candidate
within the documented software-resilience scope.

This page summarizes those audits. The full reports, probe programs and native
fixture archives are kept in the local evidence archive (about 3.4 GB, not
committed). The final audit's complete 72-command gate output is attached to the
[2.0.0 release](https://github.com/tjsingh143/Vigil/releases/tag/vigil-v2.0.0)
as `vigil-v2.0.0-release-gate.tar.gz`.

## Audited identity

| Identity | Value |
| --- | --- |
| Audited commit | `e244c728299acc48bf75982cda3443bf510fad35` |
| Git tree | `762f1ef6432fa38640fc87c95f7d39539bcbd0a7` |
| Implementation digest | `e755cbdd42ad02b96dff060bb66b0ccf59eb5c0491f6cbad91137238ee966250` |
| Release `vigild` SHA-256 (native ARM64 build) | `5c423d2fd742ec59c6a557ce8f5a4be4b9cbc02b751f889eb528550a9de68be7` |

The implementation digest is SHA-256 over sorted `path NUL content NUL`
records for `CMakeLists.txt` and every tracked file under `src/`, `include/`,
`tests/`, `scripts/`, `simulator/`, `migrations/`, `systemd/`, `config/`,
`.github/`, `fuzz/`, `tools/` and `env/`
([`source_digest()`](../tests/evidence.py)). Documentation is excluded, so a
report can record the digest of the source it describes.

The `vigil-v2.0.0` release commit descends from the audited commit through a
documentation-only merge and documentation edits. Its implementation digest is
identical to the value above. Only files outside the digest differ: documentation
(`README.md`, `CHANGELOG.md`, `CONTRIBUTING.md`, `AGENTS.md`, `docs/`), `.gitignore`,
and development-process files removed from the published tree (`CODEX_STATE.md`,
`PROJECT_BRIEF.pdf`).

## Review sequence

| Date | Candidate | Verdict | Outcome |
| --- | --- | --- | --- |
| 2026-10-24 | `29b049e` (Stage 4 release candidate) | FAIL, remediation required | Findings F1–F4. The Linux gate could not be executed in that audit's environment, so runtime claims there were reviewed as retained evidence only. |
| 2026-10-25/26 | — | Remediation | F1–F4 corrected with reproducers and a new clean gate: [RESILIENCE_REMEDIATION.md](RESILIENCE_REMEDIATION.md) |
| 2026-10-26 | `be41eb1` | FAIL, remediation required | F1, F2 and F4 confirmed resolved. F3 incomplete, reported as F5. |
| 2026-10-27 | — | Remediation | F5 corrected and requalified: [RESILIENCE_F5_REMEDIATION.md](RESILIENCE_F5_REMEDIATION.md) |
| 2026-10-27 | `e244c72` | **PASS, approved** | Fresh 72/72 gate; no release-blocking defect; three nonblocking findings (A1–A3) |

| Finding | Severity | Problem | Correction |
| --- | --- | --- | --- |
| F1 | High | An uncertain local append overlapping a confirmed replay could make retirement retry forever | Retirement reports `APPEND_PENDING`; storage reconciles and publishes the held append first, then retries the retirement |
| F2 | Medium | Allocation failure after a committed retirement could later report success with stale usage counts | Usage is rebuilt in a temporary value and published only after a complete successful reload |
| F3 | Medium | Python `-O` removed `assert`-based verification guards and test oracles | Explicit `require()` checks that always run; every test entry point refuses optimized Python before fixtures start |
| F4 | Low | The service installer described an obsolete delivery stage | Installer and release guidance updated |
| F5 | Medium, release-blocking | The SQLite source check embedded in `scripts/setup-sqlite` still used `assert` and vanished under optimization | Early refusal of optimized Python before any download, explicit digest comparison with `sys.exit`, and checks against embedded Python programs |

## Final audit results

The final audit cloned the exact commit onto native ext4 inside the project's
Ubuntu 24.04 ARM64 VM (Linux 6.8.0-147, glibc 2.39, Python 3.12.3, SQLite
3.51.3, PostgreSQL 16) and ran `./scripts/check-resilience --clean` from a fresh
build. It started at 05:58:54 UTC on 2026-10-27 and took 1,996.5 s.

| Area | Observed |
| --- | --- |
| Gate | 72 of 72 commands exited 0 |
| C units | 67 cases in each of six `ctest` runs (402), plus 7 under Helgrind |
| Python | 19 tests per discovery run; installer tests 5/5 with a genuine SQLite archive |
| Recovery coordinator | 120 cases across Debug, Release, ASan and TSan: 64 SIGKILL cuts, 56 quiet recoveries |
| Adversarial schedules | 63 Debug seeds plus 4 each in Release, ASan and TSan (75) |
| Sustained backlog | 6,128 readings in 189 batches preserved and drained; startup 0.664 s, drain 5.275 s |
| Receive overload | 49,587 received + 414 kernel drops = 50,001; forced overflow 223 + 49,778 = 50,001 |
| Storage and service | Real ext4 `ENOSPC`, read-only startup, hardened temporary systemd units (READY, kill, watchdog) |
| Analysis | Strict GCC and Clang, GCC analyzer, ASan/UBSan/LSan, TSan, Helgrind: no findings |
| Fuzzing | 85,671,139 decoder executions in 121 s; CI smoke 7,678,602 in 11 s; no finding |
| Benchmark | Six paced and six burst trials passed; replay median 73.71 readings/s (73.59–77.20), local admission 99.97 |

Beyond the gate, the audit ran its own tests:

- **Installer probes.** 32 cases ran the unmodified installer under `-O`, `-OO`,
  `PYTHONOPTIMIZE`, wrappers, failing children, missing or unreadable sources,
  and failed downloads or extraction. Every observation matched its recorded
  expectation, including the probes written to demonstrate the remaining
  weaknesses below.
- **Mutation testing.** Nine copied source trees, each running the full test
  discovery suite. Removing the early guard, removing the mismatch failure,
  ignoring a failing child, an unquoted heredoc and an unclassified interpreter
  option were all rejected. Two interpreter spellings were missed (A2).
- **Runtime witnesses.** The F1 and F2 counterexamples were re-run against the
  corrected sources with the pinned SQLite.
- **Database reconciliation.** A separate reader that imports no project code
  opened 1,339 SQLite inventories and 727 PostgreSQL exports. It checked
  integrity, conservation equations, contiguous order, run origins and exact
  payload bytes on both sides. It found zero failures.
- **Performance re-derivation.** Rates, conservation and drain were recomputed
  from raw daemon logs for 18 paced trials.

## Nonblocking findings

**A1. Partial dependency integrity (medium).** `scripts/setup-sqlite` verifies
the SHA3-256 of the upstream `sqlite3.c` before building. It does not
authenticate the whole archive, its `configure`/`Makefile` scripts or member
layout, and extraction happens before the check. HTTPS from a fixed upstream URL
is the trust boundary. The approved claim covers `sqlite3.c`, not a pinned build
supply chain. Follow-up: verify an independently justified whole-archive digest
before extraction, validate members, and restrict redirects to HTTPS.

**A2. Embedded-Python discovery blind spots (low).** The test that inventories
Python programs embedded in shell scripts misses absolute (`/usr/bin/python3`)
and versioned (`python3.12`) interpreter spellings. No current script uses
either form for verification. The check covers its supported discovery grammar,
not every possible Python invocation. Follow-up: add both forms, or keep an
explicit reviewed inventory that rejects unknown forms.

**A3. Trusted caller environment (low).** A module planted on the caller's
Python path, or `PATH`-selected tools, can subvert the installer. Sourcing the
script inside a shell conditional disables `errexit`. Both require control of
the provisioning user's environment, and supported callers execute the script
rather than source it. Follow-up: an isolated interpreter and an explicit
execute-only interface.

## Not verified

The approval covers the documented software failure contract. It does not
cover:

- abrupt power loss or host reboot during an outage, media failure, or XFS;
- physical CAN hardware, timing, arbitration or bus-off behaviour;
- remote standby or failover, or simultaneous maximum-resource stress;
- a general allocator-exhaustion campaign;
- per-record asynchronous latency, queue lock-hold or SQLite heap
  instrumentation;
- sustained replay at the 100 readings/s offered load. Replay measured about
  74 readings/s on this VM, and no remote-rate threshold is part of the contract;
- hostile administration, tamper-resistant history or a compromised toolchain.

The earlier attribution of run-to-run benchmark spread to "environmental
variation" was stronger than the data supports. The same binary measured
replay medians from 73.7 to 83.9 readings/s on a shared VM. No runtime code
change explains the spread, and these non-isolated runs cannot establish its
cause.

Hosted GitHub Actions results for the release commit are linked from the
release notes. That workflow runs `scripts/check-ci`, not the full gate.
