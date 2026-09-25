# Vigil state

Checkpoint 1 is complete. Checkpoint 2 has **not** started. The initial
repository contained only `PROJECT_BRIEF.pdf` version 3 (eight pages), fully
read before editing. There was no prior code, build, Git history or remote.

## Implemented

- Dedicated real Ubuntu ARM64 Lima VM, pinned image/checksum, repository-only
  mount, idempotent dependency and vcan setup scripts.
- C11/CMake, strict warnings as errors, Debug and ASan/UBSan builds, CTest.
- Exact classic CAN protocol: pod IDs 1..127, temperature/door int16 readings,
  status, heartbeat state/uint32 uptime, separate per-ID counters, CRC-8/SMBUS.
- Pure C decoder, explicit rejection enums, unchanged output on failure, and
  stateless counter classification including gaps, wrap and half-range ambiguity.
- Python SocketCAN simulator for one/two pods; deterministic samples, configurable
  IDs/interval/count, clean SIGINT/SIGTERM shutdown, pinned dependencies.
- `vigil-decode` literal-frame and bounded live SocketCAN diagnostic; JSON output.
- 13 cmocka cases, six Python unittest cases, ten shared literal wire vectors,
  real two-pod integration with independent receive checks and malformed traffic.
- Protocol, architecture decisions, learning guide, setup instructions and
  expected/actual verification record. Development performed by one agent.

## Environment

Host: macOS 26.5.1 ARM64, 8 GiB RAM, Lima 2.2.0 installed with Homebrew.
VM: `vigil`, 2 CPUs, 2 GiB RAM, 12 GiB sparse disk, Ubuntu 24.04.5 LTS ARM64,
kernel `6.8.0-139-generic`, systemd PID 1. GCC 13.3.0, CMake 3.28.3,
cmocka 1.1.7, Python 3.12.3, python-can 4.6.1. vcan required the running
kernel's `linux-modules-extra` package; setup handles that automatically.

From macOS run `./scripts/dev up`, then `./scripts/dev ./scripts/check --clean`.
For a Linux shell run `./scripts/dev`. Do not run shared Linux `.venv` binaries
directly on macOS. Native Ubuntu uses `./scripts/setup-linux` then `./scripts/check`.
VM is intentionally left available; test simulator/observer processes are cleaned
up. Recreate `vcan0` after reboot with `./scripts/dev ./scripts/vcan-up`.

## Commands actually verified

- `./scripts/dev up`: downloaded/checksummed the pinned image, created Ubuntu,
  installed justified dependencies, created `.venv`, and brought up `vcan0`.
- `limactl stop vigil` followed by `./scripts/dev up`: VM restarted, setup
  reran without extra package installs, and `vcan0` was recreated successfully.
- `./scripts/dev ./scripts/check --clean`: full clean build/test gate passed.
  Both Debug and ASan/UBSan builds passed all 13 cmocka cases; all six Python
  tests passed; both decoder builds passed the complete real SocketCAN gate.
  Each gate matched exactly 18 simulator frames (two pods, three cycles),
  seven malformed frame reasons, literal vectors, invalid CLI arguments,
  and SIGINT/SIGTERM shutdown. No sanitizer reports.
- `./scripts/dev .venv/bin/python -m pip check`: no broken requirements.
- `candump -L vcan0` independently observed all six first-cycle simulator
  frames from `.venv/bin/python -m simulator.pod --pod-id 1 --pod-id 2 --cycles 1`.
- `cansend vcan0 101#0098080000B7` followed by `cansend vcan0 101#0098080000B6`
  through `build/vigil-decode --listen vcan0 2`: accepted value 2200, then CRC
  rejection, expected exit 1. Earlier raw-socket/cansend loopback also passed.
- Optimized Python (`-O`) was verified to fail the integration harness explicitly,
  preventing disabled assertions from reporting false success.
- `bash -n scripts/dev scripts/setup-linux scripts/vcan-up scripts/check` and
  `git diff --check`: passed. Full diff reviewed for scope, ownership, integer
  conversion, protocol agreement, lengths and accuracy of test/document claims.

See `docs/VERIFICATION.md` for written expectations and observed frames.

## Limitations and deferred work

No unresolved Checkpoint 1 defect is known. Evidence is limited to the Linux
ARM64 environment above. vcan does not model real arbitration/bitrate/wiring.
CRC and counters cannot authenticate senders; counters cannot unambiguously
order distances of 128+ increments, determine arbitrary losses, or distinguish
restart from replay. The syntax decoder accepts fault status as an encoding;
future monitoring must reject its measurement as unusable. No physical checks
or runtime counter baselines exist yet. The live probe is not `vigild` and
subscribes only to classic data/RTR frames, not CAN FD or error notifications.

Deferred: daemon event loop, freshness/progress/plausibility, configurable
alarms/hysteresis and monitoring counters (Checkpoint 2); PostgreSQL/libpq,
persistence thread/queue/history (Checkpoint 3); systemd notify/watchdog service,
libFuzzer, CI, load/outage/crash evidence and scripted demo (later core/evidence
work). No Buildroot, dashboard, API, Modbus, authentication or hardware work.

## Exact next checkpoint: 2 — monitoring daemon core, no database

1. Read the complete PDF and project state/decisions/learning guide; inspect Git
   history/status; rerun `./scripts/dev ./scripts/check --clean` for the baseline.
2. Build `vigild` around CAN + `poll()` + monotonic `timerfd` + `signalfd` with
   explicit resource cleanup. Reuse the pure decoder/counter helper, not the
   diagnostic CLI as a production event loop.
3. Add per-pod reading and heartbeat counter baselines, per-channel freshness
   and plausibility state, explicit recovery/reset policy, loss/rejection counts,
   communication-lost handling, and stale last-known values.
4. Add simple source/timeout/range/rate/flat-window/alarm configuration, in-memory
   alarm lifecycle/severity/timestamps/hysteresis and human-readable diagnostics.
5. Use injectable monotonic time in core cmocka tests. Exercise actual vcan
   silence/restart, repeated counters, malformed traffic and multiple channels;
   add only the minimum simulator fault controls needed for those tests.
6. Clean verification, diff review, documentation/state update and local commit.
   Do not add PostgreSQL until Checkpoint 3.

The initial checkpoint uses per-command Git author `Codex <codex@localhost>`
because no user identity was configured. No global Git config changes and no
remote publishing. Identify this checkpoint with `git log -1 --oneline`.
