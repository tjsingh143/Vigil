# Vigil Resilience failure contract — Stage 0B

Stage 4 release reference: [RESILIENCE_RELEASE.md](RESILIENCE_RELEASE.md),
with event-state meanings below unchanged. Acceptance wording for earlier
stages is historical; observed current results are in RESILIENCE_STATE.md.

Contract version 1, 2026-10-22 America/Los_Angeles. **Design acceptance criteria;
the full target; local Stage 1 and remote Stage 2 are implemented. Current scope and
evidence are in [OUTBOX.md](OUTBOX.md) and [RESILIENCE_STATE.md](RESILIENCE_STATE.md).**
[RESILIENCE_ARCHITECTURE.md](RESILIENCE_ARCHITECTURE.md) specifies the schema,
algorithms, numeric bounds and file interfaces. This document defines observable
meaning, allowed losses and required proof. Current evidence and scope live in
[RESILIENCE_STATE.md](RESILIENCE_STATE.md).

## States and transaction boundaries

These are evidence states, not exclusive labels for the whole system. An event
can be locally durable and remote-outcome-unknown simultaneously. 'Observed'
refers to a generated history record, including a rejected-frame or alarm record;
it is separate from a sensor measurement being valid or accepted by monitoring.

| State / transition | Owner and boundary | Surviving evidence / permitted loss |
| --- | --- | --- |
| **Observed** | Main processes a frame or alarm transition and constructs owned history data. No persistence transaction. | Monitor/callback memory only. May be lost before queue publication; a crash need not leave a count. |
| **Volatile queued** | Queue push publishes record+ticket under its mutex; return means copied to RAM, never durable. A take moves it to the storage worker's bounded held candidate. | Queue or held memory only. Pending lane eviction, shutdown deadline, process/host death can lose it. Taking it out of the queue is not durability. |
| **Rejected due to resource exhaustion** | A specific not-yet-durable record is discarded: oldest pending lane eviction, or push rejection when a bounded ticket/counter resource is exhausted. Queue returns an explicit result and counts the affected lane/reason. | Loss counters/time bounds remain volatile until captured in a local batch. Rejection never deletes durable history. No exact cross-crash loss total is promised for unsaved counters. CLOSED is a distinct shutdown result, not resource exhaustion. |
| **Locally durable** | Storage commits payload, original identity, allocators, loss/revision capture and usage in one SQLite FULL transaction; parent-directory fsync also succeeds. Only then publishes local completion. | Complete recoverable SQLite WAL/DB transaction. Process death must not lose it. Host/power-loss survival depends on the storage assumptions below. No expiration or capacity eviction is allowed. A recovered complete transaction can establish this state even if no status reached main. |
| **Remote delivery pending** | Locally committed batch has no committed local retirement. Storage selects durable head after ending its SQL read transaction and copies it into the replay slot. No network action can precede local durability. | Original payload and order/run/batch/seq remain in SQLite. It may already exist remotely after an earlier attempt. A restart treats all remaining rows this way. |
| **Remote outcome unknown** | PG submitted or might have submitted the statement, but cannot validate complete successful response/ReadyForQuery, or process died around submission. No local retirement. | Same SQLite row survives. PG may contain the entire transaction or none; never infer 'not committed' from timeout/disconnect. Resend exactly the same bytes. For a new process, whether any past send occurred may itself be unknowable. |
| **Remote committed** | Actual PG transaction commits run/origin, all history, checkpoint and exact receipt atomically. PG worker knows success only after a complete validated inserted/equal result and idle transaction state. | PG receipt/history are server evidence; the client confirmation is initially RAM. Local row remains until retirement. If confirmation vanishes, restart safely re-verifies equal receipt. The receipt identifies content, not just a key. |
| **Locally acknowledged / retired** | Storage receives matching head success, commits deletion + usage/retired-prefix/totals, and fsyncs directory before releasing slot/publishing acknowledgement. | PG contains verified history/receipt; SQLite has the retired prefix and cumulative counts. No local payload retention promise after this boundary. Death before local retirement leaves a safe retry; death after it must not resurrect an unretired row. |

