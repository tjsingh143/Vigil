# Vigil Resilience state — Stage 2 COMPLETE

**Stage 2 PASS — PostgreSQL replay and reconciliation accepted.** One Codex
agent only; no delegation or other Codex instances. Started on clean main at
`dd2773c`. The owner authorized this implementation, isolated Linux/PostgreSQL
tests, documentation and a safe local commit. No deployment, production migration,
production-data change, push or publication. **Stop here; do not start Stage 3.**

## Completed functionality

The SQLite owner selects the oldest durable spool order, copies its immutable
payload after ending the read statement, and dispatches one bounded mailbox.
The separate PG owner retains asynchronous libpq deadline/flush/result handling.
Main keeps CAN/timer/signals, live alarms and the systemd watchdog. No SQLite,
fsync or PG I/O runs under queue/mailbox locks. Remote waits/backoff cannot block
local admission; a local retirement error specifically pauses further mutation
until its acknowledgement is reconciled and published consistently.

Migration [002](../migrations/002_resilience.sql) adds original run origins and
exact byte receipts. Its invoker replay function validates original JSON,
serializes v2 clients per run, reuses unchanged `write_batch(jsonb)`, compares
all history columns and commits the checkpoint/receipt atomically. Equal bytes
succeed; conflicting identity/content is VG001; malformed input is VG002. There
is no fallback to v1 ingestion. Migration 001, sensor/protocol/config source,
wire vectors and the service unit remain unchanged.

Only validated inserted/equal success through ReadyForQuery/idle can retire the
matching SQLite head. Deletion, prefix/usage/lifetime totals and that origin's
loss/revision cursors commit together with FULL/directory sync. Every failed
retirement COMMIT/sync requires a recovery sealing barrier before prefix-based
acknowledgement. New processes preserve old run/token/boot/times, IDs, payloads,
alarms and gaps; they never restore old live sensor state. Old-run retirement
cannot advance current-run health/written cursors.

One held admission and one shared/PG replay copy bound work memory. Existing
record/batch/byte and DB/WAL/SHM limits remain; no durable eviction. Transient
retries use 1/2/4/8/16/30-second delays capped at 30, unaffected by enqueues.
Configuration retries use 30 seconds; poison heads latch until repair/restart.
Capability probes require schema 2 and PG durability settings. Shutdown retains
the shared one-second best effort and systemd's three-second force-stop bound.
Operation, grants and recovery details: [OUTBOX.md](OUTBOX.md). Justified interface,
startup-classification and scheduling refinements are in the architecture and
contract amendments.

## Baseline and complete clean gate

The full brief v3, instructions, state/contracts/architecture, Stage 1 changes
and actual source were reviewed. The unchanged Stage 1 Debug core durability
suite passed before remote implementation; the remaining detached baseline gate
finished while implementation continued. All 55 baseline commands passed,
2026-10-23 18:45:08.249101–18:56:38.161383 UTC. Baseline source digest:
`706261f8fdf7f2302b185bf0ac26b0891ec71f14222c03276d63298dcf3019a2`.
Raw baseline: `artifacts/resilience-stage2-baseline/` and its adjacent log.

The final complete clean gate ran from the isolated native Linux source copy:

```sh
./scripts/dev sh -c 'cd /tmp/vigil-stage2-final-source; ./scripts/check-resilience --clean --output /Users/tj/Desktop/Vigil/artifacts/resilience-stage2/final7'
```

**All 61 commands exited zero**, `2026-10-24T02:25:29.975538+00:00` through
`2026-10-24T02:38:55.042373+00:00`. The source-unchanged assertion passed.
Tested parent: `dd2773c`; workspace/native implementation SHA-256:

`d67373a3641414533e7189648a2dd2c8946929a61c16598ebe5edb5a103e8399`.

