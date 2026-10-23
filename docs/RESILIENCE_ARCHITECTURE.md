# Vigil Resilience architecture — Stage 0B

Design version 1, 2026-10-22 America/Los_Angeles. **Full architecture target.**
Stage 1 local implementation/status: [OUTBOX.md](OUTBOX.md),
[RESILIENCE_STATE.md](RESILIENCE_STATE.md). Remote sections remain future work.
The normative failure meanings and acceptance tests are in
[RESILIENCE_CONTRACT.md](RESILIENCE_CONTRACT.md); actual baseline evidence is in
[RESILIENCE_BASELINE.md](RESILIENCE_BASELINE.md). The owner subsequently authorized Stage 1 implementation and daemon integration.

## Source basis and scope

Independently inspected `src/{vigild,queue,persistence,monitor,protocol}.c`,
the queue/persistence headers, `migrations/001_history.sql`, the service,
CMake, provisioning/check scripts and persistence/database/evidence fixtures.
Read the entire eight-page v3 brief and required state, decisions, learning,
monitoring and persistence documents. Starting HEAD is `60c3be2`; tree clean.
The current implementation digest equals the saved Stage 0 gate digest,
`b87b8f78da76c8bcfbf7c07d7f2ec26f795ca27d56bb18eedebf864479cf419b`;
all ten saved command exits are zero. No contradictory baseline evidence remains.

The cached upstream commits `c66b28d`, `cd579fb`, `f7a2208` change documentation
only and delete required engineering records. This design deliberately retains
the local baseline and those records. Use this local design checkpoint as the
Stage 1 base; do not merge those deletions as a prerequisite. No fetch, pull,
push, deployment or installed-database operation is part of this checkpoint.

Necessary extensions to brief §§3.3–3.4 and the original persistence contract:
add a second persistence worker and recover committed local history; retain
volatile drop-oldest ingestion, but never evict durable history. Readiness now
requires a working local store. The original implementation remains unchanged;
its documented volatile crash losses remain accurate for that binary.

## Owners and handoffs

```text
main: nonblocking CAN / timerfd / signalfd / live alarms
  -> N-reading + 256-event volatile queue
  -> SQLite storage worker: local transactions / frozen batches / retirement
  -> WAL outbox
  -> one bounded replay slot -> PostgreSQL worker: libpq / network / backoff
  <- one bounded result slot <- confirmed success or failure
```

Main never opens SQLite/libpq or waits for a durability acknowledgement. Keep
64 receive attempts per poll turn and all current sensor policies. Only main
changes live alarms or sends the watchdog. Recovered observations and snapshots
are history; never feed them into the monitor or compare old-boot monotonic
timestamps with current freshness clocks.

The storage worker owns the **only SQLite connection**, statements, allocators,
checkpointing and directory lock. The PG worker owns the only `PGconn`, query
buffers and retry deadlines. Neither worker receives a monitor pointer. Signals
are blocked before either thread starts. All shared values have owned storage.

Keep separate queue and replay mutexes; never nest them. Under the queue lock,
copy at most 32 records, counters and the latest complete snapshot in one take.
Under the replay lock, copy one work/result value and change slot state. No
serialization, SQLite, fsync, libpq, diagnostics or other I/O under either lock.
Use monotonic condition waits with predicate checks under the matching lock.

One work slot follows `empty -> ready -> sending -> result -> empty`; it holds
an order ID, original run/batch ID, length and at most 160000 payload bytes.
One result slot holds those IDs, outcome and five-character SQLSTATE/local code.
PG makes its own fixed payload copy outside the lock; storage retains the SQL
row until retirement. Only storage releases the slot after handling its result.
No list of submitted batches, unbounded acknowledgements or PG SQLite reads.

Each storage turn handles at most one result, one admission batch, then one
dispatch. It must attempt admission even while PG is connecting, retrying or
holding an ambiguous batch. A local capacity failure may pin one uncommitted
32-record candidate; retry that candidate before taking another. It does not
prevent handling a remote acknowledgement that can free local capacity. Idle
wait/probe cadence is at most 10 ms for available ingestion, with immediate
wakeup at 32 records; this is a scheduling target, not a commit latency promise.