**An individual event may be called durable only after its complete local
admission transaction and directory sync succeed, or validated recovery seals
the same committed evidence with a successful FULL transaction and directory
sync.** 'Queued', 'accepted', 'taken', 'sent', 'SQL statement started' and a
WAL-mode setting alone do not qualify. Status must say **locally durable** or
**remote committed**, rather than an ambiguous 'saved'. A successfully retired
event remains remotely durable under PG's durability assumptions; it is no
longer retained locally. An I/O error with uncertain local outcome is not an
acknowledgement of durability; preserve the candidate until reconciliation.

## Required invariants and exclusions

| ID | Guarantee / limit | Proof cases |
| --- | --- | --- |
| G1 | Main's CAN/timer/signal path does no DB/fsync/network work; only bounded copies under locks. PG stalls cannot hold the queue lock or prevent the storage worker attempting new local admission. No hard real-time or universal fsync latency guarantee. | T01, T12, T17, T19 |
| G2 | Every locally durable, unretired input remains byte-identical and replayable after process death; a batch is stored with transactional IDs, not reconstructed under a new run. All local transactions are whole or absent. | T02–T05, T08, T10 |
| G3 | Local retirement requires content-verified remote commit. Any uncertain remote outcome retains local evidence. Retried effects are idempotent; network transmission is **not exactly once**. | T05–T09, T16 |
| G4 | Run UUID plus origin metadata/token and per-run transactional counters distinguish restarts. A conflicting key never silently adopts someone else's data. Prior-run replay cannot update current live sensor/alarm truth. | T08–T11, T16 |
| G5 | All application buffers, outstanding work, logical storage and SQLite file growth are bounded as specified in the architecture. Durable data is never evicted to make room. Full/failed local storage degrades monitoring, with bounded volatile losses; startup cannot advertise a nonexistent local durability service. | T03, T12–T15, T17 |
| G6 | Saved known losses and snapshot revisions advance atomically with their immutable batch, once per local capture and once per remote retirement. No invented prior-process crash gap and no counter double counting. Snapshot state is historical and may be newer/older than individual batch observations. | T04–T06, T10–T12, T18 |
| G7 | Eventual replay is conditional on a nonpoison head, available functioning storage/PG, continued operation and adequate capacity. Strict FIFO and independent admission provide fairness, not a delivery deadline under overload. No automatic poison skip, partial batch success or mutable retry. | T07, T09, T16, T17 |
| G8 | Setup/reopen/corruption failures are explicit; no silent fresh DB, volatile fallback, unsafe sidecar deletion or automatic data-loss recovery. Shutdown distinguishes volatile abandonment from durable backlog and does not free shared state before joins. | T02, T05, T13–T15, T18 |

SQLite FULL/WAL assumes a supported Linux local filesystem, functioning fsync
and directory sync, correct SQLite/kernel/storage, and persistent media that
honors flush ordering. Media loss, corruption, admin deletion, broken flushes,
storage rollback and cloned concurrently used spools are outside the survival
guarantee. A running duplicate opener must fail the advisory ownership lock;
an offline clone must not be operated as a second independent spool. UUID/token
entropy is assumed sound; content/origin mismatches still fail closed.

Remote durability requires PostgreSQL fsync/full_page_writes and synchronous
commit enabled, honest server/storage, and no destructive history/receipt edits
or restoration behind acknowledged history. No standby/failover durability
promise beyond that configured PG server. Local retirement cannot protect
against later remote data destruction. CRC/counters do not authenticate CAN;
flat values remain suspicion, and vcan does not establish physical CAN behavior.

SIGKILL tests process recovery; **they do not prove power-cut survival**. Host
power loss may lose every input before the local boundary. Runtime failed
storage may lose admission opportunity while live alarms still progress.
Kernel CAN loss, diagnostic drops, volatile history drops and stale/plausibility
alarms are different observations; never add them as independent missing inputs.

## Reporting and accounting