Exact argv/expectations/exits/times/logs: `artifacts/resilience-stage2/final7/gate.json`;
console: `artifacts/resilience-stage2/final7.log`. Docs/state are excluded from
the digest to avoid circular reporting. The reviewed local commit is identified
by `git log -1 --oneline`; its hash is supplied in the delivery response.
Ubuntu 24.04 ARM64/ext4, Linux 6.8.0-142-generic, SQLite 3.51.3,
PostgreSQL/libpq 16.15 and systemd 255 were actually used.

## Exact failure boundaries proved

| Boundary | Controlled cut and observed assertion |
| --- | --- |
| A | Pipe hook after local COMMIT/directory sync, before publication; offline endpoint; SIGKILL. No remote record submission; exact local rows/origin/bytes survive. |
| B | Disposable PG trigger pauses after history insertion before checkpoint. Observer sees advisory wait, terminates that backend. Run/origin/history/checkpoint/receipt all absent, local head retained; healthy retry commits the whole batch. |
| C | Real-PG proxy suppresses replay response through ReadyForQuery; independent SQL observer verifies exact committed receipt. SIGKILL before client success: 20 Debug plus two per other build, **26 kills**. |
| D | Three kills per build at `after_remote_confirm_before_result`, and three at `before_retire_begin`. Local head remains and retries to equal receipt. |
| E | Three kills per build at `after_retire_delete_before_commit`. Recovered SQLite rolls back DELETE/counters; unchanged head retries safely. |
| F | Three kills per build at `after_retire_commit_before_publish`. Committed prefix/totals advance once; deleted head stays absent after restart. D/E/F comprise **48 exact cut kills**. |
| G | Proxy captures identical original byte parameters across multiple retries: **23 Debug, five each Release/ASan/TSan (38 total)**. One remote receipt/history effect per identity. |
| H | Every seeded restart retains original run/token/boot/start/observation/seq/batch/order metadata. New startup UUID differs; old replay never initializes current readings. |
| I | PG head paused before send while **64 new readings commit locally**. FIFO drains original eight plus new 64; no history/snapshot duplication, and current-run health catches up separately. |
| J | VG001 and PG CHECK 23514 latch the daemon head after one submission, retain exact local bytes and permit bounded admission/volatile overflow. Explicit fixture repair/restart drains unchanged. Direct invoker tests reject 18 malformed payload forms with VG002 and atomic rollback. |

Local Stage 1 cuts also repeat in all four builds: 20 repetitions at four
admission cuts plus origin commit in Debug, two each elsewhere: **130 local
transaction/origin SIGKILLs**, with direct stopped DB/WAL inventories.
Test controls/dispatch/VFS are absent from production binaries.

## Observed database and accounting assertions

Every fixture compares receipt bytes, full original identities, typed logical
history columns/timestamps, origin metadata, complete alarm snapshots and exact
gap totals. Remote unique key sets equal expected durable record sets; all
history/checkpoint/receipt counts match. Final healthy fixtures have exactly
zero pending records, batches and payload bytes; admitted = retired = remote
logical history. SQLite order prefixes, origin pruning and usage reconcile.

| Build | C kills / identical attempts | C unique history / receipts | I readings / history | Outage conservation | Consistent uncertain-ack status pairs |
| --- | --- | --- | --- | --- | --- |
| debug | 20 / 23 | 38 / 27 | 72 / 79 | 40 + 60 = 100 | 22 |
| release | 2 / 5 | 17 / 9 | 72 / 79 | 40 + 60 = 100 | 22 |
| asan | 2 / 5 | 17 / 8 | 72 / 79 | 40 + 60 = 100 | 23 |
| tsan | 2 / 5 | 17 / 9 | 72 / 79 | 39 + 61 = 100 | 23 |

The outage fixture uses a 40-record test cap (above the maximum 32-record
candidate) and a four-reading RAM lane. Known reading losses equal PG gap sums;
event losses were zero. Recovery clears degraded after all current cursors and
backlog catch up. Metadata grouping/receipt counts can vary with scheduling;
each admitted batch remains byte-identical over retry/restart.