## Identity and immutable unit of work

Keep PostgreSQL history key `(run_id,seq)` and checkpoint key `(run_id,batch_id)`.
Queue `ticket` replaces the current enqueue `seq`: it orders volatile records
within the new process and is never a promised remote identity. Storage assigns
history `seq` only in the transaction that inserts its payload. Admission freezes
the **remote batch at the same time**, so there is no second batch-formation
transaction, regrouping after restart, or separate event table to reconcile.

An initialized spool has a random v4 `spool_id` and transactional `next_run_no`.
Every process start commits a new origin row before admission: a new random v4
`run_id`, monotonically allocated `run_no`, independent 256-bit random
`run_token`, Linux `/proc/sys/kernel/random/boot_id`, UTC/monotonic start times.
Use `getrandom`, with no time/PID fallback; entropy failure fails startup.
Locally conflicting random IDs get at most three generation attempts, then fail.
`run_token` is public identity evidence, not authentication. UUID uniqueness is
probabilistic; conflicting origin/content is detected and fails closed remotely.

Old batches always retain their original origin, timestamps, sequences and
batch IDs. A new process never allocates into an old run. Within one admission
transaction, reserve the next contiguous sequence range, one per-run batch ID
and one spool-global `order_id`; store payload and advance all allocators
atomically. Rollback allocates nothing externally visible. All persisted IDs,
counts and milliseconds must fit nonnegative signed bigint. IDs range from 1
through `INT64_MAX-1`, leaving a representable exhausted next-value sentinel.
Check addition before conversion; exhaustion stops admission without wrapping.
UTC milliseconds are additionally limited to 0..253402300799999 (through year
9999), safely inside PostgreSQL timestamp range; invalid startup time fails
startup, and a later out-of-range wall clock pauses admission as INVALID.

Payload format 2 is UTF-8 JSON with no embedded NUL or duplicate object keys.
It contains the existing v1 batch fields, plus `format:2`, `order_id`, `origin` with the
fields above, `capture_ticket`, snapshot revision and explicit local-loss
reason totals. `run_id`/start fields agree with `origin`. `records` has 0..32
entries, increasing contiguous assigned seq; each retains existing typed fields
and detail. `alarms` is null or a complete 1..193-entry copied snapshot; its
entries are not history rows and have seq zero. Validate every kind, field,
integer range, byte length and JSON shape before commit; no truncation/coercion.
Use the existing serializer's fixed bound and SQLite JSON functions for local
validation; PG checks the input as JSON before JSONB conversion so duplicate
keys cannot disappear during parsing. No new general-purpose JSON library.

Record-bearing batches preserve ticket order after volatile evictions. Different
runs and metadata-only batches are ordered by `order_id`, never wall time.
UTC may jump; monotonic time is meaningful only with the recorded boot/run.
Snapshot capture is an independent observation: `capture_ticket` says which
publications preceded it, not that its alarm revision describes every record
in this batch. The first nonempty monitor snapshot is published before enabling
storage ingestion, removing the existing empty-initial-snapshot race.

## SQLite schema version 1

Three tables, all `STRICT`, with explicit `NOT NULL` except `first_seq` below.
UUIDs are canonical lowercase TEXT, tokens 64 lowercase hex characters; enforce
length/shape, enumerations, nonnegative counts and signed ranges with CHECKs.
Use `application_id=0x5649474c` and `user_version=1`. No persistent retry list,
event mirror, acknowledgement table or tombstone per retired batch is needed.

