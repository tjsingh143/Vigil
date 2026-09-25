# Checkpoint 2 verification

## Expectations recorded before implementation/tests

The starting clean Checkpoint 1 gate must pass before changes: strict Debug
and ASan/UBSan builds, 13 cmocka cases each, six Python tests, both real vcan
decoder integrations (18 simulator frames and seven malformed frames each).
Observed on 2026-09-25: all passed; no sanitizer reports. Git was clean at
`d37e81c`; that initial commit contains the entire previous checkpoint.

The expanded gate must retain those checks and add the following:

| Scenario | Expected behavior |
| --- | --- |
| Initialization/reset | No current values or active alarms; startup grace then missing-source alarms; reset removes all runtime state |
| Counter first/next/wrap | Independent baselines per pod and stream; first accepts any count; 255 -> 0 accepts without loss |
| Repeat/old | Reject and count separately; preserve value and freshness timestamp; raise sequence anomaly, never claim identity |
| Forward gap | Accept usable data and count exactly delta-1 missing frames; sequence alarm clears on next orderly frame |
| Silence | At configured age, mark last-known values stale and raise distinct reading/heartbeat communication alarms once |
| Recovery | Fresh valid reading clears its stale condition; heartbeat alone never refreshes a reading; silence permits counter re-baseline |
| Continuous repeats | Never reset baseline merely because accepted readings timed out; stale data cannot periodically become current |
| Range/rate/fault status | Reject measurement, retain last valid value/time; mark latest measurement invalid; progress still advances |
| Flat readings | Repeated accepted equal values over the configured window flag suspicion only; changed value clears it; no suspicion from silence |
| Threshold/hysteresis | Raise at high, clear at low, hold inside band; rejected or stale values cannot clear a threshold alarm |
| Multiple sources | Pod/channel health, values, thresholds and timestamps remain independent; reading counter remains shared across channels |
| Malformed input | Exact rejection reason; decoded output/state and alarms unchanged by the frame |
| Configuration | Valid small explicit config; reject typos, duplicates, overflow, contradictory limits and missing definitions before opening CAN |
| Linux integration | Actual vcan0 with simulator: multi-pod readings, stop/restart, heartbeat-only, repeated counters, malformed traffic, exact counts |
| Diagnostics/shutdown | Human-readable snapshots and transitions; SIGUSR1 snapshot; SIGINT/SIGTERM exit cleanly even with pending CAN traffic |
| Ownership/sanitizers | Descriptors closed, signal mask restored, bounded memory; same core and live daemon checks under ASan/UBSan |
| Diagnostic backpressure | A deliberately unread small pipe must not delay CAN processing or timeouts; dropped lines counted, in-memory alarms preserved; closed reader must not cause SIGPIPE termination |

Core tests use explicit monotonic milliseconds, including exact timeout and
threshold boundaries. Live tests wait for readiness and observable conditions
with deadlines; only real timeout behavior relies on elapsed Linux time.
No PostgreSQL, queue, systemd service, fuzzing or physical CAN evidence is claimed.

## Observed final clean gate (2026-09-25)

`./scripts/dev ./scripts/check --clean` passed after implementation and review
fixes. Environment: the existing Ubuntu 24.04.5 ARM64 Lima VM, Linux
6.8.0-139-generic, GCC 13.3.0, CMake 3.28.3, cmocka 1.1.7 and Python 3.12.3.
All bus scenarios used the real Linux `vcan0` socket interface.

