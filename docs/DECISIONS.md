# Engineering decisions

## Scope and authority

Read the complete eight-page version 3 brief before editing. The repository
initially contained only that PDF. Checkpoint 1 builds the foundation; the
owner's combined message also includes a Checkpoint 2 specification but says
explicitly to stop after Checkpoint 1. A scope clarification was requested;
absent a reply, honor that stopping boundary. No daemon core or database is
implemented here. The exact next task is Checkpoint 2.

The brief asks the project owner to write and understand the core before
AI review (section 7). The current explicit instruction assigns implementation
to Codex instead. This documented process deviation follows the owner’s
instruction; the learning guide supports understanding but does not substitute
for the owner reading and explaining the code. Development uses one Codex agent.

## Linux environment

Host inspection found Apple Silicon ARM64, macOS 26.5.1, 8 GiB RAM. Lima with
Apple Virtualization.framework runs a real Ubuntu ARM64 kernel. Two CPUs and
2 GiB RAM leave room for host tools; a 12 GiB sparse disk suffices for this
checkpoint. No emulation, Docker daemon or unnecessary container tooling.
The image URL is dated and SHA-256 checked by Lima. Only the repository is
mounted read/write, avoiding a writable mount of the full host home directory.

The Ubuntu image lacked `vcan`; setup installs the exact running kernel's
`linux-modules-extra` package only if needed. `vcan-up` is idempotent and checks
that an existing `vcan0` really is a virtual CAN interface. Interface setup is
explicit after reboot; no project systemd service is installed at this stage.
The guest already uses systemd as PID 1.

APT dependencies are build-essential, CMake, pkg-config, cmocka, Python venv,
can-utils, iproute2 and kmod, plus the required kernel module package. The
Python runtime and its transitive dependencies are pinned. libpq/PostgreSQL
installation is deferred until used. Normal-user SocketCAN operation was
verified; sudo is limited to provisioning/network administration.

Moving the repository: stop `vigil`, use `limactl edit --mount-only
"/absolute/new/repository:w" vigil`, then restart it from the new path. This
mount-edit procedure is documented from Lima's CLI, not exercised in the
checkpoint verification. Existing unrelated instances are not modified.

Sources: [Lima VZ](https://lima-vm.io/docs/config/vmtype/vz/),
[Lima start options](https://lima-vm.io/docs/reference/limactl_start/),
[Lima edit options](https://lima-vm.io/docs/reference/limactl_edit/),
[Linux SocketCAN](https://docs.kernel.org/networking/can.html).

## Protocol choices the brief leaves open

See `PROTOCOL.md` for the normative wire contract. We choose pod IDs 1..127,
channels 0/1, little-endian signed int16, exact lengths (6-byte reading and
7-byte heartbeat), explicit status/state enums, and CRC-8/SMBUS. Heartbeat
includes its own counter and CRC in addition to the brief's state/uptime.
This fits classic CAN's 8-byte limit and avoids special-case unprotected
heartbeat progress. Payload CRC excludes CAN ID; ID corruption/reassignment
is consequently outside its protection. CRC parameters have a widely used
`123456789` check value of F4 and literal full-frame fixtures.

Reading counters are shared across channels because both readings share one
CAN ID. Each heartbeat ID has a separate counter. The helper accepts forward
distances 1..127, counts distance minus one as inferred missing frames, rejects
repeats, and rejects the other half of the sequence space as old/ambiguous.
This distinguishes small delayed/replayed sequences from progress while
making the unavoidable 8-bit ambiguity explicit. It is not authentication.
The future monitor must establish/reset baselines deliberately and document
restart/timeout recovery; this checkpoint does not invent that policy.

## Separation and error handling

The C protocol library has no Linux headers, I/O, heap allocation, clocks,
threading or global mutable state. The Linux probe owns its CAN socket and
closes it on all handled exits. It uses a bounded poll timeout so integration
failures do not hang. Its JSON output makes exact comparison straightforward;
this is a test/diagnostic adapter, not an early monitoring daemon.

The decoder validates exact length before indexing payloads, rejects unsupported
encodings, and constructs the result locally before assigning the caller's
output. Errors leave output unchanged. Signed decoding subtracts 65536 in
int32 before narrowing a representable value, avoiding implementation-defined
unsigned-to-signed conversion. No struct casts of wire bytes or unaligned reads.

`STATUS_FAULT` is accepted by the syntax decoder and retained for the future
monitor to mark the measurement invalid. Temperature's wire range is separate
from physical plausibility. Door values remain binary even under fault status.

The simulator separates pure encoding, deterministic sample generation and
SocketCAN lifecycle. Later fault scenarios can change emission or supplied
frames without replacing the transport. Each cycle has fixed ordering; the
next cycle waits the configured interval after sending. There is no burst
catch-up after a scheduling stall. Uptime uses real monotonic elapsed seconds;
payload samples depend on cycle index rather than randomness.

Sources: [python-can SocketCAN](https://python-can.readthedocs.io/en/stable/interfaces/socketcan.html),
[cmocka](https://cmocka.org/).

## Evidence and future architecture

Literal shared fixtures constrain both implementations independently of their
algorithms. An independent polynomial-long-division calculation produced the
fixture CRCs. The conventional check string anchors the CRC parameters. Tests
exhaust ID, int16, invalid enum, and counter-baseline domains and verify all
single-bit corruptions of representative complete frames. ASan/UBSan run on
unit tests and the actual Linux decoder/SocketCAN adapter. This is finite
evidence, not a fuzzing or proof-of-correctness claim.

The next checkpoint will use one main poll loop over CAN, timerfd and signalfd.
Freshness, progress and plausibility will be separate state. Alarm state lives
in monitoring memory. Checkpoint 3 will add libpq ownership in a persistence
thread with a bounded queue and no I/O while its mutex is held. Those are
architectural constraints from the brief, not implemented capabilities here.

No global Git identity was configured. The initial checkpoint commit uses the
explicit per-command author `Codex <codex@localhost>`; user Git settings are
unchanged. No remote is configured or contacted for publishing.