| Table | Columns and constraints |
| --- | --- |
| `spool_meta` | `id INTEGER PRIMARY KEY CHECK(id=1)`, `spool_id TEXT UNIQUE`, `next_run_no INTEGER`, `next_order_id INTEGER`, `last_retired_order INTEGER`, `recovery_generation INTEGER`, `pending_records INTEGER`, `pending_batches INTEGER`, `pending_payload_bytes INTEGER`, cumulative `admitted_records/admitted_readings/retired_records/retired_readings INTEGER`. Next IDs >=1, cursors/totals >=0; readings <= records; pending values within limits below. Exactly one row. |
| `origin_runs` | `run_id TEXT PRIMARY KEY`, `run_no INTEGER UNIQUE`, `run_token TEXT UNIQUE`, `boot_id TEXT`, `started_utc_ms/started_ms INTEGER`, `next_seq/next_batch_id INTEGER`; `admitted_records/admitted_readings/retired_records/retired_readings INTEGER`; `captured_reading_drops/captured_event_drops/captured_revision INTEGER` and corresponding `retired_reading_drops/retired_event_drops/retired_revision INTEGER`. Origin immutable; allocators >=1; cursors/totals >=0, retired <= captured/admitted and readings <= records. Spool ID comes from the singleton and is embedded in every payload. |
| `outbox_batches` | `order_id INTEGER PRIMARY KEY` (explicit allocation; no rowid allocator), `run_id TEXT REFERENCES origin_runs ON DELETE RESTRICT`, `batch_id INTEGER`, `first_seq INTEGER`, `record_count INTEGER CHECK(0..32)`, `reading_count INTEGER CHECK(0..record_count)`, `payload TEXT`, `payload_bytes INTEGER`; UNIQUE(run_id,batch_id). `first_seq IS NULL` iff count=0; otherwise >=1 and range end <= INT64_MAX-1. `payload_bytes=length(CAST(payload AS BLOB))`, 1..160000; `json_valid(payload)` and records-array length = record_count. Required JSON types/keys must explicitly reject NULL/missing values. |

An UPDATE trigger rejects **all** updates to `outbox_batches`. No INSERT OR
REPLACE and no automatic cascade deletion. Owner-side conditional allocator and
usage updates in `BEGIN IMMEDIATE` must each affect exactly one expected row.
Validate JSON identities, consecutive record seq, origin equality, reading count
and capacity before INSERT. Reopening independently checks these cross-row
invariants and pending sums; constraints alone do not validate application JSON.
The primary order index selects the head; UNIQUE(run_id,batch_id) also supplies
the run prefix for cleanup. No observation-time sorting or full backlog load.

Maintain `last_retired_order` as the contiguous retired prefix. Pending orders
must be exactly `last_retired_order+1 .. next_order_id-1`. This survives an
ambiguous local DELETE commit without retaining an unbounded receipt ledger.
Keep origin rows only for the current run and runs with pending batches; delete
empty old origins during startup/retirement transactions. Thus origins <=4097,
including repeated crashes that produced no observations. Never reuse run_no.

## Transactions and loss metadata

**Bootstrap/start:** an explicit future `scripts/setup-outbox` initializes only
an empty, owner-controlled directory, commits schema/meta with fresh spool ID
and fsyncs the directory. Normal daemon startup opens an existing DB without
CREATE; a missing DB or lone sidecars is an error, not a fresh empty history.
Storage locks, opens, recovers/validates, prunes empty origins and commits the
new origin/allocator in one startup transaction. SQLite sync plus directory
fsync must succeed before the storage-ready latch. No PG connection required.

**Admission:** copy the bounded candidate and snapshot/cumulative volatile loss
counters; release queue lock. Begin immediate; check capacity and allocator
values; insert the exact serialized batch; advance run/global counters and
captured drop/revision cursors; COMMIT FULL and fsync parent directory. Only
then publish local durability and clear the held candidate. Never advance a
captured cursor in memory alone, or subtract losses at remote success time.
The candidate's bytes/IDs remain pinned until any uncertain local outcome is
resolved. New loss/snapshot changes wait for the next admission, never rewrite
an older batch. Metadata-only admission is allowed on loss/revision changes.
Batch kind is gap when copied loss delta is nonzero, otherwise recovery for
the first origin capture or a pending recovery capture, otherwise flush. First
capture, gap, revision change and recovery carry a complete copied snapshot.
After the first confirmed PG success following a failure, storage sets one
coalescing recovery-capture flag; the next newly admitted batch captures current
health/snapshot without altering retried work. Keep that flag until its local
commit; repeated failures cannot create an unbounded metadata queue.

