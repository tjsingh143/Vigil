# Independent audit remediation — 2026-10-25–26

The independent audit rejected candidate `29b049e` at documentation HEAD
`8a7347c6b940df24d0e5536d5c1f5e60272a07ef`. This remediation began at that exact
HEAD with a clean tree. This record is the remediation's own verification, not
independent re-verification. Scope: focused fixes, a real Linux regression and
documentation; no deployment or production-data change.

> **Later note (2026-10-27):** the subsequent independent re-audit
> (`artifacts/independent-reaudit-20261026/`) confirmed F1, F2 and F4 but found the
> F3 objective incomplete: Python embedded in `scripts/setup-sqlite` still used
> `assert` (finding F5). See [RESILIENCE_F5_REMEDIATION.md](RESILIENCE_F5_REMEDIATION.md).
> The rest of this document is the unchanged 2026-10-26 record.

**Remediation checkpoint PASS — ready for fresh independent audit.**
The completed Linux qualification and evidence-integrity results are below.
Original audit artifacts remain under `artifacts/independent-audit-20261024/`.
New evidence is under `artifacts/remediation-20261025/`; complete gate attempts
use `artifacts/remediation-final-gate/` and `artifacts/remediation-final-gate-3/`. Original-artifact SHA-256 inventory is saved
before any reproducer execution.

## Findings, corrections and progress argument

### F1: confirmed A and uncertain appended B

The original coordinator gave retirement priority and blocked append following
any retirement error. Outbox recovery could find B's committed row, seal it,
and return BUSY so append could publish B first. The caller then blocked that
very operation. Repeating recovery did not change the dependency; main remained
alive and continued watchdog notifications.

`VIGIL_STORE_APPEND_PENDING` now represents this specific dependency. Retirement
returns it only after exact candidate recovery, before mutating A. The coordinator
retains A's successful result and B's original capture, reconciles B through the
existing append path, publishes B's counters, then retries A. It takes no new
capture in this branch. Ordinary retirement failures continue to block admission
and checkpoint mutation until retirement publication catches up.

The progress measure is the two unpublished operations, B then A. With storage
healthy, recovery compares B's identity and exact payload and performs the real
FULL recovery-generation transaction plus directory sync. Successful append
reconciliation clears `pending` and publishes B once. A's next retirement cannot
return APPEND_PENDING for B because that predicate requires `pending`. It either
retires the unchanged confirmed FIFO head or reconciles its already-retired
prefix, then releases the one replay slot. Further calls may wait for an actual
storage error, but cannot repeat the original dependency cycle. No new arrival,
condition signal, daemon restart or watchdog intervention is required.

If an uncertain candidate is absent, recovery checks unchanged allocators;
retirement may free the prior head before the pinned candidate retries. An
ordinary held-but-uncommitted FULL candidate does not receive the special result.
These distinctions retain the earlier fix preventing an append publication from
exposing retired spool totals before current-run acknowledgement counters.

No new queue, dynamic backlog allocation or nested lock is introduced. SQLite
and PG retain their separate owners. RESULT grants storage exclusive access to
the immutable mailbox; queue-lock publication completes before another SQLite
call. Main's monitoring/alarm paths and all wire/SQL semantics are unchanged.

### F2: committed retirement followed by failed usage reload

