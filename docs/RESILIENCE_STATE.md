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
