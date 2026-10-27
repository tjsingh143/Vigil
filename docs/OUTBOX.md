# Durable outbox and Stage 2 PostgreSQL replay

Stage 1 replaces the volatile PostgreSQL writer with a dedicated SQLite owner.
The CAN poll loop, live alarms, freshness, progress, plausibility, decoder and
wire protocol remain unchanged. Main copies bounded records and snapshots;
it never opens SQLite, syncs storage or waits for a commit. Every SQL/fsync
boundary checks the owner and that the caller holds no queue mutex.

A separate PostgreSQL owner now replays SQLite batches in increasing spool
order, including prior runs. It uses the original asynchronous libpq connection,
flush and result machinery with two-second deadlines and <=50 ms socket polls.
Neither SQLite nor network I/O runs under queue/mailbox locks. The storage
worker handles one result, one admission and one dispatch each turn; a remote
retry never blocks new local admission. One held admission, one shared replay
payload and one PG copy bound application work memory independently of backlog.

Migration [002_resilience.sql](../migrations/002_resilience.sql) adds origin
verification and exact byte receipts. The invoker `replay_batch(bytea)` validates
JSON (including duplicate keys before JSONB conversion), takes a transaction run
lock, verifies the original origin, and reuses unchanged `write_batch(jsonb)`.
It compares every history column after that function and inserts a receipt in
the same transaction. Equal receipts return `equal`; mismatched origins, receipt
bytes, history or unverified legacy checkpoints raise VG001. Malformed content
raises VG002. All effects roll back on failure. V1-only contracts still test 001
alone; v1 ingestion of unrelated legacy runs remains compatible.

The owner applies 001 then 002 explicitly, or runs `scripts/setup-database` only
when deployment is authorized. New objects require SELECT/INSERT and EXECUTE on
`validate_replay(bytea)` and `replay_batch(bytea)`; no runtime UPDATE/DELETE/DDL.
PUBLIC has no new-function EXECUTE. No daemon startup applies migrations. Session
synchronous_commit is on; capability probes require schema 2, fsync and
full_page_writes on. There is no v1 fallback.

After complete validated success through ReadyForQuery/idle, the storage owner
accepts only the matching oldest order/run/batch/count and payload. One FULL
transaction deletes that batch, advances the retired prefix, decrements usage,
advances that origin's retired counters/loss/revision and prunes empty old
origins. Directory sync precedes publishing acknowledgement/releasing the slot.
Death or local I/O uncertainty before this boundary retains a safe retry or
resolves the committed prefix through validation and a FULL recovery barrier.
Restart never rebuilds or renames old batches. A committed retirement never
resurrects its rows. SQLite schema 1 remains compatible with Stage 1 pending
stores; Stage 1 binaries deliberately refuse stores with nonzero retirements.

Transport errors after submission are UNKNOWN, even if PG may have committed.
Retries carry identical bytes and identity, with 1/2/4/8/16/30-second delays capped
at 30, measured after failure. Enqueues cannot shorten them. Configuration,
capability and permission failures retry every 30 seconds. Since libpq exposes
no SQLSTATE for startup ErrorResponse, a connection rejected/stalled after the
server handshake uses 08004 and a conservative 30-second delay; failure before
handshake uses transport backoff. Poison VG001/VG002 and data/constraint failures
latch replay until explicit restart after operator investigation. They never
skip a head, split a batch or evict durable data. Local admission continues to
its fixed limits. Idle liveness probes create no empty history batches.

## Provision and operate

