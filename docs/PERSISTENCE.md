# Persistence and service contract (Checkpoint 3)

## Owners and memory bounds

The main thread owns monitoring, current alarms, CAN, timerfd, signalfd and
status output. The writer thread exclusively owns `PGconn` and all libpq calls.
No monitoring pointer is given to the writer. Shared state consists of copied
records, a copied alarm snapshot, counters, stop/deadline flags, and a condition
variable; one mutex protects all of it. A writer copies/removes at most 32
records under the mutex and releases it before formatting JSON, polling a
socket, connecting, or issuing SQL. Queue publication performs no I/O.

The queue has two bounded FIFO lanes. `--queue-capacity N` sets **pending
accepted readings** (default 1024, range 1..65536). Events have 256 reserved
slots for heartbeat, rejected-frame evidence and alarm transitions. When a
lane is full, its oldest pending record is evicted and its exact drop counter
increases. Events cannot consume reading slots. If an outage produces more
than 256 events, the oldest event is also lost; current alarms remain intact.
The two lanes are merged by enqueue sequence when the writer takes a batch.

There is also **one immutable batch of at most 32 records in flight**, shared
between both lanes. It stays in the writer until PostgreSQL confirms it.
Its records have already been submitted or may have committed, so evicting or
changing them could falsely count persisted data as lost. New arrivals evict
only pending records. This explicit refinement of the brief's queue-capacity
wording bounds outstanding history by **N + 256 + 32 records**, plus one
193-entry maximum alarm snapshot and fixed serialization buffers. At a settled
outage with no readings in flight, M new readings retain exactly min(M,N),
and drop max(0,M-N). Pending capacity and in-flight counts are separate in status.
No claim is made that all these bytes are durable.

## Records and reproducible schema

Apply `migrations/001_history.sql` using `psql -v ON_ERROR_STOP=1`. It is a
transactional, repeatable schema initialization with version 1 recorded in
`vigil.schema_version`. `scripts/setup-database` provisions the default local
DB and minimal daemon role; schema ownership remains with the administrator.
The runtime role has SELECT/INSERT, schema USAGE and function EXECUTE; no
schema creation, ALTER, UPDATE or DELETE permission is needed. The function
runs with invoker privileges. Deployment changes require future numbered
migrations, not rewriting a deployed version.

| Table | Meaning |
| --- | --- |
| `runs` | Random UUID per process, UTC start and Linux monotonic start; a restart never resumes the old memory state |
| `history` | `(run_id,seq)` primary key, UTC observation, monotonic ms, kind, protocol pod ID/channel and JSON detail |
| `checkpoints` | `(run_id,batch_id)` primary key, observation times, flush/recovery/gap kind, exact newly acknowledged reading/event drops, cumulative health, optional complete alarm snapshot |

`history.kind` is reading, heartbeat, rejected or alarm. Reading/heartbeat
fields include raw CAN ID, decoded/accepted/valid flags, source staleness at
handling time, value in wire units, counter, status, pod state and uptime.
`valid` describes acceptance of that record; a rejected repeat does not
invalidate a previously usable value in monitoring memory. For undecodable
frames, pod=0/channel=-1 and numeric payload fields are placeholders; do not
interpret them as measurements. `reason` gives the rejection cause. CRC and
counters never authenticate the stored protocol identity.

Alarm detail has kind, active boolean, severity, raised/cleared monotonic ms
and transition count. Channel -1 is heartbeat, -2 is the shared reading stream;
pod=0/channel=-3 is the daemon's persistence-degraded alarm. Communication
transitions record stale/recovery evidence without repeatedly storing old
values as new readings. SQL `alarms` is a complete list including cleared
alarms, captured by the monitoring thread; individual observation times are
inside each entry. Latest non-null snapshot by batch ID is history **as of
that observation**, never an API for live alarm truth. Runs must be considered
separately. There is no analytics, retention scheduler or dashboard here.

