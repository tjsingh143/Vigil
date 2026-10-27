# Vigil

[![Linux build, contracts and fuzz](https://github.com/tjsingh143/Vigil/actions/workflows/ci.yml/badge.svg)](https://github.com/tjsingh143/Vigil/actions/workflows/ci.yml)

Vigil is a Linux sensor-monitoring daemon written in C11. It reads classic CAN
frames through SocketCAN and judges each configured source on three separate
dimensions: whether data is **fresh**, whether the publisher's counter is
**progressing**, and whether the value is **physically plausible**. Live alarms
stay in memory. History is recorded without letting storage or network I/O stall
the monitoring loop.

Version 2.0.0 makes that history survive process crashes and database outages.
Readings and events are committed to a local SQLite outbox before they are
replayed to PostgreSQL. Each
PostgreSQL transaction stores an exact byte receipt, so a retry after a lost
acknowledgement is recognised and has no second effect. A process crash, a
PostgreSQL outage or an ambiguous commit no longer loses history that was
already committed locally.

| | |
| --- | --- |
| Current release | **2.0.0** (`vigil-v2.0.0`): durable SQLite outbox and verified PostgreSQL replay |
| Previous release | 1.0.0 (`vigil-v1`): monitoring daemon with a bounded in-memory PostgreSQL queue |
| Release qualification | Fresh clean run of the 72-command Linux release gate; independent release audit approved within its documented scope ([summary](docs/RESILIENCE_AUDIT.md)) |
| Hosted CI | GitHub Actions runs the subset that needs no `vcan`, mounts or systemd |
| Not claimed | Physical CAN, power-cut survival, hard real-time behaviour, sustained replay at the 100 readings/s test load |

## Architecture

```text
sensor pods ─► SocketCAN (vcan0) ─► main thread: CAN + timerfd + signalfd poll loop
                                     │ decode and validate every frame
                                     │ freshness / progress / plausibility per source
                                     │ live alarms (memory only), systemd watchdog
                                     ▼ bounded copies under a short queue mutex
                              volatile queue: N readings + 256-event reserve
                                     ▼
                              SQLite owner thread
                                     │ immutable batches, WAL + synchronous=FULL,
                                     │ then parent-directory fsync
                                     ▼
                              durable outbox: 65,536 records / 4,096 batches / 64 MiB
                                     │ oldest batch first, one replay slot
                                     ▼
                              PostgreSQL owner thread (asynchronous libpq)
                                     │ replay_batch(): origin check, history rows and
                                     │ exact byte receipt in one transaction
                                     ▼
                              PostgreSQL ── confirmed result ─► SQLite retires the batch
```

Three rules carry the design:

- **Monitoring never waits on storage.** Only the main thread changes live
  alarms or feeds the watchdog. SQLite, fsync and libpq calls run on two
  dedicated owner threads. Queue and mailbox locks cover bounded memory copies,
  never I/O.
- **Durability has one definition.** A record is durable only after a successful
  SQLite FULL commit followed by a parent-directory fsync. Before that it is in
  RAM. Overflow evicts the oldest pending RAM record and counts the loss. Durable
  records are never evicted to make room; a full outbox stops new admission.
- **Unknown is a real state.** A transport error after submission may hide a
  commit, so the same batch is retried with identical bytes. An equal receipt
  makes the retry a no-op. A mismatched origin, receipt or history row stops
  replay instead of overwriting anything.

| Evidence state | What a crash can do |
| --- | --- |
| Observed, queued or held in RAM | Lose it; some unsaved losses cannot be known |
| Locally durable | Nothing: original identity and payload survive until verified retirement |
| Sent, remote outcome unknown | The same batch stays local and is retried byte-for-byte |
| Committed remotely, not yet retired | The receipt makes the repeated submission safe |
| Retired locally | PostgreSQL holds the history; the local prefix and counters advance atomically |

Recovered history from an earlier run never restores live sensor or alarm
state. A restart is a new run with its own origin identity.

## Monitoring and protocol

Classic CAN with standard 11-bit identifiers; pods 1–127; temperature and door
channels.

| CAN ID | Payload |
| --- | --- |
| `0x100 + pod` | 6 bytes: channel, little-endian int16 value, status, rolling counter, CRC-8 |
| `0x700 + pod` | 7 bytes: state, little-endian uint32 uptime, independent counter, CRC-8 |

- **Integrity**: identifier range, length, encoding and CRC-8/SMBUS are checked
  before any payload field is used; an invalid frame cannot modify a decoded result.
- **Freshness**: usable data must arrive before the configured deadline. A stale
  value is kept for diagnosis but never reported as current, and a heartbeat
  cannot refresh a quiet channel.
- **Progress**: rolling counters must advance. Repeated or jumping counters are
  reported separately from stale data.
- **Plausibility**: range and rate rules per source; high alarms use hysteresis,
  and invalid or stale data cannot clear a high alarm. Flat readings are
  treated as suspicious, not as proof of sensor failure.

CRC and counters detect corruption and some replay or stall conditions; they do
not authenticate senders. Details: [protocol](docs/PROTOCOL.md),
[monitoring policy](docs/MONITORING.md).

## Build and run

Vigil needs real Linux for SocketCAN, `timerfd`, `signalfd` and systemd. On an
Apple Silicon Mac, the repository provisions an Ubuntu 24.04 ARM64 VM with Lima
(2 vCPUs, 2 GiB RAM):

```sh
brew install lima
./scripts/dev up        # create/start the VM and run scripts/setup-linux inside it
./scripts/dev           # shell in the VM at the repository root
```

On Ubuntu 24.04, run `./scripts/setup-linux` directly. It installs the build
and test dependencies, creates `.venv`, brings up `vcan0`, and builds SQLite
3.51.3 under `/opt/vigil-sqlite-3.51.3` after checking the SHA3-256 of the
upstream `sqlite3.c`. The distribution SQLite is left unchanged.

```sh
# Linux commands. From macOS, prefix each with ./scripts/dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

Run the daemon against a new outbox and drive it with the simulator:

```sh
./scripts/setup-outbox /tmp/vigil-state/outbox.sqlite3   # once: empty private directory, >= 2 GiB free
build/vigild --config config/vigil.conf --outbox-path /tmp/vigil-state/outbox.sqlite3
.venv/bin/python -m simulator.pod                         # in a second Linux terminal
```

The outbox must live on native ext4 or XFS. The macOS shared folder, tmpfs and
network filesystems are rejected. Normal startup requires an existing,
validated store and never creates history silently. Send `SIGUSR1` for a status
line and `SIGINT`/`SIGTERM` for a bounded shutdown. `build/vigild --no-persistence`
runs monitoring only, with no store and no worker threads.

PostgreSQL replay reads `VIGIL_DB_HOST`, `VIGIL_DB_PORT`, `VIGIL_DB_NAME` and
`VIGIL_DB_USER` (defaults: local socket, 5432, `vigil`, `vigil`; see
[config/database.env](config/database.env)). While PostgreSQL is unreachable or
lacks migration 002, monitoring continues and history accumulates locally up to
the outbox limits. To install as a hardened systemd service (unprivileged
`vigil` user, `Type=notify`, watchdog):

```sh
./scripts/setup-database     # local cluster: migrations 001 + 002 and least-privilege grants
./scripts/install-service
./scripts/setup-outbox       # provisions /var/lib/vigil/outbox.sqlite3 for the vigil user
sudo systemctl enable --now vigild
```

### Demonstration

```sh
cmake -S . -B build-resilience-release -DCMAKE_BUILD_TYPE=Release -DVIGIL_BENCHMARKS=ON
cmake --build build-resilience-release --parallel 2
./scripts/demo --build build-resilience-release
```

The demo runs the production binary against a disposable outbox and PostgreSQL
cluster. The sequence is: healthy pods, durable local admission, PostgreSQL
stopped, SIGKILL and restart before PostgreSQL returns, the original records
retained, old-run replay, duplicate suppression after an ambiguous commit, and
exact final counters. Raw output is kept under `artifacts/demo-*`.

## Verification

| Command | Environment | Scope |
| --- | --- | --- |
| `./scripts/check-ci --clean` | GitHub Actions, any Linux host | Strict GCC and Clang builds, 67 C test cases per build, Python tests, decoder CLI, PostgreSQL v1 and v2 contracts, ASan/UBSan, GCC `-fanalyzer`, fuzz smoke |
| `./scripts/check-release --clean` | Linux with `vcan` | Optimized build, real `vcan` decoder and monitor tests, local durability and replay regressions |
| `./scripts/check-resilience --clean --output artifacts/<new-dir>` | Linux with sudo, `vcan`, systemd | The 72-command release gate, about 35 minutes |

The release gate adds deterministic SIGKILL cuts at named local and remote
transaction boundaries, byte and identity comparisons against recovered
databases, real ext4 `ENOSPC` and read-only storage, temporary hardened systemd
units, TSan, Helgrind, the production demo and matched benchmarks. Each command's
expectation is recorded before it runs. `gate.json` records exact argv, exit
status, timing and a SHA-256 digest of the implementation sources.

Fresh clean run for this release (2026-10-27, Ubuntu 24.04 ARM64, Linux 6.8,
SQLite 3.51.3, PostgreSQL 16):

- 72 of 72 commands exited 0 (1,997 s).
- 67 C test cases in each of six `ctest` runs across Debug, Release, ASan/UBSan
  and TSan builds (402), plus 7 under Helgrind. The Python suite ran 19 tests
  per discovery run.
- 120 recovery-coordinator cases across Debug, Release, ASan and TSan, including
  64 SIGKILL cuts. Adversarial fault schedules ran 75 times.
- A sustained backlog of 6,128 readings in 189 batches survived restart and drained.
- Receive-overload accounting: 49,587 received + 414 kernel drops = 50,001 sent.
- libFuzzer: 85.7 million decoder executions in 121 s with no finding.
- An independent reconciliation read 1,339 SQLite inventories and 727
  PostgreSQL exports with zero failures.

The complete gate output is attached to the
[2.0.0 release](https://github.com/tjsingh143/Vigil/releases/tag/vigil-v2.0.0).
[Verification guide](docs/VERIFICATION.md) ·
[fault tests](docs/RESILIENCE_FAULT_TESTS.md) ·
[audit summary](docs/RESILIENCE_AUDIT.md)

## Measured cost

Matched benchmark, 1.0 and 2.0 alternating on the same VM, three 60-second
trials each at 100 readings/s. Medians are from the 2026-10-27 requalification
gate, which ran the same production binary as the release:

| | 1.0 | 2.0 |
| --- | ---: | ---: |
| Monitoring throughput, readings/s | 100.00 | 100.00 |
| Local durable admission, readings/s | n/a | 99.99 |
| Remote replay window, readings/s | 100.00 | 74.21 |
| Drain of a 600-reading backlog, s | 0.90 | 8.87 |
| Daemon CPU, % of one core | 2.55 | 19.31 |
| Peak sampled RSS, MiB | 7.77 | 11.68 |
| Alarm lateness beyond the 500 ms deadline, ms | 8 | 6 |

Monitoring and local durability keep up with the offered load, while
PostgreSQL replay does not. Validation, exact receipts and transactional
retirement cost throughput. Under sustained load the backlog grows and then
drains once load stops. The independent rerun measured a 73.7 readings/s
replay median, and earlier runs of the same binary measured up to 83.9. These
are finite runs on a shared 2-vCPU VM, not capacity guarantees.
[Performance report](docs/RESILIENCE_PERFORMANCE.md).

## Limitations

- `vcan` has no bit timing, arbitration, bus-off or electrical behaviour. No
  physical CAN hardware was tested.
- Crash testing uses SIGKILL. No power cut, host reboot during an outage, media
  failure or XFS run was executed. Durability assumes storage that honours flush
  ordering.
- The outbox is finite. A long outage at an offered load above the replay rate
  eventually stops admission. [RESILIENCE_RELEASE.md](docs/RESILIENCE_RELEASE.md)
  gives the capacity arithmetic.
- The SQLite installer verifies `sqlite3.c`, not the whole archive or its build
  scripts, and trusts the invoking shell and Python environment.
- CRC and counters do not authenticate senders. There is no authenticated CAN,
  API or dashboard, remote failover, or PostgreSQL retention policy.
- Not a hard real-time system. Receive-time rate checks can reject samples
  batched by scheduler stalls.
- Hosted CI covers only the subset without `vcan`, mounts and systemd. The full
  release gate runs on a local Linux VM.

## Documentation

The [documentation index](docs/README.md) groups everything. Main entry points:

- [Outbox and replay](docs/OUTBOX.md): operation, bounds, status counters, error classes
- [Failure contract](docs/RESILIENCE_CONTRACT.md) and [architecture](docs/RESILIENCE_ARCHITECTURE.md)
- [Release, upgrade and rollback](docs/RESILIENCE_RELEASE.md)
- [Code excerpts](docs/CODE_SAMPLES.md): pinned, verbatim source with links

## Repository layout

| Path | Contents |
| --- | --- |
| `src/`, `include/vigil/` | Daemon: protocol, monitor, queue, SQLite outbox, PostgreSQL replay, config |
| `migrations/` | PostgreSQL schema: `001_history.sql` (1.0) and additive `002_resilience.sql` |
| `tests/` | cmocka units, Python integration, fault, replay, adversarial and benchmark harnesses |
| `scripts/` | Setup, gates, demo, benchmark, fuzz and service helpers |
| `simulator/` | Python CAN sensor pods |
| `fuzz/`, `tools/` | libFuzzer decoder target, decoder CLI, outbox initializer |
| `systemd/`, `config/`, `env/` | Service unit, example configuration, Lima VM definition |

## History

[CHANGELOG.md](CHANGELOG.md) lists the changes in each release. 1.0.0 was
released on 2026-09-28. 2.0.0 went through a baseline audit, four implementation
stages, two independent audits, remediation of the five findings they raised,
and a final independent audit. The [remediation records](docs/README.md#historical-records)
keep that sequence.

The [1.0 review material](https://drive.google.com/drive/folders/1kzVJFzVXg2qrh3Uyq7s1tRkmlH5Cwl3t)
(overview, testing notes, demo transcript) describes the 1.0.0 snapshot and its
measurements:
[review entry point](https://drive.google.com/file/d/1y-OnH_hf6DHMCPmKEhJKxzw-7EbS-kI3/view?usp=drivesdk) ·
[overview](https://drive.google.com/file/d/1tEWgz8IWl19ZkqSTEGRgY-Vz6tb-LFFO/view?usp=drivesdk) ·
[testing](https://drive.google.com/file/d/1LA_r6cO71KDWW-JSgDHIpf1H1c-O2pvk/view?usp=drivesdk) ·
[code samples](https://drive.google.com/file/d/1xRuJ0U_Lqk9hvBxtRnMI7BvbeReI3dP-/view?usp=drivesdk)