Queue return codes identify the current record's RAM result; QUEUED_EVICTED
also reports that a different, older ticket was discarded. A held candidate
blocked on FULL/IO/NOMEM is still volatile, not rejected and not durable. Queue
overflow can continue behind it. No producer waits for disk space. Initialize
fixed worker/queue/mailbox allocations before READY; initialization OOM fails
startup. Runtime allocation or disk failures publish a bounded error enum,
extended SQLite code or five-character SQLSTATE, not credential-bearing text.

Expose run_id and these separate dimensions in each consistent status copy:
local state (ready/full/recovering/fault), remote state (disconnected/pending/
unknown/confirmed/poison), volatile lane/held counts, locally durable pending
records/batches/bytes, current-run local-admitted and retired counts, lifetime
spool totals, loss totals by lane/reason, captured and remotely acknowledged
loss/revision cursors, last error, oldest pending order and replay attempts.
'records_written' retains its meaning of remotely confirmed inputs, advanced
only once by local retirement; an inserted/equal receipt is not an inserted-row
count. 'Locally admitted' is a new counter. Snapshot/status publication is a
copy of committed progress and may lag it; never infer an event durable solely
from its observation timestamp or a high-water ticket with evicted tickets.

For each live process and lane, while exact counters remain representable:

```text
observed_for_persistence = queued + resource_rejected + closed
queued = evicted_before_local + volatile_pending + held + locally_admitted
locally_admitted = current_run_durable_pending + current_run_retired
spool_admitted = spool_pending + spool_retired
```

The first equation counts QUEUED_EVICTED as queued for the new input; eviction
is counted against its older input in the second equation. Checkpoint metadata
is not a history input. Remote unknown/confirmed-before-retirement belongs to
durable_pending, not an additional term. On shutdown, volatile abandonment is
the final pending+held subset; durable backlog is deferred, not abandoned.
After restart, old-run volatile totals are unknown except committed captures;
do not reuse these process equations to invent their loss. Persisted counters
and arithmetic must fit signed bigint; exhaustion stops local writes, while
volatile diagnostics saturate and set count_exact=false rather than wrap.

Main raises persistence_degraded for local-not-ready/full/fault, remote failure/
poison/unknown, or newly observed loss. Once raised, clear only when local and
remote are healthy, volatile+held+durable backlog is zero, and the **current
run's** loss/revision is remotely acknowledged. Slot results include run_id;
an old origin's revision must never satisfy this predicate. The clear transition
and its new snapshot then follow the normal queue path. Keep a main-owned loss
observation cursor so fast worker acknowledgement cannot hide new loss. Status
lines can drop under the existing diagnostic policy; no guaranteed external
alarm delivery is added.

## Deterministic failure controls

Build a separate `vigild-resilience-test` and test outbox library with
`VIGIL_TEST_FAILPOINTS`; production must not link the failpoint dispatcher,
test VFS, control parser or test FD protocol. No production environment variable,
CLI flag or SQL object enables these controls. Production binary checks must
assert their absence. Preserve the existing no-persistence mode as diagnostic,
not a failpoint. Test executables are not installed by the service installer.

Each hook has a name, exact hit number and optional order/run/batch filter.
Test-only I/O wrappers assert the calling worker identity and that no queue or
mailbox mutex is held at SQLite/libpq/fsync entry, providing a machine oracle
for G1's ownership rule.
A preopened test pipe reports a fixed-size hit token, then blocks only the
selected worker on a release FD. Harness uses that handshake to SIGKILL/release;
never rely on a sleep to guess a commit boundary. Real disposable PG triggers
can pause before transaction completion; a proxy waits until an independent
observer sees the committed receipt, then suppresses the actual response.
Never create hooks/functions/triggers in an installed/user database.

Required hooks: after_queue_publish; after_take_before_begin; after_local_begin;
after_local_insert_before_commit; after_local_commit_before_publish;
before_remote_send; after_remote_send; PG after_history_before_checkpoint;
PG committed_response_withheld; after_remote_confirm_before_result;
before_retire_begin; after_retire_delete_before_commit;
after_retire_commit_before_publish; before/after_wal_checkpoint;
after_startup_origin_commit. COMMIT hooks bracket the actual SQLite return;
directory-sync hooks distinguish success from namespace-sync failure. A test VFS
injects exact xWrite/xSync failures and SQLite OOM without changing production
transaction algorithms. An injected error after actual successful commit tests
lost local confirmation and reopening, not a claim about every storage failure.

