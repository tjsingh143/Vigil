# Monitoring contract (Checkpoint 2)

`vigild` is a foreground Linux daemon whose main thread owns monitoring.
The monitoring core uses no heap allocation and has bounded state for 16 pods.
Checkpoint 3 adds a separately owned persistence thread and allocated queue.
`src/monitor.c` is the clock-injected core; `src/config.c` loads startup
configuration; `src/vigild.c` owns descriptors and diagnostics. Wire version 1
and the shared cross-language vectors are unchanged.

## Configuration

With no arguments, `build/vigild` binds `vcan0` and monitors pod 1's temperature,
door and heartbeat. `--interface NAME` changes the interface. `--config FILE`
replaces the complete pod list. See [config/vigil.conf](../config/vigil.conf)
for defaults and the three lines to enable pod 2. No live reload, includes,
environment substitution, dynamic discovery or database settings.

```text
tick_ms 100
status_ms 1000
pod 1 3000
# source POD CHANNEL timeout_ms min max rate_per_s flat_ms high clear
source 1 temperature 3000 -4000 8500 500 60000 2700 2600
source 1 door        3000 0 1 0 0 1 0
```

Whitespace separates fields; `#` starts a comment. All numbers are decimal
integers. Define each distinct pod ID (1..127) before its sources; each pod
needs at least one of `temperature` or `door`. Unknown fields, duplicate
definitions, oversized lines, overflow and inconsistent limits fail startup
with exit 2. Parsing is transactional; the file closes before CAN opens.

| Setting | Meaning and bounds |
| --- | --- |
| `tick_ms` | Housekeeping period, 10..1000 ms; default 100 |
| `status_ms` | Snapshot interval, tick_ms..86400000; 0 disables periodic snapshots; default 1000 |
| `pod ID TIMEOUT` | Heartbeat timeout, tick_ms..86400000 ms |
| Source timeout | Age of last usable reading, tick_ms..86400000 ms |
| `min max` | Inclusive range inside int16, min < max; door must be 0..1 |
| `rate_per_s` | Maximum absolute change in wire units/second, 1..65535; 0 disables |
| `flat_ms` | Equal-value observation window, tick_ms..86400000; 0 disables |
| `high clear` | Inclusive raise/clear thresholds, min <= clear < high <= max |

Temperature units are 0.01 C: rate 500 means 5 C/second and high 2700 means
27 C. Door uses 0/1; high=1 means open, clear=0 closed. Door rate/flat checks
default off because discrete changes and long closed periods are normal.
Defaults are demonstration policy, not limits calibrated for equipment.

## Freshness

Each channel stores its own last usable value/time. Heartbeat separately
stores the last progressing heartbeat time, pod state and uptime. Startup
state is `never`, with no current value. At age >= timeout (measured from
startup when never seen), state becomes `stale` and raises `communication_lost`
once. Last-known values remain visible with `current=0`. A silent two-channel
pod produces three stale events: two readings and one heartbeat.

Only a usable reading clears that channel's communication alarm; a heartbeat
cannot refresh it. Fault/range/rate/repeat/malformed frames cannot refresh
readings. Communication loss with ongoing invalid traffic means "no valid
data by the deadline," not a diagnosis of broken wiring. A fresh booting/fault
heartbeat raises a separate `pod_not_operational` alarm. Timely readings and
a missing heartbeat remain independently visible.

All deadlines use CLOCK_MONOTONIC. Checks occur before every received frame
and on timer wakeups. During silence, detection occurs on the first tick at
or after the deadline: timeout + at most one tick, plus OS scheduling delay.
The VM is not a hard real-time system.

## Progress and recovery

Temperature and door share one counter baseline per pod; heartbeat has another.
No baseline accepts any first counter without inferring loss. Modulo-256 delta
1..127 advances; N infers N-1 missing frames. Delta 0 rejects as `repeat`;
128..255 rejects as `old`/ambiguous. Both preserve the baseline and raise
`sequence_anomaly`. A gap raises that warning while accepting usable data.
The next orderly frame or new baseline clears it. 255 -> 0 is ordinary progress.

Sequence acceptance precedes physical checks: a range/status/rate rejection
still advances an accepted baseline. Both channels advance the shared stream
even if one is excluded from monitoring (its measurement is then rejected as
`unconfigured_source`). Otherwise the next included good reading would falsely
count received frames as lost. Unknown pods cannot create runtime sources.

Reset a baseline only when a subsequent decoded frame follows a full silence
interval on that stream. Reading silence uses the maximum configured channel
timeout; heartbeat uses its own timeout. Repeats, old frames, excluded channels
and invalid measurements all update the decoded stream's last-seen time.
An endless repeated stream therefore cannot reset itself merely because
usable readings expire. Malformed frames do not keep a baseline alive.

A restart after silence accepts a new baseline, counts the reset and does not
guess losses across the gap. A fast restart may be rejected until counters
catch up or may look like a forward gap. Uptime/state alone cannot reset it.
An old frame after silence may pass as a first frame. Eight-bit counters cannot
resolve arbitrary replay/restart/loss; CRC and counters do not authenticate.

## Plausibility and alarm lifecycle