All four maximum-snapshot fixtures produced exactly **162 history rows**:
64 readings, 32 heartbeats, 32 high raises, 32 high clears and two degraded
transitions. Snapshots each have 193 unique alarm entries. No history dropped.
The forced lost-local-retirement-confirmation regression delivered twelve
readings and checked every captured spool/current-run conservation equation;
no inconsistent pair remained. Restart completed all pending work.

Debug transient retry intervals were **1.037, 2.099, 4.048, 8.053, 16.081, 30.059, 30.140 seconds**;
permission retry was **30.070 s**.
Missing migration 002, then fsync/full_page_writes off, produced zero remote
submissions/effects while five readings committed locally. Healthy restoration
replayed all five and left pending=0. The two new invoker functions have no
PUBLIC EXECUTE; runtime has no UPDATE/DELETE/DDL privilege.

## Regression results

- Debug/Release GCC, Clang ASan+UBSan and GCC TSan each passed 65 C cases
  (13 protocol, 26 monitor, six queue, 20 outbox). The CI script repeated two
  unit builds: **390 C executions**, plus separate Helgrind cases. No sanitizer
  finding observed; system libraries were not rebuilt with instrumentation.
- Seven Python cases passed in the main gate and again in CI. Five v1 PG
  contracts and ten v2 contracts passed twice; the latter include concurrent
  equal/conflicting batch/event clients, origin collisions, legacy checkpoint
  rejection, exact receipt bytes and whole-statement rollback.
- Real vcan decoder runs passed in Debug/Release/ASan; live monitoring, local
  fault/capture/capacity/restart and new remote suites passed in all four builds.
  No mandatory command was skipped; TSan uses setarch only for its subprocess
  tree, leaving global ASLR unchanged.
- Lock/statement/blackhole shutdowns in all builds were below 1.25 s; largest
  observed was **1.083038 s**. Actual ext4 ENOSPC returned SQLITE_FULL=13,
  published no failed-call durability, preserved high/stale monitoring and
  stopped in **1.020697 s**. Final recovered three
  records matched the final confirmed local count; a later retry reused allocated
  pages. Read-only startup exited 2/no READY with five prior records intact.
- The temporary hardened UID 501 systemd unit passed READY, SIGKILL restart,
  main-loop watchdog restart and clean stop; nine committed records survived
  with exact payloads. No temporary unit/mount/daemon remains. Default vigild
  stayed inactive; 16/main PG stayed online. No production install/data change.
- Helgrind queue and two-owner handoff checks returned zero unsuppressed errors.
  `tests/helgrind.supp` scopes only p11-kit ELF-finalizer mutex diagnostics and
  glibc internal replacement wakes, never Vigil races/direct lock/signal calls.
  Strict GCC analyzer, shell syntax, independent can-utils exchange and diff
  whitespace checks passed. Local Markdown targets were checked separately.
- Silence alarms were **500 ms** for each of three sources at a 500 ms deadline,
  20 ms tick. Natural load: **46839 received + 3162 kernel drops = 50001 sends**,
  46804 accepted readings + one heartbeat + 34 rejects; zero missed ticks.
  Forced overflow: **223 + 49778 = 50001**, one missed tick. Inferred sequence
  loss is separate and is not added to socket drops. Persistence was disabled;
  these finite vcan loads establish no physical/sustained throughput claim.
- Decoder fuzz: **89,032,894 inputs in 121 reported seconds**, RSS 313 MiB,
  slowest-unit 0 s, no crash/sanitizer/invariant failure. CI also passed its
  shorter fuzz smoke. Hosted GitHub CI was not run or claimed for this commit.

## Defects fixed, review and retained attempts

The maximum-snapshot gate found healthy buffering re-raising degraded while
persisting its clear transition; backlog now holds an existing degradation,
while failures/new losses raise it. Initial snapshot gating also prevents a PG
probe racing main's first health publication. Query failures immediately after
connect are classified as query failures. PG idle/backoff scheduling is separate
from producer condition signals; submission rechecks the shutdown deadline.