## Required implementation acceptance matrix

Every row records its expectation **before** execution, uses disposable state,
exits nonzero on a failed assertion, and saves SQL/local inventory plus exact
binary/source digest and failpoint tokens. 'Local inventory' is a stopped,
recovered SQLite DB with schema/origin/order/payload/counter checks. Tests compare
full identities and bytes, not merely aggregate row counts. This is the full target matrix. Stage 1's local subset and observed results
are recorded in RESILIENCE_STATE.md; remote crash/commit assertions are implemented by Stage 2.

| Test | Deterministic setup | Machine-checkable assertion |
| --- | --- | --- |
| T01 Monitor regression | Original strict Debug/Release, ASan/UBSan/TSan, queue/monitor/protocol gates; real Linux vcan, timerfd, signalfd and systemd. | Original sensor/rejection/hysteresis/timeout/counter invariants pass, queue has no I/O call path, sanitizer/analyzer/Helgrind failures=0; watchdog only from main. |
| T02 Local crash cut | SIGKILL after take, BEGIN, INSERT, COMMIT before status, and origin commit; repeat >=20 restarts with stopped PG. | Pre-COMMIT hooks: candidate absent. During uncertain commit: whole or absent. After successful commit+directory sync: exact batch/records/allocators present. Every baseline confirmed local identity survives; no torn row, reused ID or allocator-only advance. Newly observed volatile losses are not invented as known historical gaps. |
| T03 Admission/identity bounds | Tiny test capacities, maximum snapshot/detail, invalid integers/JSON, allocator at INT64_MAX sentinel, three forced UUID collisions, entropy/OOM failure. | Exact boundary accepts; next excess stays held or rejects by specified code; usage never exceeds caps; byte lengths and seq ranges exact; no overflow/truncation, partial transaction or reused identity. Startup failures emit no READY. |
| T04 Capture races | Pause admission after copying drop totals/revision; cause new evictions and snapshot publication; resume, crash/reopen. | First batch captures only its copied totals/revision, next captures remaining delta; gap sums equal committed captures exactly once. Snapshot has complete entries, original run and cutoff; lost intermediate revisions are never claimed complete transition history. |
| T05 Local uncertainty | VFS write/sync errors, directory-sync failure, error after successful admission/retire commit. | No unsealed durability/success publication; candidate/slot remains bounded. Reopen resolves equal row or committed retired prefix; retry is byte-identical. Mismatch latches fault; no double admission/retirement, no lost confirmed local ID. |
| T06 Remote rollback | Block/fail real PG after history insert but before checkpoint/receipt; kill connection. | Run/origin/history/checkpoint/receipt are wholly present or absent together; no local retirement on failure. Healthy retry commits the complete exact batch once. |
| T07 Remote ambiguity | Submit through response-withholding proxy; observer confirms PG receipt; kill daemon before result delivery, repeat >=20 times. | Receipt bytes unchanged; one history row per original key, one checkpoint/receipt per batch, exact gap totals; local row survives until final retirement; attempts >1 proves no exactly-once-send claim. |
| T08 Retirement crash cut | Kill after remote confirmation, before retirement, after DELETE before commit, after commit before publish. | Before committed retirement, local payload replays to equal PG receipt; after committed retirement, retired prefix/totals advance once and row stays absent. Remote IDs/content unchanged in every restart. |
| T09 Duplicate verification | Concurrent two v2 clients: same bytes; same batch key altered bytes; different batch with equal/unequal existing event key; checkpoint without receipt. | Equal duplicates succeed with identical rows; inconsistent batch/event/origin or legacy checkpoint fails VG001, transaction adds zero partial rows and local head is retained. Concurrent attempts cannot both insert inconsistent content. |
| T10 Restart origins | Two process runs with stopped PG, prior-boot fixture metadata and deliberately regressed UTC; repeated kills during replay. | SQL preserves original run/token/boot/start/observation metadata and seq; new run differs; FIFO order follows durable order, not UTC; replay never refreshes current source or restores prior alarms. |
| T11 Identity collision | Force new run UUID to match a remote legacy/v2 run, change token/origin while retaining same keys. | No remote run adoption or history mutation; VG001 latches poison, local evidence remains; new-process status/revisions stay scoped to new run. |
| T12 Logical full/overflow | Test limits individually for events, batches and payload bytes; stop PG, hold storage; exercise both volatile lanes then free one remote batch. | Durable inventory/bytes never evicted or increased past caps; exactly min(M,N) lane entries retained with max(0,M-N) evictions when held candidate excluded; reading/event losses separate; retirement frees capacity and held admission resumes. Conservation equations hold at synchronized cuts. |
| T13 Physical faults | Loop-mounted small ext4 for ENOSPC; read-only remount; BUSY via unsupported opener; corrupted DB/WAL copy; oversized sidecar. | FULL/READONLY/BUSY pace retries without spin; corruption/inconsistency latches fault without deleting files; main freshness/high alarms continue after runtime failure. Reopen fails before READY for invalid storage, preserving original bytes. |
| T14 WAL/memory growth | Sustained append/replay, 4096 metadata batches, repeated restart, forced checkpoint BUSY/failure, heap-limit/OOM injection. | Logical caps, DB <=512 MiB, WAL <520 MiB, SHM <=2 MiB and managed file lengths <=1056 MiB; no next mutation after failed required checkpoint; no per-retired-batch ledger growth; origin rows <=4097; buffers constant vs backlog, SQLite heap <=32 MiB. Resource fault reported, no durable eviction. |
| T15 Startup/service | Missing store, wrong ownership/symlink, schema/application mismatch, duplicate daemon, unwritable directory; valid full spool with PG offline; real temporary hardened unit restart. | Bad cases exit 2/no READY; valid full store becomes ready/degraded and replays; only service state directory writable, files 0600/dir0700, UID vigil; normal stop/restart preserves exact local inventory and main watchdog still detects a stopped main. |
| T16 Poison/config/partial | Invalid committed payload fixture, PG CHECK failure, VG001/VG002, missing 002/permission denial, lock/statement failures, blackhole. | Poison head never skipped/split/acked; local admission continues to cap. Permission/schema retries >=30 s; transient backoff intervals follow 1/2/4/8/16/30 s within measured scheduling allowance, never shortened by enqueues. No v1 fallback. Repaired config resumes exact head. |
| T17 Scheduling/fairness | PG blackhole/backoff and a large recovered backlog while new paced records arrive; inject one stale/high source, then restore PG with service rate above offered load. | New locally committed identities increase before PG returns; no worker holds queue lock during I/O; admitted new work and old FIFO work both retire after recovery; source timeout never early and <= timeout+tick+200 ms in this fixture; report queue/commit/retire latency and drops, do not infer hard real time. |
| T18 Shutdown/counters | SIGTERM with reachable/offline PG, durable backlog and known held/queued records; kill at drain deadline; old-run success concurrent with new-run alarm revision. | Ordinary responsive-storage stop <=1.25 s (record actual); supervisor kills a stalled stop by configured 3 s plus scheduling allowance. Only volatile pending+held counted abandoned; every locally confirmed input survives; old ack cannot clear current degraded; status conservation and retired totals reconcile with SQLite/SQL. |
| T19 Benchmark/regression | Original natural/forced-overflow 50001-frame workload with --no-persistence; separate fixed 100 records/s, 60 s, full persistence test with PG healthy/offline/returning. | Original sent=received+kernel drops and receive classifications hold; no changed claim of physical/sustained throughput. Persistence benchmark records offered/received/local/remote counts, per-lane loss, missed ticks, p50/p95/p99 enqueue-to-commit and replay latency, max lock hold, RSS and all file sizes. Paced healthy fixture has zero volatile history drops and no early/late timeout beyond T17 allowance; report measured latency, no invented performance claim. |
| T20 Failpoint isolation/power evidence | Inspect/install production artifact; run test binary's token handshake; VFS fault/barrier trace matrix; optionally abrupt disposable VM power cut after committed inventory. | Production symbols/options/environment cannot activate hooks and installer excludes test executable; test hook hit/order assertions are exact. Every published local success follows successful WAL sync and parent-directory sync in the trace; injected sync failure publishes none. SIGKILL/VFS results labeled separately. Only an actually executed abrupt-power-cut suite may add power-loss evidence; verified committed inventory must survive it, otherwise durability gate fails. |

