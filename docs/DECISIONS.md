# Engineering decisions

Checkpoint 1 entries below are historical. The final section records the
authorized Checkpoint 2 implementation; [MONITORING.md](MONITORING.md) describes
the current monitoring policy.

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

## Checkpoint 2 — monitoring core (2026-09-25)

The owner explicitly authorized completing and locally committing Checkpoint
2 using one Codex agent. The complete version 3 PDF and required records were
reread before edits; the previous clean Linux gate passed. The section-7
process deviation (agent implements; owner learns and reviews) remains
explicit. No subagents or other Codex instances were used.

The brief's main-thread architecture is implemented literally: nonblocking
classic CAN socket, monotonic periodic timerfd, blocked signals delivered
through signalfd, one poll loop. Signals precede CAN batches; each batch caps
at 64 reads to bound starvation. No event framework, sleeping poll, worker
thread or database dependency. The daemon runs in the foreground; systemd
notify/watchdog supervision remains later work as recorded at Checkpoint 1.

`monitor.c` owns state and accepts monotonic milliseconds. Its only callback
is a synchronous bounded alarm-transition notification; the Linux adapter
emits diagnostics. The core allocates no heap, does no I/O and reads no clocks.
Fixed arrays cap state at 16 configured pods; configuration is parsed once
and copied by value. Each chosen ID has one or two channels. Defaults match
simulator pod 1. Invalid configuration fails startup without silent fallback.

Freshness is per channel and separately per heartbeat. No valid data by the
deadline raises communication loss, which may coexist with invalid readings
or sequence anomalies. This is not a wiring diagnosis. Fault/booting heartbeat
means fresh communication with non-operational pod state. `current` derives
from the channel's value, freshness and measurement validity; it is not an
overall pod-health boolean.

Reset sequence baselines after a full interval with no decoded stream traffic,
not merely no accepted values. This permits recovery from silence without
periodically accepting an endless repeated frame. Uptime/boot state cannot
reset a stream. Reading silence uses the longest channel timeout. Both channels
share sequence accounting even if one is excluded. Physical/status rejection
still advances an accepted sequence. Fast-restart/replay ambiguity remains
explicit; counters and CRC provide no authentication.

Physical checks use integer wire units. Rate compares with the last valid
value while fresh; after a stale gap, range-checked data starts a new slope
reference. Flatness requires equal accepted readings over a configured window,
not elapsed silence. Its warning neither invalidates data nor proves failure.
Door defaults disable rate and flat checks.

Thresholds raise at >= high and clear at <= clear, with clear < high. These
inclusive boundaries refine the brief's illustrative above-27/below-26 example
and support binary door values. Stale/invalid data provides no clearing
evidence for high alarms. Communication, sequence, invalid measurement, flat
suspicion, high value and pod state are distinct categories in monitoring memory.

Readable snapshots and transitions expose exact counters and monotonic times.
Bounded nonblocking diagnostic writes avoid a full pipe stalling monitoring;
dropped lines are counted and later snapshots recover current state. Regular
file output can still block; no general logging framework is added. Kernel
receive drops remain separate from inferred losses. No wire behavior changed,
so protocol version 1 and all shared literal vectors are unchanged.

Checkpoint 3 is the next core work: persistence thread, bounded queue,
PostgreSQL history and outage behavior. It must own libpq/database calls,
never hold queue locks across I/O, and treat monitoring memory as current
alarm authority. None of those persistence features are implemented here.

## Checkpoint 3 — persistence and service (2026-09-25/26)

The owner authorized full implementation, real outage evidence, documentation
and a local commit. One Codex agent performed the work; no delegation or other
Codex instance. Before code changes, all eight pages of version 3, AGENTS.md,
state, decisions and learning guide were reread. Clean starting commit was
`aa785cb`; the entire clean Checkpoint 2 gate and a short vcan/simulator smoke
passed. The existing section-7 process deviation (agent implementation in
place of owner-first coding) remains explicit.

Main owns all current alarms, including persistence-degraded. The writer owns
libpq. Shared data is copied by value under a mutex. Queue locks never cover
JSON serialization, diagnostics, PostgreSQL or network I/O. Condition waits
recheck predicates under that same mutex to avoid losing a publication wakeup.
Signals are blocked before creating the writer; shutdown joins it before
restoring masks/destructing the queue. The monitor core and wire protocol are
unchanged apart from naming the added daemon-level alarm category.

