# Vigil

Vigil detects stale or invalid sensor data and is intended to keep monitoring
when its sources or database fail. **Checkpoint 1 is implemented:** protocol,
simulated pods, validated C decoding, and real Linux CAN integration. The
monitoring daemon and database behavior are later checkpoints.

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
Simulator and decoder run as the normal user. No Docker, PostgreSQL, or libpq
is installed for this checkpoint.

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

It rebuilds Debug and ASan/UBSan variants with warnings as errors, runs cmocka
and Python tests, sets up `vcan0`, then runs both decoder builds against real
simulators and malformed traffic. Any failure exits nonzero. No integration
test is silently skipped. Evidence is in [docs/VERIFICATION.md](docs/VERIFICATION.md).

Individual commands inside Linux:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
.venv/bin/python -m unittest discover -s tests -p 'test_*.py' -v
.venv/bin/python tests/integration.py --decoder build/vigil-decode
```

Use `-DVIGIL_SANITIZERS=ON` in a separate build directory for sanitizers.
cmocka is the only C test dependency; Python tests use the standard library.
CI and the brief's libFuzzer evidence target are deferred to a later evidence
checkpoint. Real bus integration currently runs locally in this Ubuntu VM.

## Map

| Path | Purpose |
| --- | --- |
| `include/vigil/`, `src/` | Pure C protocol and counter primitives |
| `tools/` | Linux C decoder diagnostic |
| `simulator/` | Python encoder, deterministic pods, pinned dependencies |
| `tests/` | cmocka, Python, shared literal vectors, Linux integration |
| `env/`, `scripts/` | Reproducible Ubuntu VM, setup and verification |
| `docs/PROTOCOL.md` | Exact wire contract |
| `docs/DECISIONS.md` | Decisions and brief interpretation |
| `docs/LEARNING_GUIDE.md` | Plain-English implementation explanation |
| `CODEX_STATE.md` | Current state and exact next checkpoint |

CRC/counters do not authenticate senders. vcan proves Linux application
behavior; it cannot model arbitration, bitrate limits, wiring or electrical
faults. No hardware, throughput, daemon, database or durability claim is made.