Rate uses daemon receive/processing time, not a timestamp supplied by the sensor.
A scheduling stall can deliver several queued readings together and create an
apparent rate spike, including differing values at the same millisecond. These
may be rejected even if the source emitted them at a physically reasonable
rate. This is a known limitation of the current arrival-time policy. Transport,
count and silence fixtures disable rate checking to isolate their assertions;
deterministic C tests still check exact rate behavior. A future timestamp-aware
policy would require separate design and evidence, not an undocumented relaxation.

Status-fault, range and rate failures retain the last usable value/time,
record the latest validity reason and raise `measurement_invalid`.
`freshness=fresh validity=range current=0` means the old value is recent but
the latest progressing measurement failed; it is not current valid data.
Malformed frames change only rejection counters, never source/alarm state;
the timer may independently expire state at the same time.

Rate uses bounded integer arithmetic:
`abs(new - last_valid) * 1000 <= rate_per_s * elapsed_ms`. Equality passes;
with zero elapsed milliseconds only an unchanged value passes. Rejected
outliers never become the reference. Once the reference is stale, a new
in-range reading establishes a new slope reference. Range still applies.

Flatness requires accepted equal readings spanning the window. Silence alone
cannot raise it. Changed values, invalid measurements and stale gaps end the
observation and clear suspicion. Flat values remain valid/current while fresh;
`flat_suspicion` is a warning, never proof of sensor failure. A stable room
can explain it.

| Alarm kind | Scope | Severity | Clear condition |
| --- | --- | --- | --- |
| `communication_lost` | Heartbeat/channel | critical | New progressing heartbeat/usable channel reading |
| `sequence_anomaly` | Heartbeat/shared reading stream | warning | Orderly frame or silence re-baseline |
| `measurement_invalid` | Channel | warning | Next progressing usable reading |
| `flat_suspicion` | Channel | warning | Changed value, invalid measurement or stale gap |
| `high` | Channel | critical | Usable value <= clear threshold |
| `pod_not_operational` | Heartbeat | warning | Progressing operational heartbeat |

An inactive high alarm raises at value >= high; an active one clears at
value <= clear. Values between hold the state. Exact 27/26 C boundaries are
intentionally inclusive, also supporting binary door thresholds. Stale or
invalid data never clears a high alarm: retained high state with `current=0`
does not assert a current above-threshold measurement.

Every alarm stores active state, severity, last raise/clear monotonic times
and lifetime transition count. Prior cleared time remains when it raises
again. The monitoring thread owns all state. Its synchronous transition
callback must not reenter the core or block. Checkpoint 3 now copies transitions
to the bounded persistence queue; libpq remains exclusively in the writer thread.

## Diagnostics and exact counts

Stdout emits readable `event key=value ...` lines; stderr reports readiness
(`pid=...`) and errors. SIGUSR1 requests a snapshot; SIGINT/SIGTERM stop cleanly
with a final snapshot. Start/stop snapshots also occur with status_ms=0.
Each snapshot has `snapshot id=N`, rows carrying `snapshot=N`, and a matching
`snapshot_end id=N`. Channels: 0 temperature, 1 door, -1 heartbeat, -2 shared
reading stream. All `*_ms` timestamps are Linux-boot monotonic milliseconds,
not UTC or pod uptime. Subtract `started_ms` for daemon-relative times.
`has_value=0` means the numeric value is a placeholder; `seen=0` similarly
marks unknown heartbeat state/uptime.

`frames = valid_readings + heartbeats + rejected`; `rejected` equals the sum
of reason counts. `repeat` + `old` are non-advancing rejections. `loss` is
inferred missing sequence frames, not measured physical bus loss.
`stale_events`, `recoveries`, `alarm_transitions` and `counter_resets` count
transitions, not polling iterations. Per-source accepted and per-stream
sequence counts explain totals. Monitoring counters have their own structure;
persistence queue/write/drop/outage counters are in a separate `persistence` row
following each snapshot; see [PERSISTENCE.md](PERSISTENCE.md).

`rx_overflow` comes from Linux SO_RXQ_OVFL. It is distinct from sequence loss
and must not be added to it as an independent loss. It updates only when a
later delivered frame carries the cumulative kernel count; final drops may
remain unreported without that later frame. `missed_ticks` counts timer
expirations beyond one per timer read.

Diagnostic lines use bounded nonblocking writes. Full/closed pipes drop
lines and increment `diagnostics_dropped`; alarms remain in memory. For
exact evidence reject incomplete/dropped snapshots and request a later one
with a draining reader. No event history is retained. O_NONBLOCK cannot stop
regular filesystem writes from blocking: use a responsive terminal or pipe,
not a potentially stalled network filesystem. Output is not durable history.

The socket receives classic data/RTR traffic including malformed standard/
extended IDs for rejection accounting. CAN FD and error notifications are
not subscribed to. Arrival times currently mark userspace processing; socket
backlog can delay them. Bounded batches and overflow counters are safeguards,
not high-rate capacity evidence. No physical CAN performance claim is made.

Checkpoint 3 adds PostgreSQL history and systemd supervision without changing
these monitoring decisions or wire semantics. History timestamps include UTC
and monotonic ms; monitoring freshness still uses monotonic time only.