A sealed retirement may be complete even when its postcommit usage query returns
NOMEM. Previously, its next retry saw the retired prefix and returned success
with the pre-retirement cached usage. The prefix success path now reloads usage
before returning OK. Prepare, first step, completion step or finalization failure
remains a failure; it is never converted to successful retirement publication.
The usage loader builds a temporary snapshot and replaces the cache only after
the whole query and finalization succeed. NULL text caused by SQLite NOMEM is
kept retryable instead of being mislabeled corruption, following SQLite's
[column-result error contract](https://www.sqlite.org/c3ref/column_blob.html).

A failed COMMIT/directory sync still requires a sealing recovery transaction.
A successful seal followed only by NOMEM can retry the prefix query and usage
reload without another DELETE or counter increment. The successful result remains
pinned until correct usage is available. Old-run retirement only advances spool
counts; current-run counters are advanced only for that run, in the same locked
publication as spool usage. Remote receipts and history remain unchanged.

### F3: unconditional verification

All 539 Python `assert` statements in the existing verification modules were
converted to `require`, which raises explicitly. Predicate and failure-detail
ASTs were mechanically compared before/after conversion; failure details remain
lazy so a successful check cannot accidentally read a blocking diagnostic pipe.
The shared verifier module refuses optimized execution at import time, before
fixture imports, argument parsing or environment/setup work. Each independently
executable Python test imports it first. `-O` and inherited `PYTHONOPTIMIZE=1`
therefore fail clearly. Evidence records the optimization flag.

Negative controls enumerate all Python test files in both interpreter modes with
an audit-hook tripwire for writes, process launch, sockets and filesystem mutation.
They require the optimization refusal, not the tripwire, to fail first. Separate
normal-mode controls reject an inconsistent SQLite inventory, preserve and reject
an actual subprocess exit 17, reject an incorrect PG fixture identity before the
destructive body, and reject unexpected SQL success when VG001 was expected.
The original seven Python cases remain; six new verification tests bring the
unit count to 13. No integrity predicate or expected workload result was removed.

### F4: service guidance

The installer now describes SQLite pending history and schema-2 PostgreSQL replay
configuration. It still leaves provisioning and service start explicit. Shell
syntax passed. Its actual output was captured from a disposable script tree with
`sudo` intercepted to record commands; no installed binary, service or configuration
was modified. The capture accurately labels this as an installer-message check,
not deployment qualification.

## Reproducers and focused results

| Check | Before repair | After repair |
| --- | --- | --- |
| Original independent C overlap | Three BUSY returns; explicit append control then succeeds; exit 1 bug witness | Three APPEND_PENDING returns instruct the caller to publish B; control succeeds. The unchanged old witness exits 2 because it specifically expects BUSY; this is not counted as a passing regression. New coordinator tests prove progress. |
| Original C retirement NOMEM | Retry returns OK with records=1, retired=0; exit 1 | records=0, retired=1; exit 0 |
| Production-coordinator overlap | Audited algorithms with test hooks only: bounded progress timeout, confirmed remote, repeated local BUSY, local failures=9; exit 1 | 18 focused overlap cases pass: eight quiet current/old-run fault cases and ten reconciliation kill cuts |
| Production-coordinator NOMEM | Audited algorithms with test hooks only: published pending=1 while actual committed pending=0/retired=1; exit 1 | 12 focused cases pass: six current/old-run prepare/step cases and six prefix-publication kill cuts |
| Strict Debug C | Original counterexamples above establish failures | All four suites pass, 67 cases including two new outbox cases |
| Python negative controls | Independent audit recorded optimized acceptance of inconsistent inventory | All 13 Python cases pass; optimized entries refuse before side effects and negative controls fail as expected |

The original C build script writes into its own directory. Exact copies ran in
fresh native Linux clones, preserving the original audit artifacts. The daemon
negative controls use original production algorithms plus pipe hooks; their
instrumentation-only production diff is saved separately. New tests exercise
`src/persistence.c` with actual SQLite, libpq, disposable PostgreSQL and vcan.
No Python virtual bus or substitute macOS integration was used.

Allocation controls inject SQLite NOMEM return codes at usage-query prepare,
first-step and completion-step boundaries using test-only linker wrappers. They
are targeted API-failure tests, not a claim of exhaustive allocator exhaustion.
Production excludes the dispatcher, VFS, controls and wrappers. New hooks use
separate pipes for both workers and fixed thread-local bookkeeping.

Each successful reconciliation publication is compared with an independently
queried, consistent frozen DB/WAL copy and real PG receipts. Original identity,
exact B bytes, pending/retired counts, current/old-run accounting and receipt/history
projections are checked. All status pairs check conservation. SIGKILL cuts cover
append failure, sealed append recovery, append publication, retirement commit,
retirement publication and NOMEM prefix reconciliation. The quiet progress cases
keep the same PID and send additional traffic only after recovery is complete.

## Repair review and retained-evidence checks

The entire production diff was reviewed for owner/lifetime rules, lock ordering,
transaction boundaries and error classification. No lock nests or owners changed.
The RESULT mailbox remains pinned until correct retirement publication; retry
errors leave both payload and confirmation intact. SQLite calls remain outside
queue/mailbox locks. Shutdown joins owners before freeing the capture/mailbox.
Neither successful prefix reconciliation nor successful append reconciliation
repeats the original mutation. The extra reconciliation work is limited to the
fault path; normal monitoring, decoder, queue algorithms and migrations are
unchanged. Production hook macros compile away.

An independent AST normalization of every changed existing Python module reduced
the diff to the documented metadata, hook-FD support, production-symbol check and
release-runner additions. All 539 original predicates and failure-detail ASTs
remain present. Review records are `final-predicate-review.json` and
`python-nonmechanical-review.diff` in the remediation evidence directory.

Read-only reinspection of the earlier Stage 4 archive passed: its SHA-256 remains
`8703ef6773bf29c45e1739313694f24f81f5917dfcd51ca36800905b54f585eb`, all 68
retained commands recorded zero exits, 763 stopped SQLite inventories matched
their exports, and 231 PG union exports reconciled 787 distinct spool/order
identities. Original benchmark summary/distribution arithmetic also matched its
raw trials. The audited commit's implementation digest was reconstructed from
Git; the repaired working digest is intentionally different. Adapted copies of
the auditor's inspectors wrote only to the remediation directory. These are
archive consistency checks, not new Linux workload executions or independent
approval of the repair. Both inspectors exited 0; raw logs and results remain.

## Full release gate and residual limits

The first clean Linux gate passed **72/72 commands**, exit 0, on kernel
6.8.0-142 from 2026-10-25T19:52:22.150087+00:00 through
2026-10-25T20:23:06.729618+00:00. Implementation SHA-256 was unchanged:
`353462a3e1bd7db82284adfbba09e81ad903c5a5540e7f964f42fc6843c96e06`.
All four build variants passed the 67 C cases and 30 new daemon scenarios;
ASan/UBSan/TSan reported no findings. Original crash/replay/monitor regressions,
72 seed executions, ext4/systemd, Helgrind, analyzer, SQL, CI subset, demo and
matched benchmarks passed. Decoder fuzz executed 89,193,392 inputs in 121 s;
the CI smoke executed 7,937,102 inputs in 11 s. Raw command logs, reports, source
and binary hashes remain in `artifacts/remediation-final-gate/`.

Closeout resumed on 2026-10-26 after the VM had been stopped. Reboot removed the
first run's temporary native SQLite/PG inventory roots before archival; the
host-side logs and JSON reports survived. The missing raw roots cannot be
reconstructed from a success claim. A second attempt in persistent native
storage stopped at vcan setup: the newly booted installed 6.8.0-147 kernel lacked
its optional modules-extra package. That first-command exit 1 is retained in
`artifacts/remediation-final-gate-2/`. Installing only the current kernel's
modules-extra, as specified by `scripts/setup-linux`, restored real vcan.

Final archived qualification: **PASS, 72/72 commands, exit 0** on
`Linux-6.8.0-147-generic-aarch64-with-glibc2.39`, from 2026-10-26T07:42:29.068977+00:00 through 2026-10-26T08:12:39.408187+00:00.
The implementation SHA-256 is `353462a3e1bd7db82284adfbba09e81ad903c5a5540e7f964f42fc6843c96e06` in both completed gates.

| Required verification | Observed final result |
| --- | --- |
| C and Python | 67 C cases per candidate build (Debug, Release, ASan/UBSan, TSan and two CI builds); 13 Python tests per invocation, all passing; all 23 Python entries reject both optimization modes |
| Coordinator F1/F2 | 120/120 scenarios; 56 quiet same-PID progress cases and 64 reconciliation SIGKILL cuts, plus old-origin seeding; exact SQLite/PG state and conservation |
| Existing runtime | All local admission/recovery/uncertainty, PG equality/ambiguity/FIFO/poison/outage, monitoring, saturation, old-origin and crash regressions passed; 72 seed executions |
| Concurrency/sanitizers | ASan/UBSan and TSan clean, including real coordinator scenarios; scoped Helgrind queue and mailbox/handoff checks report zero unsuppressed errors; existing system-library suppressions retained |
| Native platform | Real vcan/can-utils, ext4 ENOSPC/read-only, unprivileged systemd READY, watchdog and SIGKILL/restart passed |
| Analysis/fuzz/CI | Strict GCC/Clang builds and GCC analyzer passed; libFuzzer 94,510,529 inputs/121 s, CI fuzz 8,416,467/11 s; local CI commands passed, no hosted-run claim |
| Production demo | 20/20 readings, 43 history rows, 29 receipts, exact duplicate suppression, zero final pending or known gap losses |
| Large outage backlog | 6128 exact readings, startup 0.208 s, drain 4.363 s; new admission during paused replay passed |

Automatic archive: `artifacts/remediation-final-gate-3/native-inventories.tar.gz`,
15,395,795 bytes, 17 native evidence roots, SHA-256
`ad0ca8e36c6793ba6aa91b437290cf90a2d0faa6051d095a65785ff081d55c4f`. An adapted copy of the auditor's inspected archive checker
reopened 1339 stopped SQLite inventories and checked
607 PostgreSQL exports. The separate union
checker covered 607 exports and 1164 distinct
spool/order identities. Both exited 0 with no inconsistencies; benchmark arithmetic
also matched every retained trial. This is author-performed evidence review,
not a fresh independent audit.

The original counterexamples were also repeated in fresh Linux fixtures after
this gate. Before: original overlap and retirement-NOMEM witnesses exit 1;
coordinator overlap hits its bounded progress failure and retirement publishes
stale usage (both exit 1). After: original retirement-NOMEM exits 0 with exact
usage; original overlap returns APPEND_PENDING and exits 2 by its old BUSY-only
expectation. The production coordinator's passing 120-case matrix supplies the
actual F1 proof. Repeated negative-control DB/WAL and PG union evidence is preserved
in `artifacts/remediation-20261025/counterexample-native-inventories.tar.gz`.
Original audit integrity: all 2,331 original files
remain byte-identical, with no additions or removals. Cleanup inspection found no
test daemon, mounted fixture or transient unit; default vigild is inactive,
installed PG 16/main remains online, ASLR is 2 and the native baseline worktree
was removed. The first cleanup inspection lacked `rg` in Linux; the recorded
Python fallback completed the process/mount checks without installing tools.

F1–F4 are resolved in the repaired candidate. No required regression gate remains
unexecuted and no new release-blocking defect is known. The candidate is ready
for a separate fresh independent audit; it is not independently approved at
this point. The local commit receipt is
`artifacts/remediation-20261025/commit-receipt.json`.

Existing limits remain: SIGKILL is not a power cut; real ext4 does not qualify XFS,
physical CAN or media flush behavior. General heap exhaustion, forced entropy/UUID
failure, simultaneous maximum resource fill, queue-lock timing, per-record
asynchronous commit/retire latency, remote failover and hosted candidate CI remain
separate qualifications. Targeted NOMEM tests close F1/F2's usage-query coverage gap.
Queued/held work and unsaved crash-time loss counters remain volatile; remote
history has no automatic retention. Poison heads intentionally block FIFO.

The earlier measured remote throughput deficit remains visible: the previous
candidate delivered a median 82.50 readings/s in the remote status window against
100/s offered, versus 100.00/s for original Vigil; its 600-reading drain was
11.13 s versus 0.84 s. These are finite workload results, not sustainable-capacity
promises. The first repaired gate measured local admission 99.997/s, remote window
74.37/s (original 99.995/s), 600-reading drain 10.12 s (original 0.86 s), CPU
11.52% of one core (original 2.25%) and peak RSS 11.67 MiB (original 7.77 MiB).
Its remote window rate is about 9.9% below the earlier candidate's measurement;
this decrease is retained, not hidden. Paced monitoring remained 100/s, and the
same-run core p50/p95/p99 medians matched at 83/84/84 ns. These separate runs on
a shared VM do not isolate a causal effect of the repair. The archived rerun's
measurements are reported below alongside the first trial set.

### Final matched performance

The final archived rerun passed all six paced trials and six finite bursts on
Linux 6.8.0-147; raw data is in `artifacts/remediation-final-gate-3/benchmark/`.
Each paced trial reconciled all 6,800 readings without known reading/event loss;
all repaired stores ended with zero pending records, batches and bytes. The
source digest is `353462a3e1bd7db82284adfbba09e81ad903c5a5540e7f964f42fc6843c96e06`.

| Final archived rerun: median (range), three trials | Original Vigil | Repaired Resilience |
| --- | --- | --- |
| Paced monitoring readings/s | 100.00 (100.00–100.00) | 100.00 (100.00–100.00) |
| Remote window readings/s | 100.01 (100.00–100.01) | 83.86 (82.18–84.30) |
| Remote completion incl. catch-up readings/s | 100.00 (99.96–100.00) | 81.97 (81.11–83.13) |
| 600-reading drain, s | 0.94 (0.83–0.94) | 8.87 (8.85–9.17) |
| Daemon CPU, % of one core | 1.97 (1.93–2.13) | 12.86 (12.25–13.26) |
| Peak sampled RSS, MiB | 7.76 (7.75–7.78) | 11.16 (11.12–11.27) |
| Alarm lateness beyond 500 ms, ms | 7.00 (6.00–10.00) | 8.00 (7.00–8.00) |
| Core frame p50, ns | 85.00 (85.00–167.00) | 167.00 (84.00–169.00) |
| Core frame p95, ns | 86.00 (85.00–209.00) | 209.00 (84.00–211.00) |
| Core frame p99, ns | 86.00 (86.00–210.00) | 209.00 (84.00–212.00) |
| Local admission readings/s | N/A | 99.98 (99.95–99.98) |

Both completed repaired runs are retained. The kernel changed after VM reboot;
shared-host scheduling/storage also vary. The results do not isolate a causal
performance effect of this small recovery repair. Normal monitoring met the
same offered load and alarm checks, while remote replay still failed to sustain
100 readings/s for the natural batch shape. Neither the faster archived rerun
nor its API measurements erase the slower first result. No sustainable delivery,
physical CAN or asynchronous per-record latency guarantee is inferred.

The final direct-core probe's median p50 is slower (167 ns versus 85 ns), and
its bulk median is also lower; this is retained in the raw results, not treated
as an improvement. Trial 2 reverses the ordering (84 ns Resilience, 167 ns
original), and clock-pair p50 spans 0–42 ns with p95 up to 85 ns. The monitor,
config and protocol sources are byte-identical to original d179681. These
non-isolated, quantized measurements cannot attribute a core-code regression;
no tight core-latency guarantee is claimed. The production workload still meets
the offered 100/s monitoring load, exact counts and alarm timing checks.

## Changed-file map for the fresh reviewer

| Area | Files and purpose |
| --- | --- |
| Production coordination | `src/persistence.c`, `src/outbox.c`, `include/vigil/outbox.h`: dependency result, ordered recovery publication, complete usage reload and atomic cache replacement |
| Production test seam | `src/replay.c`: compile-away hook after publishing a remote result; no production replay behavior change |
| C verification | `CMakeLists.txt`, `tests/failpoint.c`, `tests/sqlite_usage_faults.c`, `tests/test_outbox.c`: test-only indexed worker pipes, one-shot NOMEM injection, exact retired-prefix/immutable-append cases |
| Python verification | `tests/verification.py`, `tests/test_verification.py`, `tests/resilience_remediation.py`: unconditional oracle, optimization/negative controls and real coordinator interleavings |
| Existing Python checks | All 20 pre-existing `tests/*.py` modules: early optimization guard, unchanged predicates converted to `require`; `evidence.py` records mode, `resilience_integration.py` supports multiple hook FDs, `verify_resilience.py` includes the new scenarios and testable subprocess-result check |
| Scripts | `scripts/install-service`: current SQLite/schema-2 replay guidance; `scripts/check`, `scripts/check-ci`, `scripts/check-release`: accurate C-test expectation count |
| Documentation | This report, `OUTBOX.md`, resilience architecture/contract/fault/release/performance documents, and the project's state, decision and study records (not part of the published tree) |

Original audit artifacts are unchanged. New raw logs, failing controls, scripts,
JSON reports and native archives live under the separate artifact directories.
Large runtime evidence remains local and ignored by Git; the tracked report
records its paths and hashes for the next auditor. The next step was a fresh
independent review of the repaired commit; see [RESILIENCE_AUDIT.md](RESILIENCE_AUDIT.md).