Captured drop deltas produce one immutable `gap` checkpoint. Reasons distinguish
volatile lane eviction and resource rejection; preserve separate reading/event
totals and cumulative lost-observation time bounds. They are not exact missing
sequence intervals. A capture can report losses whose tickets postdate some
batch records; its cutoff is explicit. Coalesce unsaved snapshot revisions;
do not persist every intermediate revision as a separate promise. No periodic
empty outbox batches just to probe PG: use PG's bounded liveness query instead.

**Retirement:** accept only a successful result naming the current durable
head's order/run/batch and expected record count. In one immediate transaction,
delete exactly that head, advance `last_retired_order`, decrement all pending
usage, increment run/spool retired totals, advance only that run's retired
loss/revision cursors from the frozen payload, and prune its origin if old and now empty.
COMMIT FULL plus directory fsync precede publishing remote-written counters,
acknowledged loss/revision cursors and releasing the replay slot. Update all
related shared counters in one queue-lock publication; only a matching current
run may advance its alarm/loss acknowledgement cursors. Old-run results update
spool backlog/totals, never the new run's live health cursor. A failed local
retirement never counts the input twice. On restart, rows remaining are replayed;
rows absent inside the committed retired prefix need no replay.

## Bounds, settings and physical allocation

These fixed v1 maxima are admission ceilings, not a promise that every ceiling
is simultaneously reachable. The first exhausted resource stops admission.
Production has no unlimited setting. Only volatile reading capacity retains
its existing CLI range; no runtime disk-limit tuning in this stage's design.

| Resource | Maximum / behavior |
| --- | --- |
| Volatile history | N readings (default 1024, 1..65536) + 256 reserved events + one held candidate of 32. Full lane evicts its oldest queued record; held candidate is pinned. |
| Durable history | 65536 unretired records, readings and events combined; 4096 batches including metadata-only batches; 64 MiB total payload bytes. No durable eviction or disk event-priority claim. |
| Fixed values | 384-byte record detail including terminator, 32-byte kind including terminator, 193 snapshot entries, payload 160000 bytes + one C terminator. Existing record detail limit remains unchanged. |
| Work buffers | One admission serializer, one shared replay payload and one PG payload copy; one held 32-record array; queue/latest and storage-captured snapshots. No arrays sized by backlog. |
| SQLite memory | 32 MiB hard SQLite heap limit, 2 MiB page-cache target, mmap disabled, temp_store=MEMORY; allocation failure is an explicit storage error. Worker stacks fixed at 1 MiB each. Application queue memory is bounded by N*sizeof(record) plus fixed arrays. |
| Database file | 4096-byte pages; `max_page_count=131072` (512 MiB including free pages/indexes). No online VACUUM, ATTACH, schema mutation or auxiliary spool files. |
| WAL / SHM | Truncate checkpoint before another mutation when physical WAL >=4 MiB; stop mutations if truncation cannot complete. Auto-checkpoint disabled. See conservative hard bound below; 4 MiB is a trigger, not a hard WAL limit. |

Require Linux native ext4 or XFS storage (VM ext4 for tests, **not** the Lima host
share, tmpfs or a network filesystem); reject other filesystem types at open.
SQLite library minimum 3.51.3, compiled
with JSON/STRICT and thread safety; check runtime version and capabilities too.
This includes the upstream WAL-reset fix; older system packages are not silently
accepted. Stage 0B found no SQLite CLI or pkg-config development package in the
VM; installed runtime library is 3.45.1-1ubuntu2.8. Dependency acquisition is
future scoped setup, not performed here.

