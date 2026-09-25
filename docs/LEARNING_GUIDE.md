# Understanding Vigil: Checkpoints 1 and 2

## What exists today

A Python program pretends to be one or two sensor pods and sends CAN frames
through Linux. The C daemon `vigild` decodes them, checks their freshness,
progress and physical plausibility, and maintains alarms in memory. A smaller
decoder diagnostic remains useful for examining literal frames. Start with
`README.md`, then read `src/protocol.c` alongside `docs/PROTOCOL.md`, followed
by `src/monitor.c` and `docs/MONITORING.md`. PostgreSQL is not implemented yet.

## What vcan and SocketCAN mean

CAN normally carries small messages among devices on a shared wired bus.
An ID identifies a message and participates in bus arbitration; it is not a
cryptographically trustworthy sender address.

SocketCAN is Linux's networking interface for CAN. Programs create a socket,
bind it to an interface such as `vcan0`, and send/receive frames. This lets our
C program, python-can simulator and candump observer use the same kernel bus.
Each observer gets its own copy; candump does not steal the C decoder's frames.

vcan is a **virtual CAN network interface inside a Linux kernel**. No adapter
or wiring is required. A sent frame is delivered to subscribed local sockets.
Our Mac therefore runs Ubuntu in a VM; macOS itself is not providing SocketCAN.
Another VM has another kernel and cannot automatically see this `vcan0`.

vcan tests application logic and integration. It does not reproduce bus
arbitration, bitrate limits, electrical noise, termination or bus-off faults.
Even a successful high-rate vcan test would only measure application behavior.
No throughput measurement has been made in this checkpoint.

## Walk through a reading

The literal frame `101#0098080000B7` means:

| Part | Interpretation |
| --- | --- |
| `101` | Hex CAN ID 0x100 + pod 1: a reading |
| `00` | Temperature channel |
| `98 08` | Little-endian integer 0x0898 = 2200, meaning 22.00 C |
| `00` | Pod reports OK status |
| `00` | Reading counter zero |
| `B7` | CRC computed over the five preceding payload bytes |

Little endian means the least significant byte comes first. Negative
temperature uses two's-complement int16: `FF FF` means -1, or -0.01 C.
The C decoder deliberately computes this conversion without relying on how
a compiler converts an out-of-range unsigned integer to a signed integer.

Door readings use channel 1 and a value of 0 (closed) or 1 (open). A reading
status of 1 means the pod reports a sensor fault; the frame can be syntactically
valid while its value must not be treated as a healthy measurement later.

`701#01000000000029` is pod 1's heartbeat: operational state 1, four uptime
bytes holding zero seconds, independent heartbeat counter zero, and CRC 29.
There is no measurement in that heartbeat. The heartbeat counter and reading
counter belong to different streams. Temperature and door share one reading
counter, so a cycle has reading counters 0 and 1, then heartbeat counter 0.

## What the CRC proves

The CRC is a short checksum derived from the payload. The sender appends it;
the receiver computes it again and compares. Many accidental changes produce
a different checksum. We test that every single-bit change in representative
reading and heartbeat frames fails the CRC check.

An 8-bit checksum has only 256 possible results, so some changed messages
will have the same CRC. Our CRC does not cover the CAN ID. Anyone who can send
on the bus can calculate a matching CRC. Therefore it is neither a signature
nor evidence of sender identity. The standard check `123456789 -> F4` helps
verify that both languages use precisely the same CRC variant.

## What counters prove

A stream normally advances 10, 11, 12. Receiving 11 again after accepting 11
does not show progress and is classified as a repeat. Receiving 14 after 11
suggests two missing messages (12 and 13); it can still carry useful new data.
Receiving 0 after 255 is the expected wrap, not a loss.

With only 8 bits, very old frames and very large losses are ambiguous. We
accept forward distances 1..127 and reject the opposite half as old/ambiguous.
For example, 9 after 10 looks old, while 138 after 10 is exactly half a cycle
away and cannot be ordered reliably. A delayed replay from a previous wrap
can still appear fresh. Two senders can produce anomalies, but counters cannot
prove there are two senders or identify either of them. An attacker can also
send an apparently advancing sequence.

The protocol helper returns these facts without storing a baseline. The
Checkpoint 2 monitor owns each pod's two stream baselines, rejection counts
and reset/recovery policy. A repeat or old frame is rejected, counted and
raises a sequence anomaly; a gap is counted while useful data is accepted.

## How the test proves the interaction

The automated integration test first starts candump and sends a known frame
using cansend. It then binds an independent raw CAN receiver and starts the C
probe, waits for its readiness message, and launches two simulated pods for
three cycles. It checks all 18 observed frames and their decoded C fields.
It also injects malformed frames and checks the precise C rejection reason.
No fake bus or mocked Linux call is involved. The same gate runs under C
AddressSanitizer and UndefinedBehaviorSanitizer.

The C tests check exact lengths before reads, unchanged output on rejection,
known wire examples, boundary values and sequence behavior. Python tests use
the same literal fixtures but their own encoder. If one side changes byte
order, IDs, CRC, channel numbering or status semantics, the fixtures should
fail. The fixtures themselves still need review when the protocol changes.

## Three different questions, with examples

Freshness asks whether a usable reading arrived recently. If the temperature
has not updated for three seconds, its old number remains visible but marked
stale and `current=0`. The heartbeat answers its own communication question;
it cannot make old temperature data current. At startup a source is unknown,
not healthy, and gets one timeout interval before a communication alarm.