## Original gates and acceptance boundary

Keep `scripts/check`, `check-ci`, `check-release`, `verify`, decoder fuzzing,
manual CAN exchange and original benchmark evidence independently runnable.
No-persistence monitor/load cases retain their original meaning. The existing
five v1 database contracts continue testing 001 alone; add v2 contracts on
001+002. When daemon integration changes, give persistence fixtures a unique
native-Linux outbox path and apply 002 in their disposable clusters. Update
crash/overflow expectations explicitly for the new local boundary; preserve
the original controlled volatile-loss case by killing before local admission.
Do not keep an obsolete 'PG outage always fills N RAM slots' assertion for a
working disk spool, or delete a regression merely because it became harder.

Future `scripts/check-resilience` runs this matrix in addition to the original
complete clean Linux gate. Include Release, ASan/UBSan and TSan integration,
GCC/Clang strict builds/analyzer, Helgrind for shared mailboxes, and full SQLite/
PG contract tests; no macOS fake bus. Verify the tests' new expected counts in
the evidence renderer. Use a fresh isolated checkout/build/output root; the
existing verifier overwrites its checkout's VERIFICATION.md. Evidence records
native Linux paths/filesystem, SQLite version/source, PG settings, exact commands,
exit codes, inventory and measurements. Hardware power-cut qualification is
separate from passing software process-crash tests and stays unclaimed until run.