| Executed check | Observed result |
| --- | --- |
| Debug and ASan/UBSan clean builds | Strict warnings as errors; both compiled/linked successfully |
| CTest in both builds | 13 existing protocol + 26 monitoring/config cmocka cases passed (39 per build) |
| Python unittest | All six existing cases passed |
| Old decoder integration, both builds | Exact 18 simulator frames; seven malformed cases; can-utils, CLI and signal checks passed |
| Normal daemon/simulator, both builds | Exactly 18 received = 12 usable readings + 6 heartbeats; 0 rejects/loss; two pods/two channels verified |
| Repeat injection, both builds | Exactly 36 received = 12 usable readings + 6 heartbeats + 18 repeat rejects; 0 inferred loss |
| Stop pod 1, keep pod 2 running | Only pod 1's three sources stale/current=0; pod 2 remained fresh |
| Timeout 600 ms, tick 20 ms | Clean-run samples [606,606,606] ms Debug and [617,617,617] ms sanitized; final sanitized [622,622,622] ms; every run passed the 600..820 ms scheduling bound |
| Restart pod 1 | 3 recoveries, 2 stream resets; no rejects/inferred loss |
| Malformed daemon traffic | 9 rejects: length, CRC, ID, EFF, RTR, channel, status, door value and heartbeat state; no value/time/alarm mutation |
| Sequence/physical faults | Gap accepted with exactly 3 inferred missing frames; repeat, old, range, fault-status and unconfigured pod each rejected once |
| Live hysteresis | Threshold oscillation produced one raise and one clear; independent cansend updated the daemon afterward |
| Repeated readings with live heartbeat | Only initial two readings accepted; no baseline resets over >3 timeout intervals; readings stale and heartbeat fresh |
| Signals and errors | SIGUSR1 with periodic output disabled produced a snapshot; SIGINT/SIGTERM exited 0 within 2-second deadlines; invalid CLI/setup exited 2 |
| Descriptor ownership | /proc showed exactly one CAN socket, one timerfd, one signalfd; config file closed before readiness |
| Unread/closed stdout pipe | CAN reading processed before the 0.6-second drain; 3 timeouts occurred, high alarm held, diagnostic drops counted; closed reader still allowed exit 0 |
| Sanitizers | Unit tests, live daemon, decoder and exercised cleanup/error paths: no reports |
| Shell syntax / whitespace | bash -n scripts/dev scripts/setup-linux scripts/vcan-up scripts/check and git diff --check passed |

Normal snapshot assertions balance total frames against usable readings,
heartbeats and per-reason rejects. No socket overflow or diagnostic drop was
observed in those scenarios. The separate backpressure test intentionally
forces diagnostic loss and verifies memory state through subsequent output;
it does not assert a scheduling-dependent exact diagnostic-drop count.

Unit expectations cover exact first/next/gap/wrap/repeat/old boundaries,
startup grace/reset, timeout/recovery, heartbeat-only freshness, per-channel
and per-pod separation, range/rate boundaries (including dt=0 and negative
slope), stale rate-reference reset, flat suspicion/clearing, high-alarm
hysteresis, invalid/stale high-state retention, pod states, transactional
config errors and callbacks. Core clocks are injected; no sleeps are used.

## Review and scope

Review found that excluding one channel must not exclude its contribution to
the pod's shared reading sequence. Fixed sequence accounting to advance for
decoded excluded-channel frames, then reject the excluded measurement; the
regression case verifies no false inferred loss. Reviewed immutable decoded
results, retained usable values, bounded rate arithmetic, source isolation,
silence reset policy, descriptor/signal cleanup, config ownership and output
backpressure. Final review also preserved the event-loop errno across final
diagnostic writes so output errors cannot replace the fatal cause. The full
clean gate was rerun after that change. No decoder/wire/vector behavior changed.

The baseline, incremental strict/core gates, both integration variants and
the final full clean gate all passed. No failing build/test was concealed or
silently skipped. The full diff and persistent records were reviewed before
the local commit. Test processes were cleaned up; the Linux VM remains running.

Evidence does not cover physical CAN, arbitrary replay/large counter gaps,
load capacity, kernel-overflow injection, hard real-time scheduling, filesystem
stall resistance, durable history, database outages, a systemd service, fuzzing
or CI. Those limits and the fast-restart policy are documented in
[MONITORING.md](MONITORING.md). Checkpoint 3 has not started.