Progress asks whether counters advance. A pod can repeatedly transmit the
same frame and fill the bus while making no progress. Vigil rejects those
repeats and does not refresh the usable reading time. With continuing repeats,
the sequence warning and a later communication-lost alarm can both be active.
That does not prove spoofing, a second device or even broken wiring.

Plausibility asks whether a progressing measurement makes physical sense.
An impossible value or rate is invalid even if it just arrived with a new
counter. It cannot replace the last good value. Flatness is weaker evidence:
equal accepted readings for the configured window raise `flat_suspicion`,
but remain valid. A stable room can be stable; a closed door can stay closed.
Door flat checking is therefore disabled by default.

These dimensions are separate fields and alarms. `current` only tells a
consumer whether that channel's displayed value is fresh and valid; it is
not a universal pod-health flag. Heartbeat state and sequence anomalies remain
visible even when a channel has recent usable data.

## What poll(), timerfd and signalfd do

Read `src/vigild.c` from `main()` into `receive_batch()`. The main loop gives
`poll()` three file descriptors and asks Linux to sleep until one is ready.
It does not repeatedly query them in a busy loop. A readable CAN socket means
frames can be received. The daemon handles at most 64 per turn, then returns
to poll so a stream of input cannot indefinitely starve shutdown and timers.

A timerfd turns a clock expiration into a readable descriptor. Every 100 ms
by default, it wakes poll even if the bus is completely silent. Reading it
returns the number of expirations; missed periods are counted, and the monitor
compares actual elapsed time rather than pretending every tick was on time.
CLOCK_MONOTONIC prevents a wall-clock adjustment from creating a false age.
Unit tests supply explicit milliseconds, so boundary checks need no sleeps.

SIGINT, SIGTERM and SIGUSR1 are blocked from ordinary asynchronous delivery.
signalfd delivers their records in the same poll loop. The code can process
them in normal program flow, request a snapshot or exit through cleanup,
without doing unsafe logging/state changes from an asynchronous signal
handler. SIGPIPE is also consumed so a closed diagnostic reader cannot kill
monitoring. The CAN socket, timerfd and signalfd all have one owner and close
on handled exits; the original signal mask and output flags are restored.

## Exactly what happens when a pod goes silent

1. The last usable temperature/door readings and progressing heartbeat retain
   their individual receive timestamps. No new usable frames refresh them.
2. Timerfd keeps waking the main loop. At each age >= configured timeout,
   monitoring sets that source stale, increments its stale-event count and
   activates its communication alarm. A two-channel pod normally has three
   separate deadlines; their alarms can occur at different times.
3. The last-known values remain visible with current=0. Existing high alarms
   stay active because silence provides no evidence that the room cooled.
   Further ticks do not repeatedly raise the same alarm.
4. A simulator restart after stream silence establishes new counter baselines.
   Fresh usable readings clear their own communication alarms and increment
   recovery counts. A heartbeat clears only its own communication alarm.
5. Range checks still apply on recovery. Rate restarts from the first new
   in-range value because the previous slope reference is stale. Flatness
   starts a new observation window after the gap.

In the quiet-bus case the timer observes a deadline within one tick after it,
plus Linux scheduling delay; this is not a real-time guarantee. The
baseline-reset timer measures lack of *decoded traffic*, not lack of
accepted values. Endless repeats therefore cannot reset their baseline and
periodically pretend to recover. A quick restart without enough silence can
look old or like a gap; an eight-bit counter cannot resolve that ambiguity.

## Why hysteresis prevents flapping

With one threshold, samples 26.99, 27.01, 26.99, 27.01 C could produce repeated
clear/raise events. Vigil raises at >=27 C and clears only at <=26 C. After
raising, all those samples leave the alarm active. Only evidence at or below
26 C clears it. Clear and raise thresholds must be distinct; the configuration
validator enforces this. An open door raises at 1 and clears at 0.

An alarm contains its active state, severity, raise/clear timestamps and
transition count. The state machine changes these once per transition.
Temperature/door high alarms and communication loss are critical by default;
sequence, measurement-invalid, flat-suspicion and pod-state alarms are warnings.
Stale or rejected readings cannot clear an existing high alarm.

## Reading the evidence

`frames = valid_readings + heartbeats + rejected`. Rejection reasons sum to
the rejected total. A forward gap contributes to `loss` while its usable
reading contributes to valid_readings; a repeat contributes only a rejection.
After a deliberate silence reset, unknown losses are not guessed. Linux
socket overflow is reported separately and may overlap inferred sequence loss.

SIGUSR1 asks for a snapshot. Source rows distinguish last value, freshness,
validity and current; progress rows show per-stream repeats/loss/baselines;
alarm rows show lifecycle times. All timestamps are Linux monotonic ms, not
UTC. Subtract started_ms to get elapsed daemon time. Dropped diagnostic lines
are counted, so an incomplete snapshot is not suitable for exact evidence.
Alarms remain in memory even when a diagnostic pipe fills. They are lost on
daemon restart; stdout is not database history.

The Checkpoint 2 gate exercises the real daemon on vcan0, including a stopped
pod while another continues, restart, duplicates, malformed frames, hysteresis,
signals and a deliberately unread output pipe. Deterministic C tests cover
exact counter, range, rate, flat-window and timeout boundaries. See
`docs/CHECKPOINT_2_VERIFICATION.md` for observed results and limits.

## Next: persistence (Checkpoint 3, not started)

Later, PostgreSQL will store history from a dedicated persistence thread.
Monitoring must not wait for a database call. Memory buffering will be bounded
and will not survive a crash; it must never be sold as guaranteed durability.
