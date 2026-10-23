# Stage 1 local durable outbox

Stage 1 replaces the volatile PostgreSQL writer with a dedicated SQLite owner.
The CAN poll loop, live alarms, freshness, progress, plausibility, decoder and
wire protocol remain unchanged. Main copies bounded records and snapshots;
it never opens SQLite, syncs storage or waits for a commit. Every SQL/fsync
boundary checks the owner and that the caller holds no queue mutex.

**Remote replay is deferred.** No PostgreSQL transmission, receipt verification,
retirement, deletion, migration 002 or remote health probe is implemented here.
The legacy `records_written/readings_written` counters remain zero. Durable
backlog stays local, and persistence-degraded remains raised because remote
history is unavailable. Existing database environment variables are validated
for CLI compatibility but no PG connection is opened. Migration 001 and its
five standalone database contracts are retained unchanged.

## Provision and operate

Use real Linux ext4/XFS, including the Lima VM's native filesystem. The macOS
repository mount, tmpfs and network filesystems are rejected. Build dependencies
use SQLite >=3.51.3 with JSON, STRICT and thread safety; CMake checks the library
version and the owner checks runtime capabilities/settings. `scripts/setup-linux`
calls `scripts/setup-sqlite`, which verifies the upstream sqlite3.c SHA3-256 and
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
Freed pages are not currently relevant because Stage 1 never retires records.
Storage assumptions include working SQLite/kernel/fsync/directory sync and
media honoring flushes. Tests prove process-crash recovery, not power-cut or
media-failure survival. Kernel I/O stalls can exceed the application drain
budget; systemd then supplies its force-stop deadline.

## Status and shutdown accounting

`persistence` retains volatile lane/held/drop fields. A separate `outbox` line
exposes local_state, remote_state=deferred, current-run local_committed and
local_readings, durable_pending, pending_batches/bytes, spool_admitted/retired,
local_failures, extended sqlite_error, reading/event evicted/rejected,
local_revision and count_exact. Error output contains codes, not SQL text or
credentials. Status may lag committed DB/WAL evidence and diagnostics may drop.

For each lane, queued = evicted + pending + held + locally_committed. Resource
rejection is separate from successfully queued inputs; dropped includes eviction
and resource rejection. Closed publication is a distinct result. Current local
commits equal current durable pending; spool_admitted equals spool_pending,
since retirement remains zero. Metadata-only batches consume batch/byte capacity,
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

The original release verifier, PG writer scenarios and demo remain historical
fixtures for the archived 1.0 implementation. Stage 1 replaces their PG outage
assumptions with local-capacity/crash assertions; it does not claim remote
backfill, receipt verification or retirement works. The current gate preserves
original unit, Python, vcan, monitoring, finite load, manual, fuzz and v1 SQL
contracts, plus local fault/recovery/supervision tests. Stage 2 T06–T09/T11/T16
and remote parts of other T01–T20 cases remain deferred. Hardware/power-cut
qualification and the remote 60-second benchmark remain unclaimed.