Set and read back `journal_mode=WAL`, `synchronous=FULL`, `foreign_keys=ON`,
`page_size=4096`, `max_page_count=131072`, `wal_autocheckpoint=0`,
`journal_size_limit=4194304`, `cache_size=-2048`, `cache_spill=OFF`,
`mmap_size=0`, `temp_store=MEMORY`, `busy_timeout=0`. Set the C SQLite hard heap
limit to 32 MiB and value/row length limit to 192 KiB; bound SQL length to 64 KiB.
No long read transaction survives copying one batch; finalize/reset statements
before network dispatch. Checkpoint and WAL reset run only on the owner thread.

With cache spilling disabled, one transaction appends at most one WAL frame
per dirty DB page at commit. With the DB page cap, no concurrent connection,
and no next mutation past the checkpoint trigger, conservatively bound WAL by
`4 MiB + (131072+1)*(4096+24) + 32 < 520 MiB`, SHM by 2 MiB, and DB+sidecars
by 1056 MiB including fixed lock/directory overhead. Test these physical sizes;
these are managed file-length bounds, excluding filesystem journals, snapshots
and allocation metadata. Do not present payload bytes or journal_size_limit as
a disk quota. Provision
at least 2 GiB free before initialization; later competing disk use may cause
ENOSPC earlier. Oversized existing files fail validation without deletion.
Freed DB pages are reused; retirement does not promise file shrinkage. Run
PASSIVE checkpoint on idle/one-second housekeeping and TRUNCATE at the trigger
or clean close; an unsuccessful checkpoint freezes further mutations until
the problem clears. Replay may retain evidence but cannot retire during this
freeze. There is no second checkpoint connection to pin/grow the WAL.

