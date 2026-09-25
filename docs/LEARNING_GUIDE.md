# Understanding Checkpoint 1

## What exists today

A Python program pretends to be one or two sensor pods. It sends CAN frames
through Linux. A small C program receives those frames and validates/decodes
them using the same library tested by cmocka. There is no monitoring daemon
yet: decoding alone does not decide whether a source is healthy or raise an
alarm. Start with `README.md` to run it, then read `src/protocol.c` alongside
`docs/PROTOCOL.md`.

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

The current helper returns these facts without storing a baseline. Checkpoint
2 must own the per-pod state, rejection counters and explicit reset/recovery
policy. We have not yet implemented non-advancing rejection in a daemon.

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

## Where this leads

Next, the monitor will answer three separate questions: whether fresh data is
arriving, whether the publisher's sequence advances, and whether values make
physical sense. A heartbeat alone cannot make an old temperature current.
A stable value alone cannot prove a failed sensor. A high temperature alarm
will use separate raise/clear thresholds so tiny fluctuations do not cause
repeated alarm transitions. Those behaviors belong to Checkpoint 2.

Later, PostgreSQL will store history from a dedicated persistence thread.
Monitoring must not wait for a database call. Memory buffering will be bounded
and will not survive a crash; it must never be sold as guaranteed durability.
