# Vigil

Vigil is a Linux sensor-monitoring daemon in C. It distinguishes stale data,
non-advancing publishers and implausible measurements, maintains alarms in memory,
and records history in PostgreSQL without making monitoring wait for the database.
It exists to make failures observable and testable: what stopped, when it was
detected, what history survived, and exactly what was lost.

The finished core and final audit implement `PROJECT_BRIEF.pdf` v3, with the
documented architecture/process refinements and limitations below.
This is an original small protocol and engineering exercise. It does not reproduce
Schneider NetBotz or A-Link. Start with the [final audit](docs/FINAL_VERIFICATION.md),
[interview guide](docs/INTERVIEW_GUIDE.md) and [measured verification](docs/VERIFICATION.md).

## Architecture

```text
Python pods → Linux SocketCAN/vcan → main C poll loop (CAN + timerfd + signalfd)
                                    │ decode; freshness/progress/plausibility
                                    │ in-memory alarms; readable diagnostics
                                    ▼ copy records under a short mutex
                              bounded queue + alarm snapshot
                                    ▼ persistence pthread, sole libpq owner
                                PostgreSQL history
```

Main owns current alarms. SQL is history, not the authority for live state.
The writer copies a batch, releases the mutex, then performs I/O with bounded
connection/query deadlines. `systemd` runs the daemon unprivileged with readiness,
watchdog and failure restart. [Monitoring](docs/MONITORING.md) and
[persistence](docs/PERSISTENCE.md) define the exact contracts.

## Protocol

Classic CAN, standard 11-bit IDs; pod IDs 1–127; two channels: temperature and door.

| ID | Payload |
| --- | --- |
| `0x100 + pod` | 6 bytes: channel, little-endian int16 value, status, rolling counter, CRC-8 |
| `0x700 + pod` | 7 bytes: state, little-endian uint32 uptime, independent counter, CRC-8 |

Temperature uses 0.01 °C; door is 0/1. CRC-8/SMBUS covers payload bytes before
the CRC. Repeated/old counters are rejected; small forward gaps infer missing
frames while accepting usable data. Exact lengths, validation order, counter
ambiguity and shared C/Python fixtures: [wire protocol](docs/PROTOCOL.md).

## Linux setup and build

SocketCAN, vcan, timerfd, signalfd and systemd require **real Linux**.
On Apple Silicon macOS, use the repository's Ubuntu 24.04 ARM64 Lima VM:

```sh
brew install lima
./scripts/dev up
./scripts/dev ./scripts/verify --clean
```

The VM has 2 CPUs, 2 GiB RAM and a 12 GiB sparse disk; only this repository is
mounted read/write. Setup installs CMake, GCC, Clang/libFuzzer, cmocka, libpq,
PostgreSQL, libsystemd, Valgrind, can-utils and pinned Python dependencies.
The Ubuntu image is pinned; package security updates remain enabled.
Prefix Linux commands below with `./scripts/dev` from macOS, or use
`./scripts/dev` to open a shell. Shared `.venv`/build outputs are Linux files.
After reboot: `./scripts/dev ./scripts/vcan-up`. Stop VM: `limactl stop vigil`.
Mount relocation is documented in [decisions](docs/DECISIONS.md).

On native Ubuntu 24.04, run `./scripts/setup-linux`. For an individual build:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

## Run pods and daemon

In two Linux terminals:

```sh
# Terminal 1: explicit monitoring-only mode
build/vigild --no-persistence --config config/vigil.conf
# Terminal 2: pod 1, temperature + door + heartbeat
.venv/bin/python -m simulator.pod
```

Ctrl-C stops the simulator. At the default 3000 ms freshness deadline, each
source becomes stale, its old value has `current=0`, and communication loss
is raised on the next timer check (default 100 ms tick, plus scheduling delay).
Restart after silence to recover. Temperature high raises at 27 °C and clears
at 26 °C. Flat readings raise suspicion only. SIGUSR1 requests a daemon snapshot;
SIGINT/SIGTERM cleanly stop it. Diagnostics use Linux monotonic milliseconds.

Simulator options include `--pod-id 1 --pod-id 2`, `--cycles 3`, `--interval 0.1`
and `--repeat-each-frame`. Configure pod 2 in `config/vigil.conf` before monitoring
it. Deterministic frame injection, silence, threshold sequences and load controls
live in the scenario harness; they use real SocketCAN.

```sh
candump -L vcan0
build/vigil-decode 101 0098080000b7  # value=2200, counter=0
```

## PostgreSQL and supervised operation

Persistence is enabled by default. An unavailable database raises degraded state
while monitoring continues. Provision the default local peer-authenticated service:

```sh
./scripts/install-service
./scripts/setup-database
./scripts/vcan-up
sudo systemctl enable --now vigild
journalctl -u vigild
```

The installer preserves existing configuration; it does not implicitly start the
service. `config/database.env` documents connection settings. Numeric/Unix endpoints
keep connection deadlines bounded. Schema: `migrations/001_history.sql`.
`--queue-capacity N` sets pending reading capacity (default 1024).
[Persistence documentation](docs/PERSISTENCE.md) includes grants, SQL inspection,
record semantics, timeouts and shutdown behavior.