WAL plus FULL is the SQLite power-loss durability basis; flushing and directory
entry persistence must actually work. See [SQLite WAL](https://www.sqlite.org/wal.html),
[synchronous and size pragmas](https://www.sqlite.org/pragma.html),
[runtime limits](https://www.sqlite.org/c3ref/limit.html) and
[WAL checkpoint API](https://www.sqlite.org/c3ref/wal_checkpoint_v2.html).

## Reopening, faults and service lifecycle

Acquire a nonblocking exclusive `flock` on `outbox.lock` for the entire process
life; every supported administrative opener takes it too. Refuse duplicate
instances, symlink paths, wrong ownership/modes, nonregular DB/sidecars and
unrecognized application/schema IDs. No supported online external SQLite
connection; inspect a stopped, consistent copy. Open version 1 only, never
downgrade or automatically migrate an unknown spool. Future versions need an
explicit transactional migration; a failed one must leave old data usable.

Before READY, SQLite performs normal WAL recovery, quick_check, foreign_key_check
and a streaming scan of <=4096 batches/64 MiB validating schema, JSON/origins,
allocator/order ranges and usage sums. Never silently repair or reset counters.
Startup target is 25 s, with a progress-handler CPU deadline; planned systemd
TimeoutStartSec=30s terminates a stalled start without false readiness. A full
but valid spool may start degraded and replay. An invalid/unwritable/locked or
unrecoverable spool fails startup, exit 2. No implicit --no-persistence fallback.

After READY, FULL/BUSY/READONLY/NOMEM pause local admission with paced 1 s
retry and keep at most one held candidate. Corruption/NOTADB, impossible
constraints, malformed committed payload or allocator inconsistency latch a
local fault, disable replay and require offline operator investigation; monitoring
continues degraded. IOERR or an uncertain COMMIT requires close/reopen/validation
before more admission or replay. An in-process reopen retains its current run;
only process startup allocates a new origin. On any failed write/COMMIT, inspect
sqlite3_get_autocommit and roll back an active transaction; if rollback/absence
cannot be established, use IO_UNKNOWN reopening rather than retrying in an
unknown transaction. Never assume a COMMIT error proves absence.
If the candidate's exact ID+bytes survived, a successful FULL recovery/start
transaction and directory fsync seal that evidence before reporting durable;
if absent and allocators unchanged, retry the same candidate. Mismatch is a
latched fault. For retirement, row absence plus the retired prefix resolves
the uncertain outcome. Retryable recovery is at most once per second.
An in-process recovery transaction must actually increment the checked
`recovery_generation` (initially zero), forcing a WAL write/sync; a read or
optimized no-op COMMIT is insufficient. Startup's new-origin write also forces
this barrier. Counter exhaustion is handled like the other durable allocators.

Do not overwrite corrupt files, delete WAL/SHM, auto-salvage into a fresh spool,
or silently skip records. Offline recovery preserves the complete DB/WAL set;
resetting/discarding history needs a separate explicit owner decision. An I/O
or corruption failure may invalidate the assumed storage guarantees; reporting
a fault cannot reconstruct destroyed bytes.

Planned service settings: `StateDirectory=vigil`, `StateDirectoryMode=0700`,
`UMask=0077`, DB `/var/lib/vigil/outbox.sqlite3`, sibling WAL/SHM/lock 0600.
Retain strict filesystem protections and unprivileged vigil ownership; systemd
provides this writable directory without opening the whole filesystem.
`--outbox-path ABSOLUTE` selects an explicitly provisioned test/foreground store;
default is the service path. `--no-persistence` creates no store/workers and
remains a clearly volatile diagnostic mode. No new legacy volatile-PG mode.
See [systemd 255 state directories](https://www.freedesktop.org/software/systemd/man/255/systemd.exec.html).

On SIGTERM/INT main stops CAN observation, publishes final snapshot, then closes
ingestion. Give storage/PG a shared **one-second** best-effort drain deadline.
Storage prioritizes local admission over optional remote drain and continues
processing results; PG starts no new network work after the deadline and polls
shutdown at <=50 ms. Stop PG, then finish available local result bookkeeping,
close SQLite/checkpoint best effort, join storage, destroy shared objects last.
All mailbox/condition waits observe the stop flag/deadline; neither worker can
wait indefinitely for the other to consume a slot during shutdown.
Ordinary completion target <=1.25 s; retain TimeoutStopSec=3s as the supervisor's
force-stop bound. Never cancel a thread mid-SQLite call or free objects while
it runs. Kernel disk stalls/fsync and local libpq file stalls are not bounded
by these application deadlines; systemd may SIGKILL. Existing durable work
remains recoverable, while final volatile work can be lost. A deferred durable
backlog is not an abandoned/lost volatile record.

## PostgreSQL migration 002 and replay algorithm

**002 is necessary.** Migration 001 returns early for an existing checkpoint
and ignores history conflicts; neither verifies payload or originating-run
content. Preserve 001 byte for byte, its function and all historical tables.
Future `002_resilience.sql`, applied by the owner, adds the objects below.
All new PG columns are NOT NULL, with signed ranges and UUID/token/length
constraints matching the local format; keys/FKs are explicit.

- Widen `schema_version`'s CHECK to versions 1 or 2, preserve row 1, insert row
  2 in the same migration transaction. Existing v1-only fixtures remain valid
  against 001 alone; the v2 capability probe requires version 2 and its function.
- `vigil.run_origins`: run_id UUID PK/FK to runs; spool_id UUID, run_no bigint
  positive, run_token bytea length 32, boot_id UUID, original UTC/monotonic start
  milliseconds bigint nonnegative; UNIQUE(spool_id,run_no), UNIQUE(run_token).
- `vigil.replay_receipts`: `(run_id,batch_id)` PK and FK to checkpoints;
  `payload bytea` NOT NULL length 1..160000, `record_count smallint` 0..32.
  Keep exact bytes, not a hash-only content promise; no pgcrypto dependency.
- Invoker function `vigil.replay_batch(bytea)`, fixed pg_catalog search_path,
  fully qualified tables, returning one `(run_id,batch_id,record_count,disposition)`
  row, disposition `inserted` or `equal`. Runtime receives SELECT/INSERT and
  EXECUTE only for new objects, no DDL/UPDATE/DELETE/security-definer elevation.
  Revoke PUBLIC EXECUTE on the new function; grant the provisioned runtime role.

Call the new function with one bounded binary bytea parameter, in a single
autocommit statement. Algorithm inside that transaction:

1. Validate exact format/ranges/shape, unique consecutive record identities,
   origin consistency and payload length before writes. Parse once; no SQL
   built from JSON. Invalid payload raises `VG002`.
2. Take transaction-scoped `pg_advisory_xact_lock(hashtextextended(rid::text,0))`
   before reading/inserting that run. All v2 callers use this run lock; hash
   collisions cause extra serialization, never identity equivalence. This
   needs no table UPDATE privilege. Insert the runs row if absent; verify all
   original run timestamps and origin fields; attach origin only to a runs row newly inserted by this
   transaction. An existing legacy run without origin is an identity conflict.
   Concurrent same-origin callers see and verify the winner after the lock,
   using READ COMMITTED isolation (other isolation failures retry unchanged).
3. Existing receipt: require byte-for-byte equal payload and count; return
   `equal`. Unequal payload raises `VG001`, never a successful no-op. An existing
   checkpoint without a v2 receipt is also `VG001`, not adopted as evidence.
4. For each history key, INSERT ON CONFLICT DO NOTHING, then compare **all**
   persisted columns with the expected v1 projection, using null-safe equality
   and JSONB semantic equality for detail. Existing equal content is allowed;
   unequal content raises `VG001`. Checkpoint insertion and receipt insertion
   are in this transaction too. Run-origin uniqueness conflicts are `VG001`.
5. Return `inserted` only with all inputs/checkpoint/receipt present and equal.
   Any constraint, poison record or failure rolls back the entire statement.
   A batch is never partially acknowledged, split, renamed or rewritten.

Concurrent v2 calls must pass equal/conflicting batch and event-key tests. V1
history stays readable and v1 ingestion works for unrelated legacy runs.
Direct runtime SQL tampering, concurrently using v1 for a v2 origin, destructive
admin edits and rolling PG back behind acknowledged history are outside contract.
Receipts/origins have the history tables' existing no-auto-retention policy;
deleting them independently invalidates replay evidence.

On connect, probe schema/function and require fsync=on, full_page_writes=on;
set synchronous_commit=on in the session. Keep numeric/Unix endpoints, 2 s
connect/query deadlines, 1000/500 ms statement/lock timeouts and <=50 ms poll
slices. Drain results through ReadyForQuery/idle transaction state and validate
the one returned identity/count before sending success to storage. Server
settings or capability failures leave local admission working and replay degraded;
there is no fallback to `write_batch`.

Replay the oldest order only, one batch at a time. A failed submitted batch
keeps the same bytes until success or explicit operator repair. Retry transient
transport, timeout, lock/deadlock/serialization failures after 1,2,4,8,16,30 s,
capped at 30 s, measured from failure with a monotonic clock; no jitter is
needed for this single daemon. Enqueues cannot shorten backoff. Reset after
confirmed success. Config/auth/schema failures retry/probe every 30 s. `VG001`,
`VG002` and nontransient data/check violations latch a poison-head replay fault;
no automatic skip. Failure-result consumption releases the mailbox for the
same head without changing its PG-owned retry deadline; poison suppresses
redispatch until explicit restart after operator repair. New local records may
commit until bounded capacity fills.
Operator repairs schema/grants or restores authoritative evidence offline;
immutable data cannot be casually edited to bypass an identity mismatch.

No persisted 'sent' bit is needed. After any restart every remaining row is
pending/possibly committed remotely; resubmit head, verify equal receipt and
retire. This handles repeated SIGKILL after PG commit and before local ack.
FIFO gives each admitted new batch finitely many predecessors; eventual delivery
requires a healthy head, available storage/PG and sufficient service capacity.
Do not claim bounded delivery delay under overload or progress past poison.
Independent storage scheduling prevents replay starvation of new admissions;
strict FIFO prevents new traffic starving recovered history.

The transaction/conflict behavior is grounded in
[PostgreSQL 16 INSERT](https://www.postgresql.org/docs/16/sql-insert.html),
[transaction advisory locks](https://www.postgresql.org/docs/16/explicit-locking.html#ADVISORY-LOCKS),
[libpq asynchronous processing](https://www.postgresql.org/docs/16/libpq-async.html)
and [WAL durability settings](https://www.postgresql.org/docs/16/runtime-config-wal.html).
Network transmission is at least once under retry, never exactly once.

## Exact implementation seams for a later authorized stage

| File/interface | Planned responsibility |
| --- | --- |
| `include/vigil/queue.h`, `src/queue.c` | `vigil_queue_push` returns QUEUED / QUEUED_EVICTED / CLOSED / RESOURCE_REJECTED with volatile ticket; `vigil_queue_take_local` copies records+stats+snapshot atomically; `vigil_queue_local_committed` publishes held-to-durable transfer instead of current remote queue ack. Preserve two lanes and existing drop-oldest unit semantics. |
| New `include/vigil/outbox.h`, `src/outbox.c` | Owner-only `vigil_outbox_open_validate`, `vigil_outbox_begin_run`, `vigil_outbox_append`, `vigil_outbox_peek_head`, `vigil_outbox_retire`, `vigil_outbox_checkpoint`, `vigil_outbox_close`; typed OK/FULL/BUSY/IO_UNKNOWN/CORRUPT/INVALID/NOMEM/EXHAUSTED. Append takes a copied capture, allocates IDs/serializes/commits and returns durable IDs/counts; peek copies bytes, retire takes the confirmed head identity. No PG dependency; schema SQL is private. |
| `include/vigil/persistence.h`, `src/persistence.c` | Lifecycle coordinator + SQLite worker + bounded status/mailboxes; start waits local-ready, stop drives common deadline and joins both workers. Replace in-memory-only run/batch allocator. |
| New `include/vigil/replay.h`, `src/replay.c` | PG owner; move current connect/socket/query machinery here, retain deadline restrictions, add bytea replay, capability check, fixed result and backoff classifier. No SQLite or monitor API. |
| `src/vigild.c` | CLI outbox path; initial-snapshot latch; distinguish volatile/local/remote status; main-owned degraded policy; shutdown handoff. No change to decoder, sensor policies, protocol or simulator. |
| Future `migrations/002_resilience.sql`, `scripts/setup-database`, new `scripts/setup-outbox` | Additive PG objects/version/grants; explicit service-user local bootstrap. No automatic production migration. |
| `CMakeLists.txt`, `scripts/setup-linux`, `systemd/vigild.service` | SQLite version/capability/build dependency, test-only target, state directory and start timeout. No edits to these files in Stage 0B. |
| New `tests/test_outbox.c`, `tests/resilience_integration.py`, `tests/resilience_benchmark.py`, `tests/resilience_database_contract.py`, `scripts/check-resilience` | Contract matrix T01–T20, real Linux/SQLite/PG fixtures and comparable measured load. Extend evidence digest/verifier to cover new files without changing original gate meaning. |

Stage 1's narrow implementation slice is the outbox module, local unit/fault
tests and build dependency only; preserve the original daemon until a later
explicitly authorized integration checkpoint. All wire, API/dashboard, hardware,
Buildroot, authentication, remote retention and live-state restoration remain
outside this resilience design.

## Stage 1 scope amendment

The owner's Stage 1 request explicitly broadened the earlier module-only slice
to daemon CLI/status, initial snapshot, shutdown, service configuration and real
process-kill tests. This supersedes the earlier narrow slice and stop wording.
The SQLite owner is the only persistence worker in this stage. PG work slots,
replay, receipts, migration 002 and retirement remain unimplemented. No v1 send
fallback is retained. This preserves original live monitoring while every local
batch remains pending and persistence-degraded stays raised for deferred remote
history. The schema includes retirement columns at zero and rejects nonzero
retirement on Stage 1 open; a future compatible implementation must add verified
retirement before using them. An additional immutable-origin trigger and exact
schema validation strengthen the specified identity constraints. Current code
and test entrypoints are documented in OUTBOX.md; historical release evidence
and original PG scenarios are retained separately.