Stage 0B acceptance is a source-grounded, internally reviewed design with no
open product decisions. Stage 1 readiness means the specified local module
can be implemented after explicit owner authorization. It does not mean the
outbox exists, these tests passed, or deployment/production changes are approved.

## Stage 1 acceptance slice

The owner authorized daemon/local integration for this checkpoint. Tests cover
local identity/append/rollback/order/reopen, logical/physical limits, capture
races, storage faults, volatile lane reserve, shutdown, real process kills and
original live monitoring. Remote transmission, acknowledgement, retirement,
collision verification against PG and the remote benchmark are not accepted or
implemented by Stage 1. Existing v1 SQL contracts remain regression evidence
for migration 001, not proof of v2 replay. Local admission never increments
remote-written counters. Durable older history remains pending at capacity;
only volatile inputs can be rejected/evicted. Production/test artifacts remain
separate. SIGKILL evidence does not qualify hardware power-loss behavior.

## Stage 2 acceptance slice and justified refinements

Stage 2 implements remote rollback/ambiguity, origin/content equality and
conflicts, FIFO replay across runs, atomic local retirement/recovery, bounded
retries, poison handling, independent admission, lifecycle and bounded shutdown.
`tests/resilience_replay.py` maps the owner's A-J boundaries to deterministic
pipe hooks or a real-PG transaction pause/response-withholding proxy. Direct
inventories and SQL reconcile keys, payload bytes, history, snapshots, gaps,
pending/retired prefixes and counts. `resilience_database_contract.py` tests
repeatable 001+002, runtime privileges, concurrent equality/conflicts and rollback.
Current observed results are in RESILIENCE_STATE.md; matrix entries are design
requirements, not evidence by themselves.

The architecture amendment records the auxiliary invoker validator, conservative
30-second classification of post-handshake startup failures (libpq has no startup
SQLSTATE accessor), and retention of late shutdown results for next-start replay.
No success or local retirement follows an incomplete remote response. None of
these refinements weakens payload identity or the local durability boundary.

The Stage 3 persistence benchmark/instrumentation, hardware power cuts, physical
CAN qualification and remote retention remain outside this checkpoint. Stage 2
acceptance does not claim every optional/full-target matrix workload was run.
Stop after the local Stage 2 commit; no Stage 3, deployment or publication.