A controlled lost-retirement-confirmation test reproduced inconsistent spool
vs current-run counters when a new append recovered the committed prefix before
pending acknowledgement publication. Storage now reconciles/publishes that
result before another mutation; the integrated regression covers arrivals,
unknown local completion and restart. Failed retirement COMMIT/sync always seals
recovery before acknowledgement. Recovery errors roll back active transactions;
allocation guards and lifetime signed-count exhaustion are explicit. Validation
resource errors retain retryable SQLSTATEs rather than being relabeled poison.

Fixture corrections: tolerate disappearing ephemeral PG FDs in non-atomic /proc
enumeration while retaining timerfd/signalfd checks; require current expected
reading counts rather than an older caught-up status; use a test cap that can
fit a whole candidate; compare ENOSPC final committed inventory with final status
because allocated DB/WAL pages may be reused. A first retry-timing fixture filled
4096 batches and exceeded its arbitrary 40-second drain wait; the timing test now
uses four arrivals/second to isolate backoff. No production delivery deadline was
weakened or benchmark claim added.

Earlier dev and final1–6 attempts are preserved under `artifacts/resilience-stage2/`;
some were stopped for confirmed fixes, others failed the stated fixture oracle.
They are not counted as complete gates. Test-only orphan CAN processes after
one interruption were explicitly stopped before the final run; final inventories
and results above come only from the complete, otherwise-idle-bus final7 gate.
Final diff/source review covered ownership/lock ordering, commit ambiguity,
error/rollback paths, origin scoping, resource bounds, shutdown and lifetime cleanup.

## Evidence preservation and limits

Native DB/WAL/SHM inventories remain at the nine paths listed in
`final7/native-evidence-paths.json`. A portable offline archival copy is
`artifacts/resilience-stage2/final7/native-inventories.tar.gz`,
**6511398 bytes**, SHA-256
`110d1a6dd688562d5e8bcafe90c8afa5eb5adb5783ee496548cc907d4ec564fd`. SQL exports, exact hook tokens, binary digests and per-case
counts are in each build's result.json and daemon transcripts.

SIGKILL/VFS tests prove process recovery, not power-cut/media-loss survival.
Honest local/remote storage, flushes and PG durability configuration remain
assumptions. Kernel/credential-file stalls can exceed application deadlines;
systemd provides the force-stop bound. Only Ubuntu ARM64/ext4 ran; XFS and
hardware portability are unqualified. Conservative physical maxima were not
simultaneously stress-filled. Queued/held inputs remain volatile, and unsaved
crash losses cannot be reconstructed. A poison head intentionally blocks FIFO;
remote receipts/history have no automatic retention. No unconditional delivery
latency, exactly-once network transmission or absence-of-all-races claim.

No confirmed Stage 2 implementation defect remains unresolved in the tested
scope. Scoped system-library Helgrind diagnostics remain a tooling limitation.
The 60-second persistence benchmark/further instrumentation, hardware power
qualification, new products and Stage 3 remain deferred. **Stage 2 PASS; stop
following the local checkpoint commit. Do not deploy, push or publish.**

---

# Historical Stage 1 and architecture checkpoints

# Vigil Resilience state

## Authorized scope and final status

**Stage 1 COMPLETE — local durable-outbox acceptance passed.** One Codex agent;
no delegation or other Codex instances. Work started on clean main at
`2bc113bb604996c985176426b9e0c64de4456b78`. The owner explicitly authorized
SQLite, daemon/CLI/status/shutdown/service configuration integration, genuine
process-kill tests and a safe local commit, superseding Stage 0B's proposed
module-only slice. No deployment, production-data change, push or publication.