The `write_batch(jsonb)` function inserts the run, records, and checkpoint in
one atomic PostgreSQL statement/transaction. A checkpoint also acts as an
idempotency ledger: retrying an already committed `(run_id,batch_id)` does
nothing. The C writer retains exactly the same payload/ID on any failure,
including a lost COMMIT response. A failed batch is never half-acknowledged.
`records_written` counts confirmed history rows, not checkpoint rows. It may
lag actual PostgreSQL commits if a response was lost; retry resolves that
ambiguity while the process remains alive.

## Outage, recovery and shutdown

A failed connection or statement marks the writer disconnected and starts a
monotonic outage interval. Even an idle daemon probes history once per second.
Failures retry after one second; enqueue/condition-variable wakeups cannot
shorten the failure backoff. Monitoring continues to decode frames and tick
freshness deadlines. Main observes failure/drop counters on every loop/frame
and maintains a warning-level `persistence_degraded` alarm. Newly observed loss raises it even
if the writer has already recorded the gap before main sees the counters. It also raises
while the initial connection is not yet established. This is deliberately
stronger than raising only on overflow. It remains active until connection,
queue catch-up, recorded gaps and the published alarm revision are acknowledged.

After reconnection, any ambiguous immutable batch is retried first. Available
history is backfilled by sequence. Exact losses since the last acknowledged
drop totals produce a `gap` checkpoint; cumulative first/last lost observation
monotonic times bound the lost history. Several gaps can cover a continuing
overflow. Recovery/gap checkpoints carry complete current alarm snapshots.
If the state changed while an older retry was outstanding, the newer revision
is written in the next batch. Main clears degraded after catch-up, and queues
that clear transition and new snapshot too. The completed outage duration is
reported independently of how long draining the backlog takes.

SIGINT/SIGTERM stops monitoring, publishes its final alarm state, and requests
a **one-second drain budget**. The writer wakes, drains if reachable, otherwise
exits by that budget plus at most a 50 ms poll slice and scheduling delay.
Main joins it before destroying the queue/condition variable/mutex. Final
status counts `abandoned_readings` and `abandoned_events`; these are pending
plus unconfirmed in-flight records, not additional overflow drops. A reachable
but large/slow backlog can exceed the drain budget and be abandoned too.

SIGKILL/crash loses the queue, unconfirmed batch, drop counters and current
alarm memory. Committed PostgreSQL rows survive subject to PostgreSQL's own
durability guarantees. If death occurs after COMMIT but before confirmation,
some unconfirmed rows may already be present: surviving SQL is the evidence.
A new run cannot reconstruct exact lost counts/times from vanished memory and
does **not** invent a gap marker or backfill for that prior run. The crash gate
kills during a confirmed stopped-DB interval with no readings in flight, so
its three unpersisted readings are unambiguously lost. No spool is implemented.

## Timeouts and configuration

The brief proposed blocking libpq with `connect_timeout` and server-side
`statement_timeout`. The dedicated writer instead uses libpq's asynchronous
API with its own poll/deadline loop. This is a documented refinement for
bounded shutdown when a server or network stops responding: server timeouts
alone cannot bound a response that never reaches the client. Connection
parameters still include `connect_timeout=2`; because `PQconnectPoll` ignores
that setting, the writer explicitly enforces its two-second intent. Each
query/flush has a two-second client deadline, `statement_timeout=1000` ms,
and `lock_timeout=500` ms. Every network wait rechecks shutdown at most every
50 ms. DNS is excluded by requiring a numeric IP or Unix socket path.

| Environment variable | Default |
| --- | --- |
| `VIGIL_DB_HOST` | `/var/run/postgresql` (directory) |
| `VIGIL_DB_PORT` | `5432` |
| `VIGIL_DB_NAME` | `vigil` |
| `VIGIL_DB_USER` | `vigil` |

Values are copied before thread creation; no environment mutation/reload.
`PGSERVICE` and `PGHOSTADDR` indirection are rejected to preserve endpoint
bounds. Other standard libpq authentication/SSL settings remain available;
use `PGPASSFILE` (0600, readable by vigil) for TCP credentials. The supplied
local installation uses Unix peer authentication, no password. Server notices are discarded to avoid libpq's default blocking stderr sink.
Query failures expose only SQLSTATE (or transport 08001/08006), never connection strings or
credentials. Permanent schema/auth errors remain degraded with paced retry.
`--no-persistence` is explicit diagnostic/test mode; Checkpoint 1/2 regression
uses it so unrelated database state cannot alter their original contracts.

