# Vigil state

Checkpoint 2 is implemented and its final clean Linux gate passed on
2026-09-25. Checkpoint 3 has **not** started. Development used one Codex agent,
no delegation or additional Codex instance. The complete eight-page version 3
brief, AGENTS.md, previous state, decisions and learning guide were read before
edits. Starting commit `d37e81c` was clean and its complete clean gate passed.

## Implemented through Checkpoint 2

- Checkpoint 1 foundation remains: Ubuntu ARM64 Lima VM, strict C11/CMake,
  original classic CAN protocol/CRC/counters, Python SocketCAN simulator,
  C decoder diagnostic and ten shared cross-language wire vectors.
- `vigild`: one main poll loop over nonblocking CAN, monotonic timerfd and
  signalfd. Bounded 64-frame receive batches, signal-driven shutdown/status,
  explicit descriptor cleanup, no project heap allocation or database calls.
- Pure clock-injected monitoring core with separate per-channel freshness,
  per-stream progress, and physical plausibility. Repeats/old frames reject;
  forward gaps count inferred loss and accept usable data; wrap is supported.
- Recovery after genuine decoded-stream silence resets sequence baselines;
  continuously repeated frames cannot refresh data or reset themselves.
- Per-source range, rate and flat-window checks. Invalid readings retain the
  last usable value/time with current=0. Flatness is warning-level suspicion
  and never a claim of proven sensor failure.
- In-memory alarm lifecycle/severity/raise-clear times/transition counts,
  threshold hysteresis, separate communication/sequence/invalid/flat/high/
  pod-state categories. Stale/invalid data does not clear high alarms.
- Small validated startup config for up to 16 chosen pod IDs and two channels;
  built-in settings and `config/vigil.conf` match simulator pod 1.
- Readable periodic/SIGUSR1/final snapshots and alarm transitions. Exact frame
  outcomes/rejection reasons, valid readings, heartbeat, loss, stale/recovery,
  reset and alarm counters; separate kernel-overflow/diagnostic-drop counts.
- Minimum simulator fault control: `--repeat-each-frame` sends each original
  frame twice. Wire bytes/semantics, decoder and shared vectors are unchanged.
- 26 new cmocka monitoring/config cases (39 total) and a real SocketCAN daemon
  integration gate. README, decisions, monitoring contract and learning guide
  explain current behavior and its limits.

## Environment and commands

The repository's `vigil` Lima VM remains the real Linux gate: Ubuntu 24.04.5
LTS ARM64, kernel `6.8.0-139-generic`, GCC 13.3.0, CMake 3.28.3, cmocka 1.1.7,
Python 3.12.3/python-can 4.6.1. Host is Apple Silicon macOS. No macOS CAN mock
or Python virtual bus was used.

From macOS:

```sh
./scripts/dev up
./scripts/dev ./scripts/check --clean
./scripts/dev build/vigild
# Another terminal:
./scripts/dev .venv/bin/python -m simulator.pod
```

The VM was already running this session. Setup/reboot evidence from Checkpoint
1 remains in `docs/VERIFICATION.md`; it was not repeated for Checkpoint 2.
Use `./scripts/dev ./scripts/vcan-up` after reboot. Do not execute Linux build
outputs or shared `.venv` binaries directly on macOS. Tests clean up their
simulators/daemons and observers; the VM is intentionally left available.

## Observed verification

The final `./scripts/dev ./scripts/check --clean` rebuilt everything after
review fixes. Both Debug and ASan/UBSan passed all 39 cmocka cases; all six
Python unit tests passed. Both old decoder integration gates and both new
real daemon gates passed. No compiler warnings or sanitizer reports.

- Normal two-pod simulator: exactly 18 frames, 12 usable readings, 6 heartbeats,
  zero rejects/loss; distinct channel values and stream baselines verified.
- Repeated simulator: exactly 36 frames, 12 usable readings, 6 heartbeats,
  18 repeat rejections, zero inferred loss.
- Stop pod 1 while pod 2 runs: three communication alarms, last-known values
  stale/current=0, pod 2 current. With timeout 600 ms/tick 20 ms, observed
  clean-run alarm ages included [606, 606, 606] ms Debug and [617, 617, 617] ms
  sanitized. The final post-review clean run also passed (sanitized ages
  [622, 622, 622] ms); every run passed the 600..820 ms scheduling bound.
  Restart produced 3 recoveries/2 baseline resets, no rejects/inferred loss.
- Nine malformed frames rejected by exact reason without value/timestamp/alarm
  changes. Gap accepted with loss=3; repeat/old/range/fault/unknown-pod rejected;
  threshold oscillation produced one raise and one clear. `cansend` reached
  the live daemon independently.
- Ongoing repeated readings stayed stale across more than three timeouts while
  progressing heartbeats remained fresh. SIGUSR1 snapshots and SIGINT/SIGTERM
  shutdown passed, including startup failure paths and optimized-Python guard.
- `/proc` inspection found one CAN socket, one timerfd, one signalfd and no open
  config file. A deliberately unread 4 KiB diagnostic pipe did not stop CAN
  processing/timeouts; drops were counted and high alarm state retained.
  Closing the pipe reader did not cause SIGPIPE termination.
- Shell syntax and diff whitespace checks passed. Full change review covered
  scope, ownership, lengths, conversions, clock arithmetic, counter/reset policy,
  rejected-value immutability, config, alarm lifecycle and documentation claims.

See `docs/CHECKPOINT_2_VERIFICATION.md` for expectations and final results.

## Limits and deferred work

No known unresolved Checkpoint 2 defect. Evidence is Ubuntu ARM64/vcan only,
not physical CAN, high-rate capacity or real-time latency guarantees. CRC and
8-bit counters cannot authenticate, resolve arbitrary loss/replay or reliably
identify rapid restarts. Recovery policy and slope re-baselining are explicit
in `docs/MONITORING.md`. Receive timestamps are userspace processing times;
backlog can delay them. Kernel overflow reports may lag until the next frame.

Alarms/counters are volatile; stdout is not durable history. Full pipes drop
counted diagnostics; regular-file writes can still block. No config reload,
CAN FD/error subscription, PostgreSQL/libpq, persistence thread/queue, systemd
notify/watchdog service, fuzz target, CI, outage/crash evidence, dashboard,
API, Modbus, authentication, Buildroot or hardware work was added.

## Exact next checkpoint: 3 — persistence, only when authorized

1. Reread the complete brief and current records, inspect Git, rerun the clean
   Checkpoint 2 gate before changes. Preserve all monitoring semantics/tests.
2. Add the bounded persistence queue and dedicated writer thread that exclusively
   owns libpq. Use connect_timeout/statement_timeout. Copy batches under the
   mutex, release it, then perform I/O. Never put database calls in poll/core.
3. Keep alarm state owned by monitoring memory. Add distinct queued/written/
   dropped/outage counters; preserve exact evidence of history loss.
4. Implement finite buffering, oldest-reading drops when full, persistence-
   degraded alarm, gap history and current-alarm snapshot upon recovery.
   Do not claim crash durability for an in-memory queue.
5. Add deterministic queue tests and real PostgreSQL outage/recovery evidence,
   update docs/state, review, clean gate and local checkpoint commit.

This checkpoint uses per-command Git identity `Codex <codex@localhost>` as
Checkpoint 1 did, without changing global settings or publishing. Identify
its commit with `git log -1 --oneline`.