## Tests, evidence and interview demo

Use an otherwise idle `vcan0`; stop the installed service and other simulators
before tests. Test clusters are disposable and never stop the default/user
PostgreSQL cluster. The systemd gate installs/removes its own temporary unit.
Sudo is needed for network and unit setup; the daemon remains unprivileged.

```sh
./scripts/verify --clean        # complete gate; writes observed docs/VERIFICATION.md
./scripts/check --clean         # C/Python, vcan, PostgreSQL, systemd, sanitizers, Helgrind
./scripts/check-ci --clean      # exact hosted-CI command suite, no vcan/systemd dependency
./scripts/check-release --clean # optimized build and real decoder/monitor/persistence integration
.venv/bin/python tests/fault_scenarios.py --output artifacts/my-fault-run
.venv/bin/python tests/database_contract.py
./scripts/fuzz 120              # libFuzzer + ASan + UBSan; saved corpus/log/crash artifacts
./scripts/demo                 # asserted, narrated interview story with disposable DB
```

Each scenario prints its expectation before execution. Failure exits nonzero;
JSON results, daemon configuration/output, timestamps and counters are retained
in new directories under `artifacts/`. Do not reuse an output directory.
`--scenario NAME` runs one fault case. The demo exports SQL history/checkpoints,
showing healthy → silent/stale → return → database outage → queue drops → backfill
and gap. [Manual CAN checklist](docs/MANUAL_CAN.md) supplements automation.

The complete gate includes 45 cmocka cases in six builds, seven Python unit
cases, ten brief fault scenarios, and five additional hardening scenarios in
Release/ASan/TSan. It includes decoder/daemon integration, SQL assertions, systemd SIGKILL/watchdog
restart, GCC/Clang strict diagnostics, GCC static analysis, ASan/UBSan, TSan,
Helgrind and a 120-second fuzz budget. TSan uses `setarch -R` for its process tree
on the ARM64 VM; global ASLR is unchanged. [Fuzzer contract](fuzz/README.md).

| Evidence boundary | What it checks |
| --- | --- |
| GitHub Actions workflow | GCC/Clang builds; C/Python units; decoder CLI; real PostgreSQL schema/transaction/permission checks; ASan/UBSan; GCC analyzer; 10-second fuzz budget |
| Linux-local gate | Real vcan decoder/monitor/persistence scenarios, database outages, load accounting, systemd, TSan/Helgrind, demo and longer fuzz session |

The workflow does not assume hosted runners can configure vcan. It neither skips
a failed bus test nor substitutes a Python virtual bus. Its commands have been
run locally; **a hosted CI run has not yet been performed**, so no hosted green
badge or CI-proven platform claim is made. Workflow: [.github/workflows/ci.yml](.github/workflows/ci.yml).

## Limits and non-claims

- Queue memory is volatile: N pending readings, 256 events and one immutable
  in-flight batch of at most 32 records. Oldest pending records are evicted and
  counted. The demo's capacity-four/eleven-arrival fixture has no readings in flight.
- Database recovery backfills retained records and writes gaps only for known
  loss. SIGKILL loses unpersisted memory. A lost COMMIT response is ambiguous;
  some unconfirmed data may already be stored. New runs cannot reconstruct lost
  counters/history. There is no disk spool or crash-durable queue.
- vcan load tests measure Linux application behavior. They cannot establish CAN
  arbitration, bitrate limits, electrical behavior, wiring quality or bus-off
  recovery. The burst test disables persistence; it is not a database-throughput claim.
- CRC/counters do not authenticate a sender. Large sequence gaps and old replays
  across wraps are ambiguous. Stable readings are not proof of failed sensors.
- Rate plausibility uses receive time: scheduler batching can create apparent
  spikes and reject otherwise reasonable samples.
- No hard real-time or production-capacity guarantee. Diagnostic regular-file
  output may block; full diagnostic pipes drop counted lines. PostgreSQL/storage
  determine the durability of committed rows. Network deadlines do not bound
  local libpq credential/certificate file stalls or arbitrary scheduler delays.
- No hardware validation, NetBotz/A-Link compatibility, dashboard/API, authenticated
  CAN, on-disk spool or Buildroot extension is included.

Use the [interview guide](docs/INTERVIEW_GUIDE.md) to demonstrate the project and
the [resume evidence](docs/RESUME_EVIDENCE.md) to choose defensible claims.
Study the [learning guide](docs/LEARNING_GUIDE.md), then `src/monitor.c`,
`src/queue.c` and `src/persistence.c`. [Decisions](docs/DECISIONS.md) record brief
refinements. [State](CODEX_STATE.md) records the completed checkpoint and boundary.
Earlier evidence: [1](docs/CHECKPOINT_1_VERIFICATION.md),
[2](docs/CHECKPOINT_2_VERIFICATION.md), [3](docs/CHECKPOINT_3_VERIFICATION.md).