Sources: [libpq connection control](https://www.postgresql.org/docs/16/libpq-connect.html),
[asynchronous processing](https://www.postgresql.org/docs/16/libpq-async.html),
[server timeouts](https://www.postgresql.org/docs/16/runtime-config-client.html).

## Counters and inspection

A separate `persistence key=value ...` line follows each normal status
snapshot and appears on shutdown. It has run ID, connection/degraded state,
transition counts, accepted readings/events enqueued, confirmed history rows
and readings written, per-lane drops/depths/high-water marks/capacities,
in-flight counts, connection/query failures, connections/reconnects, outage
count/current/total/last duration, gap/snapshot counts, published/acknowledged alarm revisions, last error and shutdown
abandonment. These fields are read in one mutex-protected snapshot.

While running, exact conservation is:

```
readings_queued = readings_written + readings_dropped + reading_depth + in_flight_readings
readings_queued + events_queued = records_written + readings_dropped + events_dropped
                                + reading_depth + event_depth + in_flight
```

Drops concern unsubmitted history; abandoned counts report the final memory
subset and must not be added to that same equation again. In-flight records
can have an ambiguous database outcome. Outage duration begins at observed
failure, not the unknowable instant a network/server fault first occurred.
All counter/timing fields are process-local uint64 values; no infinite-uptime
or hard real-time guarantee is made. Diagnostic lines can still be dropped
under stdout backpressure, as in Checkpoint 2; SQL assertions use direct queries.

Example queries (replace UUID):

```sql
SELECT seq, observed_at, pod, channel, kind, detail
FROM vigil.history WHERE run_id='UUID' ORDER BY seq;
SELECT batch_id, observed_at, reading_drops, event_drops, detail
FROM vigil.checkpoints WHERE run_id='UUID' AND kind='gap' ORDER BY batch_id;
SELECT alarms FROM vigil.checkpoints WHERE run_id='UUID' AND alarms IS NOT NULL
ORDER BY batch_id DESC LIMIT 1;
```

## systemd installation

Inside the repository's Ubuntu VM, after the clean gate:

```sh
./scripts/install-service
./scripts/setup-database
./scripts/vcan-up
sudo systemctl enable --now vigild
systemctl status vigild
journalctl -u vigild
```

The installer copies the built binary/unit, creates a system account without
login, preserves existing `/etc/vigil` configuration and reloads systemd.
It does not start the service on its own. CAN administration is separate and
can require sudo; `vigild` needs no capabilities/root privileges for SocketCAN.
After reboot, configure the desired CAN interface before starting the service.
The supplied virtual-interface setup is a development command, not hardware
network provisioning. Stop the service before running tests on the same bus.

`Type=notify` readiness means the monitoring descriptors and writer thread
are initialized, even if the database is down. `WatchdogSec=4s` is fed by the
main loop every half interval. It supervises the monitoring loop, not database
availability. A stopped/stalled main misses its deadline and systemd restarts
it. SIGKILL also restarts it (`Restart=on-failure`, one-second delay); normal
SIGTERM/service stop does not. Restart storms are limited to ten per 30 s.
TimeoutStopSec=3s exceeds the writer's normal one-second drain budget.

Hardening includes no new privileges, empty capabilities, read-only system,
protected home/kernel/control groups, private temporary/devices namespaces,
and only AF_CAN/AF_UNIX/AF_INET/AF_INET6 socket families. The daemon has no
writable data directory because it has no spool. `After=postgresql.service`
is ordering only: no dependency stops monitoring when PostgreSQL stops.
The real integration gate uses this unit in a temporary test service, a
restricted runtime role, and a disposable DB; it tests notification, watchdog
and two restart causes without stopping any user's PostgreSQL cluster.
