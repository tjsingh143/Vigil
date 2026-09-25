# Vigil wire protocol, version 1

This is Vigil's own protocol, not A-Link or CANopen. Only the heartbeat ID
convention resembles CANopen. Classic CAN, standard 11-bit IDs, data frames
only. Reject extended IDs, RTR, error frames and CAN FD. Pod IDs are 1..127;
zero and every other ID are reserved. There is no on-wire version field:
incompatible changes require coordinated encoder/decoder deployment.

## Reading: CAN ID 0x100 + pod ID, exactly 6 bytes

| Byte | Encoding |
| --- | --- |
| 0 | Channel: 0 temperature, 1 door; all others invalid |
| 1..2 | Signed int16, little endian, two's complement |
| 3 | Status: 0 OK, 1 sensor fault; all others invalid |
| 4 | Unsigned 8-bit rolling reading counter |
| 5 | CRC of bytes 0..4 |

Temperature is in 0.01 degrees Celsius, with wire range -32768..32767
(-327.68..327.67 C). Physical plausibility limits are separate monitoring
policy, to be implemented in Checkpoint 2. Door is 0 closed or 1 open; other
values are malformed even with fault status. Status 1 is a valid *encoding*,
not a usable measurement. Decoding never establishes physical validity.

## Heartbeat: CAN ID 0x700 + pod ID, exactly 7 bytes

| Byte | Encoding |
| --- | --- |
| 0 | Pod state: 0 booting, 1 operational, 2 fault; others invalid |
| 1..4 | Unsigned uint32 uptime in whole seconds, little endian |
| 5 | Unsigned 8-bit rolling heartbeat counter |
| 6 | CRC of bytes 0..5 |

Uptime is elapsed monotonic time since simulator startup, floor to seconds,
modulo 2^32. It is not a wall-clock timestamp. The brief leaves heartbeat
counter/CRC unspecified; including them makes heartbeat integrity and
progress checkable by the same primitives. No padding or reserved bytes.

## CRC

CRC-8/SMBUS: width 8, polynomial 0x07 (x^8+x^2+x+1), initial 0x00,
RefIn=false, RefOut=false, xorout=0x00. Bytes processed in wire order,
most significant bit first. Empty input gives 0x00; ASCII `123456789` gives
0xF4. CRC covers preceding **payload bytes only**, excluding CRC and CAN ID.
It supplements physical CAN's own CRC; vcan has no physical bus CRC.
It detects many accidental corruptions, not every possible corruption or
ID reassignment. Anyone can compute it. It provides no authentication.

## Counters

One counter per CAN ID: temperature and door **share** the reading counter.
Heartbeat has an independent counter. Each emitted message advances its
stream counter by one modulo 256; the first counter is 0. Every simulator
cycle sends temperature, door, heartbeat, in that order for each pod.

Given an accepted baseline, delta = (current - previous) modulo 256:

| Delta | Classification | Action in helper |
| --- | --- | --- |
| No baseline | first | Accept, no inferred loss |
| 0 | repeat/non-advancing | Reject, no inferred loss |
| 1 | next | Accept, no inferred loss |
| 2..127 | forward gap | Accept; infer delta-1 missing frames |
| 128..255 | old or ambiguous | Reject, no inferred loss |

The pure helper returns classification, acceptance, and inferred loss; it
does not store state. The caller updates its baseline only on acceptance.
A gap is evidence of a sequence anomaly, not proof of physical bus loss.
Half-range ordering requires fewer than 128 increments between observations.
Larger gaps, restart, wrap-delayed replay, and interleaved publishers cannot
be distinguished reliably using an 8-bit counter. A forged advancing sequence
passes. Timeout/reset policy belongs to Checkpoint 2 and is not implemented
by the stateless decoder. Never claim sender authentication or duplicate-pod
identification from these checks.

## Decoder contract and examples

Validation order is arguments, frame format, ID, exact length, CRC, then
channel/status/value or pod state. The first failure supplies the error enum;
the output object remains unchanged. Non-null data must reference `length`
readable bytes. Oversized and truncated lengths are rejected before reading
payload bytes. Unused decoded fields are zero on success.

| Complete frame (hex ID#payload) | Decoded meaning |
| --- | --- |
| `101#0098080000B7` | Pod 1, temperature 22.00 C, OK, reading counter 0 |
| `101#00FFFF008073` | Pod 1, temperature -0.01 C, OK, reading counter 128 |
| `102#01010000FE80` | Pod 2, door open, OK, reading counter 254 |
| `701#01000000000029` | Pod 1, operational, uptime 0 seconds, heartbeat counter 0 |
| `77F#0078563412FFCB` | Pod 127, booting, uptime 0x12345678 seconds, heartbeat counter 255 |

`tests/protocol_vectors.txt` contains literal complete frames and expected
decoded fields. C cmocka and Python unittest both read it; the tests do not
generate their expected payloads using production code. Commands and the
specific annotated vectors are listed there and in the developer README.
