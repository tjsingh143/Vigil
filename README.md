# Vigil

[![Linux build, contracts and fuzz](https://github.com/tjsingh143/Vigil/actions/workflows/ci.yml/badge.svg)](https://github.com/tjsingh143/Vigil/actions/workflows/ci.yml)

Vigil is a Linux sensor-monitoring daemon in C. It distinguishes stale data,
non-advancing publishers and implausible measurements, keeps live alarms in
memory, and admits history to a bounded SQLite durable outbox without storage
I/O in its CAN monitoring thread.

**Vigil Resilience: local release candidate for independent review.** SQLite
freezes each batch before submission. A separate libpq worker replays the oldest
durable batch, verifies its origin and exact receipt bytes, then the SQLite owner
retires it transactionally. Lost responses remain unknown and retry unchanged.
The [outbox contract](docs/OUTBOX.md) describes operation;
[resilience state](docs/RESILIENCE_STATE.md) records actual evidence and the stop
boundary. Deployment requires the additive migration 002 and runtime grants;
001 remains unchanged. Monitoring owns live alarms throughout replay.
[Release/setup/rollback handoff](docs/RESILIENCE_RELEASE.md),
[measured comparison](docs/RESILIENCE_PERFORMANCE.md), and
[current interview guide](docs/INTERVIEW_GUIDE.md) describe this candidate.
It has not been independently verified.

```text
Python pods → real Linux SocketCAN/vcan → main CAN/timerfd/signalfd poll loop
                                         │ decode; freshness/progress/plausibility
                                         │ current in-memory alarms
                                         ▼ bounded copies, short queue mutex
                                 N readings + 256 reserved events
                                         ▼ one SQLite owner pthread
                                 immutable WAL/FULL outbox batches
                                         │ locally committed history survives restart
                                         └ one replay slot → libpq owner → PostgreSQL receipts
                                           ← confirmed result → SQLite retirement
```

Queue publication and a held batch are volatile. A successful SQLite FULL
commit plus parent-directory fsync establishes local durability. Older durable
history is never evicted to admit new records. Recovery preserves original
run/event identity, order and payload; it never restores live sensor state.
No power-cut or physical-CAN qualification is claimed.

## Linux setup and build

Use real Linux for SocketCAN, timerfd, signalfd and systemd. On Apple Silicon,
use the repository's Ubuntu ARM64 Lima VM:

```sh
brew install lima
./scripts/dev up
./scripts/dev ./scripts/verify --clean --output artifacts/my-rc-gate
```

Setup provisions build/test dependencies and an isolated, source-verified
SQLite 3.51.3 under `/opt/vigil-sqlite-3.51.3`; distribution SQLite is preserved.
The minimum version includes SQLite's WAL-reset fix. Native Ubuntu users run
`scripts/setup-linux`. The VM has 2 CPUs, 2 GiB RAM and a 12 GiB sparse disk;
only the repository is mounted read/write. **Outboxes must use native Linux
ext4/XFS, not the macOS repository share or tmpfs.**

```sh
# Linux commands; prefix with ./scripts/dev from macOS.
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
./scripts/setup-outbox /tmp/my-vigil-state/outbox.sqlite3
build/vigild --config config/vigil.conf --outbox-path /tmp/my-vigil-state/outbox.sqlite3
# In another Linux terminal:
.venv/bin/python -m simulator.pod
```

Initialization requires an empty private directory and at least 2 GiB available.
Normal startup requires the existing store; it never silently creates new
history. Use `build/vigild --no-persistence` for explicit monitoring-only mode.
This mode creates no store or worker. `--interface`, `--config`,
`--queue-capacity N` (1..65536, default 1024), and signals retain their behavior.
SIGUSR1 requests status; SIGINT/SIGTERM give local admission a one-second drain
budget, join both workers and report remaining volatile abandonment separately
from durable backlog. Kernel storage stalls can exceed that application budget.

## Protocol and monitoring

Classic CAN, standard 11-bit IDs; pods 1–127; temperature and door channels.

| ID | Payload |
| --- | --- |
| `0x100 + pod` | 6 bytes: channel, little-endian int16 value, status, rolling counter, CRC-8 |
| `0x700 + pod` | 7 bytes: state, little-endian uint32 uptime, independent counter, CRC-8 |

Temperature uses 0.01 °C; door is 0/1. CRC-8/SMBUS and rolling counters detect
anomalies; they do not authenticate senders. Exact decoding, fixtures and
counter ambiguity are in [PROTOCOL.md](docs/PROTOCOL.md). Wire behavior is
unchanged by resilience work.

Freshness, progress and plausibility stay separate. A stale value is never
shown as current; a heartbeat cannot refresh a quiet channel. High alarms use
hysteresis; invalid/stale data cannot clear a high alarm. Flat values are
suspicious, not proof of failure. Receive-time rate checks can reject batched
samples after scheduler stalls. [Monitoring policy](docs/MONITORING.md).

## Capacity, failures and service

Volatile ingestion has N reading slots, a separate 256-event reserve and one
held batch of at most 32 records. Full RAM lanes evict their oldest pending
record and count the loss. Durable capacity is 65536 total records, 4096
batches and 64 MiB of payload, with no durable eviction or disk event reserve.
Database/WAL/SHM physical limits and checkpoint policy are separate from payload
capacity. FULL/BUSY/READONLY and transient failures retain a bounded candidate
and pace retries while monitoring continues. Corrupt/inconsistent state fails
closed; no automatic reset, sidecar deletion or repair exists.

The unit remains unprivileged with Type=notify and a main-loop watchdog.
`StateDirectory=vigil` supplies mode 0700 state; DB/sidecars/lock use 0600.
Readiness requires validated local storage. An explicitly authorized service
installation uses `scripts/install-service`, then `scripts/setup-outbox` with
no path, separate CAN setup, and explicit start/enable. No default service or
user database is changed by the tests. [Operation/details](docs/OUTBOX.md).

## Tests and evidence

Use an otherwise idle vcan0. Tests create disposable outboxes, PostgreSQL
clusters, ext4 loop mounts and a temporary systemd unit; they never target a
developer's actual SQLite or PG data. Sudo is needed for network/mount/unit
setup; the daemon remains unprivileged.

```sh
./scripts/check-resilience --clean --output artifacts/my-rc-gate
./scripts/check-ci --clean      # hosted subset: no vcan/systemd dependency
./scripts/check-release --clean # optimized units + vcan/monitor/local recovery
.venv/bin/python tests/database_contract.py # original PG v1 schema, unchanged
```

The complete candidate gate includes strict GCC/Clang builds, C/Python tests,
genuine vcan monitoring, deterministic SIGKILL cuts, byte/identity
comparisons against recovered DB/WAL, capacity/accounting/capture races, real
ENOSPC/read-only storage, hardened supervision, sanitizers, Helgrind, GCC analyzer,
manual can-utils exchange, finite receive-load accounting and decoder fuzzing.
Expectations precede execution. gate.json records exact commands, exits and
implementation digest; logs and native DB/WAL inventories retain raw evidence.
Production binaries exclude the test fault dispatcher and VFS. The gate also
runs the asserted production-binary demo and three alternating original/Resilience
benchmark trials with explicit warm-up and measurement boundaries.

```sh
cmake -S . -B build-resilience-release -DCMAKE_BUILD_TYPE=Release -DVIGIL_BENCHMARKS=ON
cmake --build build-resilience-release --parallel 2
./scripts/demo --build build-resilience-release
./scripts/benchmark --build build-resilience-release --runs 3 --seconds 60 \
  --output artifacts/my-comparison
```

The demo shows healthy pods, durable local admission, stopped disposable PG,
SIGKILL/restart before PG recovery, original records retained, safe old-run replay,
ambiguous-commit duplicate suppression, current alarms and exact final counters.
Resources are disposable and cleaned up; raw evidence remains under `artifacts/`.
The benchmark builds pinned original `d179681a40497ada533d2f61e50b5ac60a6ca293`
in a temporary detached worktree and alternates matched Linux trials. It reports
variability and explicit limits; it does not establish universal throughput.
See the release handoff for native-filesystem build-copy instructions.

The [original 1.0 release audit](docs/FINAL_VERIFICATION.md),
[original measured report](docs/VERIFICATION.md),
legacy interview fixture (`tests/interview_demo.py`) and PG scenarios remain historical
records for the pre-outbox implementation. Their PG backfill/volatile-crash-loss
expectations do not describe the durable-outbox daemon. The hosted badge covers the published
1.0 revision until this local checkpoint is explicitly published; no new hosted
CI run is claimed. CI configuration provisions SQLite and runs the v1/v2 PostgreSQL contracts.

No hard real-time, physical CAN, hardware power-cut, unlimited retention or
unconditional delivery deadline. Replay requires a healthy head and adequate storage/service capacity. vcan lacks arbitration, bitrate and electrical
behavior. Diagnostic pipes can drop counted lines; regular-file output can
block. API/dashboard, brokers, Modbus, authentication and hardware extensions
remain outside this checkpoint. [Decisions](docs/DECISIONS.md),
[learning guide](docs/LEARNING_GUIDE.md), [state](CODEX_STATE.md).