Accepted readings and events have independent FIFO reserves. The configurable
reading lane defaults to 1024; the fixed event lane has 256 slots. Full lanes
evict their oldest pending records, with separate exact counters. Keeping an
event reserve avoids alarm chatter stealing reading capacity. Event overflow
is explicitly possible and counted; current alarm snapshots remain separately
bounded and recover the latest state, not every lost historical transition.

Two necessary brief refinements are explicit in [PERSISTENCE.md](PERSISTENCE.md):

- The writer retains one immutable submitted batch of at most 32 records
  **in addition to** the pending reading/event queue capacities. A submitted
  transaction may have committed despite a lost response; treating it as an
  evictable pending record would misreport data loss. The exact bound is
  N+256+32 outstanding records, plus fixed snapshot/serialization storage.
  Drop-oldest applies to pending, unsubmitted records. Status distinguishes
  pending capacity from the in-flight count. At a settled outage without
  readings in flight, N+K arrivals retain exactly N and lose exactly K.
- Instead of unbounded blocking libpq waits inside the writer, asynchronous
  libpq calls have a two-second client deadline and 50 ms shutdown checks.
  `connect_timeout=2` is supplied and its intent implemented explicitly because
  PQconnectPoll ignores that parameter. Server statement/lock timeouts are
  1000/500 ms. Numeric/Unix endpoints prevent hostname lookup from bypassing
  the connection bound. No PostgreSQL calls move into the main poll loop.

Schema v1 has run identities, typed history metadata with small JSON detail,
and atomic batch checkpoints with gaps/health/alarm snapshots. A reproducible
SQL initialization script and provisioning grants keep DDL with the database
owner. `write_batch` is an invoker function, not a privileged service. UUID and
batch IDs make retries idempotent; the exact original payload survives retry.
Records-written counts confirmed history records, not internal checkpoint rows.
UTC timestamps support external history; monotonic timestamps remain the only
basis for freshness, outage duration and retry/drain deadlines.

Persistence-degraded also raises during connection/query failure and initial
unconnected startup, not only when overflow first loses data. This stronger
warning is useful before loss. Every newly observed drop raises it even if writer acknowledgement won the
scheduling race. It clears after backlog, drop counts and the
published alarm revision are acknowledged. Monitoring never reads SQL to
restore current state. Recovery/gap snapshots include cleared and active
alarms and lifecycle counters; a new process starts unknown with a new UUID.

Shutdown has a one-second best-effort drain budget and reports any abandoned
memory. No spool, crash recovery, invented previous-run loss count, or claim
that a memory queue is durable. A lost response can leave unconfirmed rows
already committed; SQL contents settle what survived. The killed-outage test
uses a known stopped database and no readings in flight to make its exact
three-record loss unambiguous.

systemd is implemented with libsystemd sd_notify/sd_watchdog_enabled. Readiness
means monitoring is ready, independent of database availability. Watchdog is
fed only by the main loop. User/group vigil has no capabilities; CAN setup is
separate. The service orders after PostgreSQL but is not coupled to its liveness.
Read-only system/home/kernel protections and private devices/tmp are retained
in the actual service tests. Installers preserve owner configuration and do not
enable/start the service implicitly. No automatic physical network setup.

PostgreSQL 16.15 and systemd 255 in the actual Ubuntu ARM64 VM were used. Tests
create their own clusters, restricted runtime roles and temporary services.
PostgreSQL's fsync and synchronous_commit remain enabled. A local TCP forwarding
fixture drops an actual committed response; it does not replace PostgreSQL.
The tests inspect SQL values, exact gap counts and latest complete alarm state.

GCC 13 TSan initially aborted before tests with unexpected memory mapping.
`setarch aarch64 -R` fixes its address layout for the test subprocess tree;
no global kernel/ASLR setting changed. TSan unit and real persistence gates
therefore run, in addition to ASan/UBSan and Helgrind concurrent queue checks.
System libraries are not rebuilt with sanitizer instrumentation; this remains
finite dynamic evidence. See [checkpoint evidence](CHECKPOINT_3_VERIFICATION.md).