[OUTBOX.md](OUTBOX.md) defines current operation. The full
[failure contract](RESILIENCE_CONTRACT.md) and
[architecture](RESILIENCE_ARCHITECTURE.md) retain deferred remote requirements
and record the scope amendment. Earlier authorization stop wording below is
historical. **Stop after this local checkpoint; do not start Stage 2.**

## Implemented behavior

The dedicated SQLite owner commits immutable format 2 batches, transactional
run/event/batch/order identities, captured losses/revisions and logical usage.
Verified WAL/FULL and parent-directory fsync precede local success. Unknown
outcomes close/reopen/validate, compare the pinned identity/payload and force a
recovery_generation-changing FULL barrier. No SQL or sync runs under a queue
lock or on the CAN monitoring thread. Main retains all live alarms, CAN/timer/
signal processing and watchdog ownership; recovered history never initializes
live measurements. Initial complete snapshot publication gates ingestion.

Native ext4/XFS, private ownership/modes, exclusive ownership, explicit bootstrap,
exact schema/version/JSON/cross-row validation, signed integer limits and bounded
RAM/record/batch/payload/physical storage policies apply. Durable older history
is never evicted. One held candidate remains volatile during FULL/BUSY/READONLY/
I/O faults; retries are paced. Corruption/impossible state latches a fault.
A failed checkpoint freezes every subsequent mutation, including shutdown and
recovery paths, until successful truncate checkpoint plus directory sync.

No PG sender, migration 002, remote receipt/content verification, retirement or
backlog deletion is implemented. Remote-written counters remain zero; all local
batches stay pending and persistence-degraded remains active. No v1 send fallback.
Migration 001 and its standalone v1 database contracts are unchanged.

## Baseline and exact final gate

Before edits, archived the original HEAD (2bc113b) and ran this command:

```sh
./scripts/dev sh -c 'mkdir -p /tmp/vigil-stage1-baseline; git archive HEAD | tar -x -C /tmp/vigil-stage1-baseline; ln -s "$PWD/.venv" /tmp/vigil-stage1-baseline/.venv; cd /tmp/vigil-stage1-baseline; ./scripts/check-release --clean'
```

All 45 original C cases and real decoder/monitor/PG integration passed,
including original backfill/overflow, lock/statement timeout, lost response,
event reserve, TCP blackhole and volatile-crash cases. Baseline console:
`artifacts/resilience-stage1/baseline.log`. Independently recomputed original
implementation SHA-256
`b87b8f78da76c8bcfbf7c07d7f2ec26f795ca27d56bb18eedebf864479cf419b`;
it matches the saved complete Stage 0 gate with ten zero command exits.
These original PG results are baseline evidence, not Stage 2 acceptance.

Final complete clean gate, run from an isolated native Linux source copy to
avoid shared-filesystem build timestamp skew:

```sh
./scripts/dev sh -c 'cd /tmp/vigil-stage1-final-source; ./scripts/check-resilience --clean --output /Users/tj/Desktop/Vigil/artifacts/resilience-stage1/final'
```

All **55 commands exited zero**, 2026-10-23T09:24:59.542409 through
09:29:53.719239 UTC. The final unchanged-source assertion passed. Tested parent:
`2bc113bb604996c985176426b9e0c64de4456b78`. Native copy and workspace share
implementation SHA-256:

`706261f8fdf7f2302b185bf0ac26b0891ec71f14222c03276d63298dcf3019a2`.

Docs/state are excluded from this digest. The reviewed local checkpoint commit
is identified by `git log -1 --oneline`. Exact argument arrays, expectations,
exit codes, timestamps and logs: `artifacts/resilience-stage1/final/gate.json`;
console: `artifacts/resilience-stage1/final.log`.

## Observed results