Use real Linux ext4/XFS, including the Lima VM's native filesystem. The macOS
repository mount, tmpfs and network filesystems are rejected. Build dependencies
use SQLite >=3.51.3 with JSON, STRICT and thread safety; CMake checks the library
version and the owner checks runtime capabilities/settings. `scripts/setup-linux`
calls `scripts/setup-sqlite`, which refuses an optimized Python interpreter before
any download, explicitly verifies the upstream sqlite3.c SHA3-256 (a mismatch
stops before configure/build/install) and
installs 3.51.3 under `/opt/vigil-sqlite-3.51.3`, preserving distribution SQLite.
CMake uses that prefix when available. The library includes the upstream
[WAL-reset fix](https://www.sqlite.org/releaselog/3_51_3.html).

```sh
./scripts/dev ./scripts/setup-sqlite
./scripts/dev cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
./scripts/dev cmake --build build --parallel 2
# Foreground disposable store, owned by the invoking Linux user:
./scripts/dev ./scripts/setup-outbox /tmp/my-vigil-state/outbox.sqlite3
./scripts/dev build/vigild --outbox-path /tmp/my-vigil-state/outbox.sqlite3
```

For an explicitly authorized service installation, run `scripts/install-service`,
then `scripts/setup-outbox` with no argument to provision the vigil user's
`/var/lib/vigil/outbox.sqlite3`. Start/enable remains explicit. The unit retains
User/Group=vigil, no capabilities, strict filesystem protections and a main-loop
watchdog. `StateDirectory=vigil`, mode 0700, grants only its state directory;
startup timeout is 30 s and stop timeout 3 s. This checkpoint does not install or
start the developer's default service.

Initialization accepts only an empty private directory with >=2 GiB available.
Directory mode 0700, DB/WAL/SHM/lock mode 0600, current ownership and regular
single-link files are required. Symlink components, duplicate openers, missing
DB, oversized files, wrong versions and corrupt/inconsistent stores fail before
READY (exit 2). The daemon never initializes a missing database. Supported
openers take `outbox.lock`; never open a live spool with an external SQL client.
Stop the daemon and preserve DB/WAL/SHM together before offline inspection.
Do not delete sidecars, reset history, salvage automatically or run concurrent
clones. A failed initialization leaves evidence for explicit investigation.

`--interface`, `--config`, `--queue-capacity`, signals and `--help` remain.
`--outbox-path ABSOLUTE` selects an already provisioned store.
`--no-persistence` starts no worker and creates no storage. Combining it with
an outbox path fails as inconsistent configuration. All monitoring-only
regressions still use that explicit mode.

## Transactions, identity and bounds

The schema is private in `src/outbox.c`: application_id=0x5649474c, user_version=1,
STRICT `spool_meta`, `origin_runs`, `outbox_batches`, foreign keys, immutable
origin fields and an immutable-batch update trigger. Startup compares exact
schema definitions, quick_check, foreign_key_check, bounded streaming JSON
validation, contiguous order/sequence/batch allocators, usage and captured-loss
sums. No backlog-sized application allocation is made. Progress-handler startup
elapsed deadline 25 s does not bound kernel I/O stalls.

Each process commits a fresh getrandom UUIDv4, 256-bit public origin token,
Linux boot UUID, UTC/monotonic starts and transactional run number. Empty old
origins are pruned; origins with history remain. Each admission atomically
allocates spool-global order, per-run batch and contiguous history sequences,
freezes format 2 JSON bytes, updates logical usage and captures loss/revision
cursors. Volatile queue tickets are distinct from durable sequences. IDs never
wrap; the signed bigint sentinel stops admission. UTC is bounded through 9999.
Reopening preserves original run/boot/timestamps, payload bytes and identities;
old records never initialize live sensor or alarm state.

Queue publication and a held candidate are **volatile**. Only successful WAL
FULL commit **and parent-directory fsync**, or a validated recovery barrier,
establish local durability. On uncertain I/O, the owner closes/reopens,
validates, compares the exact pinned candidate and allocators, increments
`recovery_generation` in a FULL transaction and syncs the directory before
publishing success. A failed sync can leave a whole recoverable transaction;
an error is never a durability acknowledgement. Rollback allocates no visible
identity. All SQLite return codes, binds, finalizes, checkpoints and syncs are
checked. A rollback that cannot establish autocommit becomes uncertain I/O.

If confirmed replay A overlaps a committed-but-unpublished append B, retirement
returns `APPEND_PENDING` before changing A. The storage owner reconciles and
publishes the same held B, then retries A. B's successful reconciliation clears
its pending marker, so this dependency cannot repeat after storage recovers.
Ordinary retirement errors still freeze new mutations. Successful retirement,
including an already-retired-prefix retry, requires a complete usage reload;
NOMEM during prepare/step remains a retryable failure and cannot publish stale
counts. Usage is replaced atomically only after the full query succeeds. See the
[remediation progress argument and runtime evidence](RESILIENCE_REMEDIATION.md).

| Resource | Bound/policy |
| --- | --- |
| Volatile readings | N, default 1024, range 1..65536; oldest pending eviction counted |
| Volatile events | Independent 256-slot reserve; oldest pending eviction counted |
| Held admission | One immutable candidate, at most 32 records, copied capture/snapshot |
| Durable records/batches/payload | 65536 records, 4096 batches, 64 MiB; first exhausted resource stops new admission |
| Event priority | Reserve is in RAM only; events share durable capacity; no infinite event retention |
| SQLite | 32 MiB hard heap, 2 MiB cache target, mmap=0, temp memory, 192 KiB value limit, 64 KiB SQL limit |
| DB | 4096-byte pages; max_page_count=131072, 512 MiB |
| WAL | 4 MiB truncate trigger before next mutation; conservative file bound <520 MiB; journal_size_limit is not a quota |
| SHM/managed files | SHM <=2 MiB, DB+WAL+SHM/lock <1056 MiB; filesystem journals/allocation overhead excluded |

No durable older record is evicted, expired or overwritten at capacity. One
held candidate retries each second; bounded lane evictions can continue behind
it. FULL/BUSY/READONLY/NOMEM pause admission. I/O uncertainty reopens before
more admission. Corruption, bad committed content, impossible schema/allocator
state or exhaustion latches a fault; main keeps monitoring degraded. No volatile
fallback or destructive repair exists. Invalid internal records latch a fault.

Settings are set/read back: WAL, FULL, foreign_keys=ON, page_size=4096,
max_page_count=131072, wal_autocheckpoint=0, journal_size_limit=4194304,
cache_size=-2048, cache_spill=OFF, mmap_size=0, temp_store=MEMORY, busy_timeout=0.
Idle housekeeping runs PASSIVE each second. A failed checkpoint freezes further
mutations; TRUNCATE must succeed before resuming. Close attempts TRUNCATE.
Freed pages are reused after retirement; DB file shrinkage is not promised.
Storage assumptions include working SQLite/kernel/fsync/directory sync and
media honoring flushes. Tests prove process-crash recovery, not power-cut or
media-failure survival. Kernel I/O stalls can exceed the application drain
budget; systemd then supplies its force-stop deadline.

## Status and shutdown accounting

`persistence` retains volatile lane/held/drop fields. A separate `outbox` line
exposes local_state, remote_state=disconnected/pending/unknown/confirmed/poison, current-run local_committed and
local_readings, durable_pending, pending_batches/bytes, spool_admitted/retired,
local_failures, extended sqlite_error, reading/event evicted/rejected,
local_revision, oldest_order, replay_attempts and count_exact. The persistence
line's records_written/readings_written and acknowledged revision/loss cursors
belong to the current origin; old-run retirement changes spool totals only. Error output contains codes, not SQL text or
credentials. Status may lag committed DB/WAL evidence and diagnostics may drop.

For each lane, queued = evicted + pending + held + locally_committed. Resource
rejection is separate from successfully queued inputs; dropped includes eviction
and resource rejection. Closed publication is a distinct result. Current local commits equal current durable pending plus current-run retired
records. Spool admitted equals durable pending plus lifetime spool retired. Metadata-only batches consume batch/byte capacity,
not history-record counts. Copied losses/revisions commit once and later
changes wait for a newer immutable batch. An initial complete snapshot gates
admission. Snapshot history need not describe every intermediate transition.

Shutdown closes ingestion and prioritizes local admission for a shared one-second
budget. Main joins the owner before freeing shared state. Abandoned counters
include only remaining volatile lane/held inputs; durable backlog is deferred
and survives restart. New process means new origin, not restored live state.

## Verification

`./scripts/dev ./scripts/check-resilience --clean --output artifacts/NEW-RUN`
(or `scripts/verify` with the same options) runs the current checkpoint gate.
Exact commands, expectations, exits, source/binary digests and measurements are
saved in gate.json/logs; native SQLite inventories retain DB/WAL/SHM and full
payloads under the reported Linux evidence path. The test dispatcher/VFS and
its environment controls exist only in test targets, never the installed binary.

The original release PG writer scenarios and `tests/interview_demo.py` remain historical
fixtures for the archived 1.0 implementation. Stage 1 replaces their PG outage
assumptions with local-capacity/crash assertions; the new Stage 2 fixtures assert durable replay, receipt verification and retirement. The current gate preserves
original unit, Python, vcan, monitoring, finite load, manual, fuzz and v1 SQL
contracts, plus local fault/recovery/supervision tests. Stage 2 adds real PG rollback/ambiguity/retirement/crash/FIFO/poison/deadline
and concurrency contracts in `tests/resilience_replay.py` and
`tests/resilience_database_contract.py`. Hardware/power-cut
qualification remains separate; the Stage 4 measured benchmark is in
[RESILIENCE_PERFORMANCE.md](RESILIENCE_PERFORMANCE.md).

## Replay lifecycle and accounting

Current-run `locally_committed = current durable pending + records_written`.
Spool `admitted_records = pending_records + retired_records`. Reading equations
use corresponding reading counts. `queued = evicted + pending RAM + held +
locally_committed`; resource rejections and closed publications remain separate.
An unknown remote outcome is part of durable pending, not another input.
Metadata-only batches count against batch/byte limits, not history rows.

Main raises degraded on remote/local failure, poison, unknown or newly observed
loss. It clears only when local/remote health, all backlog and the current run's
loss/revision have caught up. Old-run acknowledgements cannot advance the new
run's cursors or restore sensor state. Recovery captures current health/snapshot
in a coalesced new batch; it never edits the retry. Completed outages and known
gaps are historical evidence, not a reconstruction of unsaved crash losses.

Shutdown gives both owners the existing shared one-second budget. PG stops
network work by that deadline; storage prioritizes local admission and handles
available results. Both workers join before their queue/mailbox is destroyed.
An unprocessed success leaves the durable row for safe restart verification.
Kernel disk stalls and local credential-file reads remain outside application
bounds; systemd retains its three-second force-stop and main-only watchdog.
Durable backlog is deferred; only queued/held RAM counts as abandoned.

The PG scheduler polls its bounded mailbox with <=10 ms monotonic sleeps;
producer condition signals belong to the storage waiter. Active network I/O
retains <=50 ms deadline/shutdown polls. Test Helgrind uses a narrow suppression
for libp11-kit ELF-finalizer mutex diagnostics and glibc internal replacement
wakes from timed waits; no Vigil race, lock-order or direct mutex/signal call
is suppressed. System-library internals are outside sanitizer proof.

Current production-binary demonstration: `scripts/demo --build build-resilience-release`.
Setup, upgrade/rollback and independent-review handoff: [RESILIENCE_RELEASE.md](RESILIENCE_RELEASE.md).