At Checkpoint 3 completion, Checkpoint 4 had not started. Fuzzing/CI, a
demonstration bundle and load characterization were deferred to the evidence
checkpoint recorded below. Buildroot and all deferred products remain outside
the completed core.


## Checkpoint 4 — evidence (2026-09-26 local / 27 UTC)

The owner authorized the complete evidence checkpoint and local commit, with
one Codex agent only. All eight pages of version 3 and the required project
records were read before edits. The initial working tree at `89f13f5` was clean.
The initial clean baseline exposed test-harness regressions: startup could
expire freshness before simulators became ready; waits could select an old
startup snapshot; simulator startup/shutdown waits stopped draining diagnostic
pipes. The fix records an observed healthy baseline, bounds observations to the
correct phase and drains during lifecycle waits. No daemon freshness or rejection
policy was weakened. The existing complete clean gate subsequently passed.

The scenario table is an executable Python harness over the existing real
SocketCAN encoder, daemon and disposable PostgreSQL cluster. Deterministic raw
frame injection is a test control, not a substitute transport or additional
product feature. Each case prints an expectation, asserts the actual state and
retains JSON measurements and raw diagnostic/configuration files. systemd's
SIGKILL case now proves exact known queue loss and actual supervisor restart
in one fixture. SQL state and run IDs are checked directly.

Timeout evidence distinguishes the configured deadline from timer quantization
and Linux scheduling: the 500 ms fixture uses a 20 ms tick with a documented
200 ms test allowance. It must never alarm early; observed ages are recorded.
The high-rate fixture counts kernel receive losses via SO_RXQ_OVFL and a final
post-drain heartbeat; no inference from the 8-bit sequence is presented as an
exact transport count. A paused-receiver case forces loss. Burst rates and counts
are specific to the run, with persistence disabled, and say nothing about
physical CAN arbitration, bitrate limits or electrical effects.

One libFuzzer target compiles the decoder itself with coverage, ASan and UBSan.
Thirty reproducible seeds use existing wire vectors plus malformed cases. Its
oracles include error-output immutability and successful-field invariants;
there is no stateful daemon/persistence fuzzing claim. Strict warnings now also
cover cast alignment, writable strings, VLAs, double promotion, null dereferences
and GCC duplicate/logical/format-signedness checks. GCC -fanalyzer adds a small
static gate. The only production-code change removes redundant EWOULDBLOCK
comparison (an EAGAIN alias in Linux); no wire semantics changed, so protocol
and cross-language vectors remain unchanged.

GitHub Actions runs the identical non-vcan command script tested locally:
GCC/Clang build, C/Python units, decoder CLI, actual PostgreSQL transactional and
permission contracts, ASan/UBSan, analyzer and short fuzz run. vcan/systemd are
mandatory separate local gates, not conditional passes or mocked replacements.
Actions are pinned to resolved upstream commit hashes; the job has read-only
repository permissions and uploads failure logs. No remote or hosted run was
created; local success must not be described as a hosted green CI result.

The full verifier starts clean builds, runs all original and added gates plus
the demo and longer fuzz session, then renders VERIFICATION.md from actual
results and SQL exports. The implementation digest covers CMake, src, include,
tests, scripts, simulator, config, migrations, systemd, fuzz and workflow files;
the `docs/` directory, root README and state file are excluded so the generated
report can identify its own source without a circular hash. The checked-out parent commit plus this digest identifies
the tested pre-commit working tree. Logs remain local/ignored; the concise measured
report is committed. The original Checkpoint 1 report is preserved as
CHECKPOINT_1_VERIFICATION.md.

The section-7 owner-first coding process deviation and Checkpoint 3 immutable
in-flight batch / asynchronous libpq refinements remain documented. The core is
not a NetBotz or A-Link reproduction. No Buildroot, hardware, spool, API, dashboard,
retention system, or authenticated-CAN extension is authorized or started.


A final clean rerun exposed another fixture assumption: a scheduler stall can
batch simulator samples into the same receive timestamp, triggering the existing
rate rule. Transport/count and silence tests now configure rate=0 to isolate
those concerns; rate boundaries remain in deterministic C tests. This is not a
production-policy relaxation. Arrival-time slope inference and its possible
false invalidation under batching are explicitly documented in MONITORING.md,
README and the learning guide. The complete clean gate is rerun after the fix.
