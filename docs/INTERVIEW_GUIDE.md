# Demonstrating and explaining Vigil

Read this with [the learning guide](LEARNING_GUIDE.md). The implementation was
agent-authored under the owner's instructions, a recorded deviation from brief
section 7. Understand and demonstrate it yourself before claiming personal
implementation or testing experience. This guide describes observable behavior.

## Thirty seconds

“Vigil is a C sensor monitor on Linux. Python pods send temperature, door and
heartbeat messages over SocketCAN. The daemon checks freshness, publisher
progress and measurement plausibility separately. Its main loop owns live
alarms; a separate thread writes PostgreSQL history through a bounded queue.
The demo stops a pod and the database, then shows stale detection, exact queue
loss and recovery. It uses a custom protocol, not A-Link.”

## Two-minute architecture explanation

Start at a frame: the decoder accepts only supported classic CAN IDs, exact
payload lengths, CRC and field encodings. It builds a temporary result and
publishes it only on success. Wire validity alone does not make a measurement
physically plausible or identify its sender.

The main thread waits in `poll()` on CAN, `timerfd` and `signalfd`. A periodic
monotonic timer checks deadlines even when the bus is silent. CAN processing
is capped at 64 frames per turn so timers and shutdown get opportunities to run.
Each channel has its own last usable value and freshness; heartbeat is separate.
Sequence progress belongs to the CAN-ID stream. Temperature and door therefore
share one counter, while heartbeat has another. Range, rate and flatness have
different meanings. Flatness is suspicion, and threshold hysteresis prevents
repeated alarm toggles around one boundary.

Main owns all live alarms. It copies readings/events and alarm snapshots into
bounded storage protected by a mutex. The persistence thread copies a batch,
releases the mutex, and then performs libpq I/O. A slow database cannot hold
the queue lock. During an outage, pending queues fill and evict oldest records
with exact counters. Recovery writes retained history, known gaps and a current
alarm snapshot. An immutable batch ID makes retry safe if the COMMIT response
was lost. Pending memory still disappears on a daemon crash.

systemd runs the daemon as an unprivileged user, receives readiness, watches
main-loop heartbeats and restarts failures. All this was exercised on real
Ubuntu ARM64, vcan, PostgreSQL and systemd. vcan validates application behavior;
it cannot demonstrate electrical CAN behavior or hardware portability.

## Run the demonstration

From the macOS host, in this repository:

```sh
cd /Users/tj/Desktop/Vigil
./scripts/dev ./scripts/vcan-up
./scripts/dev ./scripts/demo
```

The VM and dependencies must already be installed and `build/vigild` built;
follow [README](../README.md) for a fresh setup. Keep `vcan0` idle: stop your
other simulators and the installed default service before demonstrating.
Inside Linux the command is `./scripts/demo`. It uses a disposable PostgreSQL
cluster and removes it afterward; it does not stop the default database.

The script prints expectations, narrates the story and asserts results. Any
failure exits nonzero. The closing line names a new directory under `artifacts/`.
It contains `results.json`, raw daemon output/configuration and SQL exports in
`interview/history.json` and `interview/checkpoints.json`. Those exports remain
after the disposable database is removed. Keep the terminal result visible.

| Stage | Explain while it runs | Expected output concepts |
| --- | --- | --- |
| Healthy pod | Three cycles contain six readings and three heartbeats | `freshness=fresh`, `current=1`, accepted counts |
| Pod silent | Each channel and heartbeat expires independently | 500 ms configured deadline, actual detection age, old value with `current=0` |
| Pod returns | Silence allows new counter baselines | Three recoveries; eight readings now committed |
| Database stops | Writer detects failure; main still processes input | `persistence_degraded=1`, capacity four, no readings in flight |
| Eleven hot readings | High alarm and later freshness timeout still work | Seven oldest pending readings dropped, newest four retained |
| Database returns | Reconcile queue counters with actual SQL | Twelve total SQL readings, seven-reading gap, no event loss, degraded clears |
| Clean stop | Join writer after its bounded drain | Zero abandoned records for this run; evidence paths |

Say “observed failure duration,” not the exact physical instant the database
became unavailable. The printed latency and run UUID change per execution.
Use [final verification](FINAL_VERIFICATION.md) for release measurements.
The high alarm remains active when hot data goes stale: silence is not evidence
of cooling. The automated demo intentionally ends in that honest state.

For a slower interactive explanation, follow [MANUAL_CAN.md](MANUAL_CAN.md).
Show `101#0098080000B7`, explain little-endian 2200 = 22.00 C, then send its
bad-CRC variant. `vigil-decode` deliberately exits 1 after malformed input;
that is an expected rejection, not a crashed program.

## Why these choices exist

