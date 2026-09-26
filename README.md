# Vigil

Vigil detects stale or invalid sensor data and keeps monitoring through database
outages. **Checkpoint 3 is implemented:** Linux C monitoring, simulated CAN pods,
in-memory alarms, a bounded queue and dedicated PostgreSQL writer, explicit
history gaps, and an unprivileged systemd notify/watchdog service. Current
alarms belong to monitoring memory; PostgreSQL stores history. The queue is
volatile and cannot preserve unpersisted data through a daemon crash.

`PROJECT_BRIEF.pdf` version 3 is the specification. This is an original small
protocol inspired by sensor-pod monitoring, not an implementation of A-Link.

## Start development

On an Apple Silicon Mac with Homebrew:

```sh
brew install lima
./scripts/dev up
./scripts/dev ./scripts/check --clean
```

`up` creates/starts a dedicated VM named `vigil`: Ubuntu 24.04 ARM64, 2 CPUs,
2 GiB RAM, 12 GiB sparse disk. It shares **only this repository** read/write,
installs the C/Python development dependencies, creates a Linux `.venv`, and
brings up `vcan0`. Initial setup needs network access and several minutes.
The image is pinned by dated URL and SHA-256. Ubuntu package security updates
remain enabled; this is reproducible setup, not a bit-identical OS snapshot.

`./scripts/dev` opens a Linux shell in the repository. Prefix any Linux command
with `./scripts/dev` from macOS. The shared `.venv` and build outputs are Linux
files; do not use them with macOS Python or tools. Lima must remain in a user
session (no root invocation). The VM is intentionally left running after checks.

On native Ubuntu 24.04, run `./scripts/setup-linux`, then `./scripts/check --clean`.
Setup uses sudo only for package installation and interface administration.
Simulator and decoder run as the normal user. Setup now installs PostgreSQL,
libpq, libsystemd and Valgrind too. Database integration uses disposable clusters
and does not stop the default PostgreSQL cluster or any user database.

After a VM reboot, run `./scripts/dev ./scripts/vcan-up` (or `./scripts/dev up`).
Stop the VM with `limactl stop vigil`; start it again with `./scripts/dev up`.
If the repository moves, its Lima mount needs updating while the VM is stopped;
see [environment decisions](docs/DECISIONS.md). Do not delete an existing VM
with valuable files to repair a mount.

## Run the simulator and observe it

Use separate terminals, starting observers before the finite simulator:

```sh
# Terminal 1: independent can-utils observer (Ctrl-C to stop)
./scripts/dev candump -L vcan0

# Terminal 2: C diagnostic decoder, 18 frames, 10-second overall deadline
./scripts/dev build/vigil-decode --listen vcan0 18

# Terminal 3: two pods, three cycles, six messages each cycle
./scripts/dev .venv/bin/python -m simulator.pod \
  --pod-id 1 --pod-id 2 --interval 0.1 --cycles 3
```

Omit `--cycles` for continuous operation; SIGINT/SIGTERM shut it down cleanly.
Default: pod 1, one cycle/second. Each cycle emits temperature, door, heartbeat
for each configured pod. Values follow a deterministic pattern. Heartbeat
uptime measures actual monotonic elapsed seconds.

Add `--repeat-each-frame` to transmit each frame twice for deliberate counter
fault injection. Counters still advance normally between original frames;
the daemon rejects and counts each immediate repeat.

## Run the monitoring daemon

```sh
# Terminal 1: foreground daemon, default pod 1, 3-second freshness timeouts
./scripts/dev build/vigild --no-persistence

# Terminal 2: default pod 1; Ctrl-C stops it and demonstrates communication loss
./scripts/dev .venv/bin/python -m simulator.pod
```

The daemon emits alarm transitions and periodic readable status snapshots.
Stopping the pod makes its two readings and heartbeat stale; old values remain
visible with `current=0`. Restarting after the timeout recovers fresh values
and clears communication alarms. Stop the daemon with Ctrl-C. To request an
extra snapshot, send SIGUSR1 to the Linux PID printed in its readiness line
(`./scripts/dev kill -USR1 1234`, replacing 1234 with that PID).

Use `--config config/vigil.conf` for explicit settings. Its comments show how
to enable pod 2 before starting a two-pod simulator. Default temperature
alarms raise at 27 C and clear at 26 C; door alarms raise on open and clear on
closed. Ranges, rates, flatness windows and timeouts are per source.
[Monitoring contract](docs/MONITORING.md) explains configuration, alarm
lifecycle, reset policy, timestamps, counters and diagnostic output limits.

`vigild` exits 0 after SIGINT/SIGTERM and 2 on usage/setup/I/O failure. It runs
in the foreground as an unprivileged user. The example above explicitly disables
history for a quick monitoring-only demonstration. By default persistence is
enabled; an unavailable/unconfigured database raises persistence-degraded while
monitoring continues. For the complete local installation:

```sh
./scripts/dev ./scripts/install-service
./scripts/dev ./scripts/setup-database
./scripts/dev ./scripts/vcan-up
./scripts/dev sudo systemctl enable --now vigild
./scripts/dev journalctl -u vigild
```

Installation preserves existing configuration and runs the service as `vigil`;
the default local database uses peer authentication for that account. CAN setup
is a separate privileged step. `config/database.env` shows connection settings;
`--queue-capacity N` changes the default 1024 pending-reading capacity.
[Persistence contract](docs/PERSISTENCE.md) explains schema, exact counters,
oldest-reading drops, the separate event reserve/in-flight batch, deadlines,
gap markers, crash limits and service operation. Stop the installed service
before running the isolated regression suite on the same `vcan0`.

Decode a literal frame without a CAN interface:

```sh
./scripts/dev build/vigil-decode 101 0098080000b7
# result=ok, pod=1, temperature channel=0, value=2200 (22.00 C), counter=0
```

The probe prints JSON lines; exit 0 means all decoded frames passed, 1 means
at least one protocol rejection, 2 means usage/I/O/timeout error. It is a
bounded diagnostic tool, not `vigild`. It does not apply counter state,
freshness, physical limits or alarms. Live sockets accept classic CAN only;
FD and error frames are not subscribed to. The pure decoder explicitly rejects
FD/error/RTR/extended formats when supplied, and live RTR/EFF are tested.

## Build and test

The complete local gate requires an otherwise idle `vcan0`:

```sh
./scripts/dev ./scripts/check --clean
```

It clean-builds strict Debug, ASan/UBSan and TSan variants, runs 45 cmocka cases
per build and six Python tests, then real decoder/monitor/persistence integration.
Checkpoint 1/2 gates retain their original checks. PostgreSQL gates inspect SQL
directly for normal writes, outage/backfill, exact overflow, lost responses,
current alarms, timeouts and crash loss. A real systemd service proves readiness,
unprivileged operation, watchdog and SIGKILL restarts. The gate needs sudo for
vcan and test-service setup; test DBs run as the invoking user in disposable
clusters. No integration is silently skipped.

GCC TSan on this ARM64 VM needs ASLR disabled **only for its test process tree**
using `setarch -R`; the script does this without changing global settings. It
also runs Helgrind on the concurrent queue tests. Evidence:
[Checkpoint 1](docs/VERIFICATION.md), [Checkpoint 2](docs/CHECKPOINT_2_VERIFICATION.md),
[Checkpoint 3](docs/CHECKPOINT_3_VERIFICATION.md).

Individual commands inside Linux:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
.venv/bin/python -m unittest discover -s tests -p 'test_*.py' -v
.venv/bin/python tests/integration.py --decoder build/vigil-decode
.venv/bin/python tests/monitor_integration.py --daemon build/vigild
```

Use `-DVIGIL_SANITIZERS=ON` in a separate build directory for sanitizers.
The C build requires cmocka, libpq, pthreads and libsystemd; the integration
harness uses Python's standard library plus the existing python-can simulator.
CI and the brief's libFuzzer evidence target are deferred to a later evidence
checkpoint. Real bus integration currently runs locally in this Ubuntu VM.

## Map

| Path | Purpose |
| --- | --- |
| `include/vigil/`, `src/` | C protocol, monitoring core, startup config and Linux daemon |
| `config/` | Example daemon settings matching built-in defaults |
| `tools/` | Linux C decoder diagnostic |
| `simulator/` | Python encoder, deterministic pods, pinned dependencies |
| `tests/` | cmocka, Python, shared literal vectors, Linux integration |
| `env/`, `scripts/` | Reproducible Ubuntu VM, setup and verification |
| `docs/PROTOCOL.md` | Exact wire contract |
| `docs/MONITORING.md` | Runtime policy, configuration and status contract |
| `docs/PERSISTENCE.md`, `migrations/` | Database history, queue/outage contract and schema |
| `systemd/` | Unprivileged notify/watchdog service |
| `docs/DECISIONS.md` | Decisions and brief interpretation |
| `docs/LEARNING_GUIDE.md` | Plain-English implementation explanation |
| `CODEX_STATE.md` | Current state and exact next checkpoint |

CRC/counters do not authenticate senders. vcan proves Linux application
behavior; it cannot model arbitration, bitrate limits, wiring or electrical
faults. No hardware, load-capacity or crash-durable queue claim is made. Current
alarms and counters exist in daemon memory; database records are historical
evidence. No on-disk spool, API, dashboard, Buildroot or Checkpoint 4 work is added.