| Gate | Actual result |
| --- | --- |
| Strict builds/C units | Debug/Release GCC, Clang ASan+UBSan, GCC TSan; each passed 13 protocol + 26 monitor + six queue + 15 outbox cases: **240 C executions** |
| Original Python/PG | Seven Python cases; five disposable v1 SQL schema/transaction/permission contracts passed |
| Real Linux integration | Decoder/vcan in Debug/Release/ASan; unchanged monitoring in all four builds; all passed |
| Local fault/recovery | All 13 integration cases in each build; direct recovered schema/origin/order/identity/payload/counter assertions passed |
| Process kill cuts | Debug: 20 repetitions at four admission cuts plus 20 origin-commit kills; other builds: two each; **130 transaction/origin SIGKILLs** total, plus separate systemd crash/watchdog tests |
| Postcommit survival | Debug retained **160 test readings + 20 startup events** across 20 kills; original run/token/boot/seq/order and exact batch bytes survived. Precommit readings were absent. Two exact hook tokens ensure all N inputs are queued before the selected candidate |
| Capture race | Exactly 16 later reading evictions captured once in the next immutable batch; five retained readings committed |
| Logical/physical bounds | Exact test record/batch/byte limits preserved durable older rows; separate RAM event reserve/overflow accounted. WAL-growth case crossed 4 MiB while every WAL sample remained <5 MiB, then reopened all 48 batches |
| Storage failures | Actual ext4 ENOSPC returned SQLITE_FULL=13; live high/stale alarms continued; shutdown **1.027906 s**. Actual read-only ext4 rejected startup (exit 2/no READY), retaining five committed records. VFS BUSY/READONLY/xRead/xWrite/xSync and directory-sync errors published no failed-call durability |
| Checkpoint failure | VFS checkpoint-lock BUSY while writer lock remained usable: further append below 4 MiB was refused, no allocator/usage advance; successful truncate retry admitted the same candidate |
| Supervision | Temporary hardened unit UID501, zero effective capabilities, READY and SIGKILL/watchdog restart; all nine committed records retained identical bytes |
| Silence | All three sources alarmed at **506 ms**, configured deadline 500 ms, tick 20 ms |
| Natural finite load | **50001 sent = 48047 received + 1954 kernel drops**; zero rejects, zero missed ticks. Inferred sequence loss 162 is separate and is not added to kernel drops |
| Forced receive overflow | **50001 sent = 223 received + 49778 kernel drops**; zero rejects, zero missed ticks |
| Remaining regressions | All seven applicable original fault/load cases and independent candump/cansend exchange passed |
| Thread/static checks | Helgrind: zero errors for full queue suite and concurrent outbox handoff/shutdown case. GCC -fanalyzer passed. No sanitizer finding observed; system libraries were not rebuilt with sanitizers |
| Decoder fuzzing | `./scripts/fuzz 120 .../final/fuzz` exited zero: **93210029 inputs**, reported 121 s, peak RSS 302 MiB, slowest-unit report 0 s; no crash/sanitizer/invariant failure |
| Production isolation | Test dispatcher/VFS/environment controls absent from production binary; installer copies only production daemon |

Direct SQLite evidence contains **199 stopped DB/WAL/SHM inventories**. Largest
sampled individual DB/WAL/SHM lengths: 212992/1396712/32768 bytes. Largest
combined sample: 1474536 bytes. These inventory maxima are separate from the
larger 4–5 MiB C WAL-growth fixture. No online external SQL client opened a live
owner store. Native inventories remain in Lima:

- `/tmp/vigil-stage1-evidence-6lb181c6` (Debug)
- `/tmp/vigil-stage1-evidence-5afhbmkp` (Release)
- `/tmp/vigil-stage1-evidence-wwfxptwy` (ASan/UBSan)
- `/tmp/vigil-stage1-evidence-n0gsbh4i` (TSan)
- `/tmp/vigil-stage1-physical-evidence-wuegpul2` (ext4/systemd)

Each build's result.json records binary digest and exact tokens/native path.
A portable archival copy (not an active store) is retained at
`artifacts/resilience-stage1/final/native-inventories.tar.gz`: 3505872 bytes,
SHA-256 `2edd28cb58867cc6bb2ca9c5e4a96178768f9e674cd763fead58f1fef66b4442`.

