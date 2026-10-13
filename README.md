# Vigil

[![Linux build, contracts and fuzz](https://github.com/tjsingh143/Vigil/actions/workflows/ci.yml/badge.svg)](https://github.com/tjsingh143/Vigil/actions/workflows/ci.yml)

Vigil is a Linux sensor-monitoring daemon written in C. It reads classic CAN frames, tracks whether each configured source is fresh, progressing, and physically plausible, maintains live alarms in memory, and records bounded history in PostgreSQL without making monitoring wait on the database.

## Architecture

```text
sensor pods → SocketCAN/vcan → monitoring thread
                              │
                              ├─ decode + validate frames
                              ├─ freshness/progress/plausibility state
                              ├─ live alarm state
                              └─ bounded queue
                                      │
                                      ▼
                               persistence thread
                                      │
                                      ▼
                                  PostgreSQL
```

The monitoring thread owns live state and never performs database I/O. A separate writer thread owns the PostgreSQL connection and persists copied records from a bounded queue. If PostgreSQL is unavailable, monitoring continues; retained history is bounded and dropped records are counted explicitly. Recovery backfills retained records and records a gap when known history was lost.

Technical details are in [monitoring](docs/MONITORING.md), [persistence](docs/PERSISTENCE.md), and the [wire protocol](docs/PROTOCOL.md).

## Protocol

Vigil uses classic CAN with standard 11-bit identifiers. Sensor frames carry a channel, signed measurement, status byte, rolling counter, and CRC-8. Heartbeats carry source state, uptime, an independent counter, and CRC-8.

Validation separates several failure classes instead of treating every bad reading the same way:

- **freshness** — usable data must arrive before the configured deadline;
- **progress** — rolling counters must advance rather than repeat indefinitely;
- **plausibility** — values must remain within the configured source rules;
- **integrity** — frame length, identifier range, encoding, and CRC must be valid.

A stale last-known value can remain available for diagnosis but is marked non-current and is not treated as live data.

## Build and run

The Linux integration path uses SocketCAN, `timerfd`, `signalfd`, PostgreSQL, pthreads, and systemd.
Build prerequisites include CMake, a C compiler, pkg-config, and development
libraries for PostgreSQL, systemd, and cmocka. `./scripts/setup-linux` installs
the development environment on the supported Debian/Ubuntu path.
The [verification guide](docs/VERIFICATION.md) separates hosted and local gates.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

For the complete Linux development environment:

```sh
./scripts/setup-linux
./scripts/vcan-up
```

A monitoring-only daemon can be started with:

```sh
build/vigild --no-persistence --config config/vigil.conf
```

The simulator can then publish deterministic sensor traffic through SocketCAN:

```sh
.venv/bin/python -m simulator.pod
```

Persistence is enabled by default when database configuration is present. The service helpers provision a local PostgreSQL schema and an unprivileged systemd unit:

```sh
./scripts/setup-database
./scripts/install-service
sudo systemctl enable --now vigild
journalctl -u vigild
```

## Verification

The project includes unit, integration, fault-injection, concurrency, database-contract, sanitizer, static-analysis, and fuzzing checks.

```sh
./scripts/verify --clean
./scripts/check --clean
./scripts/check-ci --clean
./scripts/check-release --clean
.venv/bin/python tests/fault_scenarios.py --output artifacts/fault-run
.venv/bin/python tests/database_contract.py
./scripts/fuzz 120
```

The release verification includes:

- 45 cmocka cases across six build configurations;
- 7 Python unit cases;
- 10 fault scenarios plus 5 hardening scenarios;
- 5 PostgreSQL contract checks;
- real `vcan` decoder, monitor, and persistence scenarios;
- systemd readiness, watchdog, restart, and failure behavior;
- GCC and Clang strict builds;
- GCC static analysis;
- ASan/UBSan, TSan, and Helgrind checks;
- libFuzzer coverage of the decoder.

The hosted GitHub Actions workflow runs the non-`vcan` subset: builds, C/Python tests, decoder CLI checks, PostgreSQL contracts, ASan/UBSan, GCC analysis, and a short fuzz budget. The release commit passed that hosted Linux workflow. Bus-level `vcan`, systemd supervision, TSan/Helgrind, and hardware-dependent behavior remain local-only gates.

The manual CAN checklist in [docs/MANUAL_CAN.md](docs/MANUAL_CAN.md) provides direct `can-utils` commands for inspecting frames and verifying decoder behavior outside the automated harness.

## Persistence behavior

The queue is intentionally bounded. When a database outage lasts longer than available queue capacity, the oldest pending history is evicted and the loss is counted rather than allowing memory growth to become unbounded. An immutable in-flight batch prevents the writer from silently replacing a transaction whose commit result is still uncertain.

Writes use stable run/batch identities so reconnect and retry behavior can distinguish known retries from new history. PostgreSQL remains a historical store; live alarm state does not depend on a successful query.

## Failure model

Vigil is designed to make common monitoring failures visible:

- a source stops transmitting;
- a publisher repeats old counters;
- a measurement becomes implausible;
- malformed or corrupt frames arrive;
- PostgreSQL becomes unavailable;
- the persistence queue fills;
- the service crashes or stops making watchdog progress.

The daemon reports these conditions without turning historical storage into the authority for current sensor health.

## Limits

- Local bus integration uses `vcan`; it does not establish physical CAN timing, arbitration, wiring quality, bus-off recovery, or electrical behavior.
- The in-memory persistence queue is bounded but not crash-durable. A process crash can lose records that have not reached PostgreSQL.
- CRC and rolling counters detect corruption and some replay/stall conditions, but they do not authenticate senders.
- Large sequence gaps and wrap-around cases are inherently ambiguous without a stronger protocol.
- Receive-time plausibility rules can be affected by scheduler batching.
- The system is not a hard real-time implementation and does not claim production-capacity limits from local benchmarks.
- No dashboard/API, authenticated CAN, disk spool, or hardware-board validation is included in this release.

## Code excerpts

[Verified code excerpts](docs/CODE_SAMPLES.md) provide direct source links for the main implementation boundaries.

## Review material

[Project portfolio](https://drive.google.com/drive/folders/1kzVJFzVXg2qrh3Uyq7s1tRkmlH5Cwl3t) contains the project overview, architecture,
code examples, testing notes, and supporting evidence. Portfolio measurements
retain their stated snapshot and verification boundaries.

[Review entry point](https://drive.google.com/file/d/1y-OnH_hf6DHMCPmKEhJKxzw-7EbS-kI3/view?usp=drivesdk) · [Overview](https://drive.google.com/file/d/1tEWgz8IWl19ZkqSTEGRgY-Vz6tb-LFFO/view?usp=drivesdk) · [Testing](https://drive.google.com/file/d/1LA_r6cO71KDWW-JSgDHIpf1H1c-O2pvk/view?usp=drivesdk) · [Code samples](https://drive.google.com/file/d/1xRuJ0U_Lqk9hvBxtRnMI7BvbeReI3dP-/view?usp=drivesdk)