| Choice | Reason and cost |
| --- | --- |
| One main loop | One owner for monitoring/alarm state; Linux-specific descriptors |
| Monotonic deadlines | Wall-clock corrections cannot change source ages; timestamps describe host processing time |
| Separate persistence thread | Network waits cannot hold up CAN/timeouts through the queue mutex; introduces synchronized state |
| Value-owned queue records | No borrowed pointer can outlive a stack CAN frame; copying has bounded cost |
| Separate event reserve | Alarm chatter cannot consume reading capacity; events can still overflow independently |
| Immutable in-flight batch | Lost COMMIT response does not justify dropping possibly committed records; adds up to 32 records to the bound |
| UUID and batch ledger | Same-process retry is idempotent; a new process cannot reconstruct vanished memory |
| Memory alarm authority | Monitoring remains truthful during DB outages; restart loses live state |
| Hysteresis | High raises at 27 C, clears at 26 C; in-band values hold state |
| Guest Linux/vcan | Real SocketCAN without hardware; separate kernels have separate interfaces |

## Likely technical questions

**What does the decoder validate first?** Arguments, frame format, ID, exact
length, CRC, then enums/value encoding. Rejection leaves its output unchanged.

**Why not cast payload bytes to a struct?** Padding, alignment, byte order and
signed conversion would become assumptions. Explicit shifts and a representable
int32 subtraction decode int16 safely.

**Can heartbeat make a stale temperature healthy?** No. They have independent
freshness deadlines and communication alarms.

**Dead pod or quiet one?** Missing usable data means communication loss for that
source. It cannot distinguish stopped software, power loss or a broken link.
Advancing heartbeats can show a communicating pod with a stale measurement.

**Stable room or stuck sensor?** Equal values with advancing counters can be
legitimate. Flatness raises a warning after a window; it does not invalidate
the value or prove failure. Repeated counters are a separate progress fault.

**What happens at counter wrap?** 255→0 is progress. Delta 1–127 is accepted,
delta 0 repeats, delta 128–255 is old/ambiguous. Replays across wraps and quick
restarts are not reliably distinguishable. Uptime resets do not bypass checks.

**Does CRC prevent spoofing?** No. It covers payload bytes, not CAN ID, and any
sender can compute it. Counters indicate sequence anomalies, not identity.

**What is shared between threads?** Copied records/snapshot, queue metadata,
counters and stop/wakeup state under one mutex. Neither live monitor structs
nor the libpq connection are shared.

**What happens during DB failure?** Failure is observed, degraded raises,
retries are paced, main continues monitoring, and pending oldest records are
evicted if capacity is exhausted. Recovery reconciles rows, gaps and snapshots.

**What does capacity four really mean?** Four pending accepted readings, plus
256 pending events and one shared immutable batch of at most 32 records.
The demo explicitly settles the outage with zero readings in flight before
its eleven-arrival/seven-drop calculation.

**What survives SIGKILL?** Actually committed SQL rows, subject to PostgreSQL
durability. Pending memory and counters disappear. An unconfirmed transaction
may already be committed. The controlled stopped-DB crash test proves three
known lost readings; arbitrary crash loss cannot be reconstructed.

**How would you make the queue durable?** A bounded disk journal with flushing,
replay, acknowledgement, compaction, corruption and full-disk policy, followed
by crash/power-loss tests. A plain file write is insufficient.

**What did the fuzzer find?** The recorded finite decoder runs found no crash,
sanitizer report or invariant violation. Quote the execution count and duration
from final verification. It does not fuzz threads, SQL, alarm state or hardware.
The audit's SQLSTATE parsing defect was found by review, not by libFuzzer.

**Can you promise the timeout?** The deadline is exact in the clock-driven
core. Live observation adds timer quantization and scheduler delay. The 500 ms
fixture allows 20 ms ticks plus 200 ms explicit test allowance; no hard real-time
guarantee is made.

**What did the load test measure?** A finite offered message burst with
persistence disabled. Sent frames equal received frames plus kernel socket
drops after a probe. It measures neither sustained database throughput nor
physical CAN arbitration/bitrate limits.

**What changes on hardware?** Interface selection and much of the logic can
carry over. A board image, controller driver/device tree, bitrate, wiring,
termination and physical validation are new work. QEMU alone would not prove them.

**What limitations would you prioritize?** Start from the intended deployment:
crash durability, source timestamps for rate checks, real CAN validation and
history retention are separate design decisions. They are outside this release.

## Avoid unsupported claims

No A-Link/CANopen compatibility, sender authentication, crash-durable queue,
hard real-time guarantee, production throughput or hardware validation. The
workflow commands passed locally; a hosted GitHub Actions run has not occurred.
Diagnostic pipes can drop counted lines; regular-file output and local libpq
credential/certificate reads can block. Network deadlines do not bound all
filesystem or scheduler behavior. The optional Buildroot build was deferred
without producing an image; its incomplete work was removed. There is no
embedded-image claim. The attempt is recorded in final verification.