## Defects fixed and review

- Replaced crash-vulnerable RAM-only persistence admission with the transactional
  local boundary and recovery barrier, preserving old durable identity/content.
- Fixed the initial-snapshot waiter consuming shutdown's drain budget without
  admitting queued work. Snapshot/counters/records now copy atomically.
- Pinned captured loss/revision metadata together with retry payload/IDs.
- Routed pre-BEGIN validation read errors through close/reopen recovery; SQL
  step errors retain their real code instead of being coerced to corruption.
- Validated complete health/loss fields, UTF-8, duplicate keys and signed numeric
  atoms. SQLite can label an overflowing integer lexeme while materializing a
  REAL atom; that conversion is rejected before commit.
- Made checkpoint freeze an outbox invariant across every caller, including
  shutdown/recovery, with a VFS fault test that would otherwise admit a write.

Development fixture fixes: explicit first/second token barriers replace an
assumption that eight sends form one batch; ext4 filler preallocation and a
larger diagnostic pipe isolate storage behavior from transcript backpressure;
systemd signals target the main process. Linux refused live read-only remount
while SQLite held writable mappings, so physical read-only coverage uses a
stopped store and runtime READONLY uses the VFS. The VM mounts /run noexec;
the test executable used a disposable /opt directory removed afterward.

Earlier failed/interrupted gate roots and the preceding checkpoint (before the
final freeze fix) are retained, not counted as final acceptance. An earlier
fuzzer log reported a 40 s slow unit and differing timing bases; its raw artifact
is retained without assigning an unverified cause. The final run above reported
no slow unit. No hard timing guarantee follows from either run.

Final source/diff review and whitespace checks passed; 45 local Markdown targets
were checked. No temporary unit, loop mount or test daemon remains. Default
vigild stayed inactive; PostgreSQL 16/main stayed online. No production install,
production-data change, push, publication or Stage 2 implementation occurred.

## Known limitations and stop

- No PG transmission/reconciliation/retirement, migration 002 or remote benchmark
  claim. The local spool eventually fills; no disk event priority or unlimited
  retention. Legacy PG demo/scenarios describe the archived original binary.
- SIGKILL/VFS tests prove software process recovery, not actual power-cut/media
  loss survival. Honest filesystem/fsync/directory/media flushes remain required.
  Kernel I/O stalls can exceed application deadlines; systemd supplies force-stop.
- Only Ubuntu ARM64/ext4 executed. XFS is accepted but untested here. Conservative
  512/520 MiB physical maxima and all simultaneous logical maxima were not
  stress-filled; tiny limits and the trigger-growth fixture exercised policies.
- Finite vcan load, with persistence disabled, is application accounting evidence;
  no sustained throughput, physical CAN or hardware claim. Diagnostics may drop;
  regular-file output can block. CRC/counters do not authenticate; flat values
  remain suspicion. Hosted CI has not run on this local checkpoint.

**Stage 1 complete. Stop after the reviewed local commit; do not start Stage 2
or publish.** Historical design acceptance and baseline evidence follow.

---

# Historical Stage 0B checkpoint (superseded scope, retained evidence)


## Authorized scope and status

**Stage 0B COMPLETE — architecture/failure contract accepted by technical
self-review; ready for Stage 1 authorization.** One Codex agent, no delegation.
The owner requested architecture finalization and a safe local documentation
commit. No implementation, migration application, production data/service change,
push, publication or deployment is authorized. Owner review/approval of the final
contract is not inferred from the agent's acceptance assessment.

Normative design:
[failure contract](RESILIENCE_CONTRACT.md) (states, guarantees, T01–T20 assertions)
and [architecture](RESILIENCE_ARCHITECTURE.md) (schema, bounds, ownership,
transactions, algorithms and exact future interfaces). They describe intended
behavior, not features of the current binary. Stage 0's historical observations
remain in [RESILIENCE_BASELINE.md](RESILIENCE_BASELINE.md).

## Final decisions

- Main keeps the nonblocking CAN path and owns live alarms. Separate SQLite
  storage and asynchronous PG workers communicate through bounded copied slots.
- Local admission commits the immutable remote batch, original identities,
  sequence allocation and loss/snapshot capture together. Only FULL/WAL commit
  plus directory sync establishes local durability. Queue publication is volatile.
- New process means new origin; old work retains its old run/boot/time identity.
  PG verifies complete origin/content, including exact persisted batch bytes.
- Migration 002 is required and additive; 001 and existing history keys remain.
  Remote commit precedes atomic local retirement. Unknown outcomes retry unchanged;
  no exactly-once transmission promise, durable eviction or poison-head skipping.
- Limits, checkpointing, fault/start/stop behavior and /var/lib/vigil state path
  are fixed in the architecture. Replay does not restore current sensor state.

## Evidence reconciled and checks actually run

Starting Stage 0B HEAD: `60c3be2` on `main`, clean tree. Independently read the
full v3 brief, required repository records, monitoring/persistence contracts and
relevant C/SQL/build/unit/tests. Read the saved Stage 0 gate JSON and recomputed
the current implementation digest using `tests/evidence.py:source_digest`:

`b87b8f78da76c8bcfbf7c07d7f2ec26f795ca27d56bb18eedebf864479cf419b`.

It matches that gate; all ten saved command exit codes are zero. The complete
clean Linux gate **ran in Stage 0**, 2026-10-23T01:45:02.585 to 01:51:08.021 UTC;
it was **not rerun in this documentation-only stage**. No outbox test or power
cut has been executed. Prior measured counts/results are linked in the baseline,
not reclassified as new resilience evidence.

Evidence root: `artifacts/resilience-stage0-20261023T000920Z/`, primary gate at
`full-source/artifacts/stage0-full/gate.json`. Existing builds/reports/raw evidence
are preserved. Stage 0B expectations and documentation validation are under
`artifacts/resilience-stage0b/`. Checks: local Markdown links, complete T01–T20
matrix, source/scope preservation, saved-gate/hash reconciliation, diff review
and `git diff --check`. These are design/document checks, not runtime verification.

Read-only VM inspection confirmed Linux `6.8.0-142-generic`; SQLite CLI and
pkg-config development package are absent. No packages were installed. The
installed runtime library is `libsqlite3-0 3.45.1-1ubuntu2.8`. The
design requires SQLite >=3.51.3 (including WAL-reset fix), native Linux state
storage and later scoped dependency provisioning. The VM's installed vigil/PG
services and user databases were not test targets in Stage 0B.

## Review and remaining work

Reviewed local commit/ack ambiguity, repeated remote-commit SIGKILL, FIFO replay,
mailbox ownership, starvation, disk/WAL/memory ceilings, full/corrupt storage,
restart origins, signed integer exhaustion, snapshot/loss capture and old-run
acknowledgement races. PG uses a transaction advisory run lock so content
verification does not require broadening the runtime role to table UPDATE.
No unresolved product-level architecture issue remains. All durability and
performance claims still require implementation and the specified tests.

The three cached upstream documentation-only commits delete required records.
Reconciliation decision: preserve the local audited source/documents and use
this design commit as the prospective implementation base; do not pull those
deletions into it. No source divergence or code merge is needed for this design.
This resolves the baseline's base-selection question without rewriting history.

Stage 1's proposed scope is only the owner-only SQLite module, build dependency
and local tests; later daemon/replay/schema/service integration requires its own
authorization. Dependency setup and software crash/fault tests remain work, not
evidence blockers for the completed design. Actual power-cut qualification is
separate and unclaimed. **STOP: do not implement SQLite or start Stage 1 without
explicit owner instruction.** Local commit only; no push/publication/deployment.
