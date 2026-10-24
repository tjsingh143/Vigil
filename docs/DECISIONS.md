# Engineering decisions

Entries are chronological records. Statements about unfinished or unauthorized
work apply to their checkpoint, not to the finished release. The final section
records Checkpoint 5; [MONITORING.md](MONITORING.md) describes current policy.

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
repository permissions and uploads failure logs. At the time of this decision no
remote or hosted run existed, and local success must not be described as a hosted
green CI result. The repository was published after the 1.0 audit and the hosted
job passed on the release commit; that result still covers only the non-vcan
subset and is not evidence for vcan, systemd or hardware.

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

## Checkpoint 5 — final engineering audit (2026-09-27–28 UTC)

The owner authorized an independent release audit and a conditional, bounded
Buildroot attempt after the core and evidence package were complete. That
instruction supersedes the earlier checkpoint stopping boundary. One Codex
agent read the complete brief, all documentation, repository tree, history,
source and tests, then ran the original clean gate before edits. The baseline
passed; prior reports were not treated as proof of current behavior.

The audit confirmed two evidence defects: SQLSTATE identifiers such as `08001`
lost their leading zero in the Python parser, and the implementation digest
omitted `tools/decode.c` and environment definitions. The parser now preserves
identifiers and has a regression test. Digest coverage includes `tools/`,
`env/` and retained `embedded/` packaging. Historical hashes used the earlier
coverage; they must not be compared as though they used the new algorithm.
No production C or SQL behavior changed; the release version is 1.0.0.

The release gate now includes optimized integration and five additional
behavioral scenarios under Release, ASan/UBSan and TSan: six quick counter
restarts with silence recovery, three outages within one daemon UUID, denied
database-function permission followed by recovery, the complete 193-entry
alarm snapshot, and ten invalid startup configurations. The maximum-snapshot
observer uses a larger pipe to isolate serialization from the separately
tested diagnostic-drop policy. It does not weaken that production policy.

The expanded clean core gate passed all ten commands before optional packaging
began. Interview, resume and learning documents distinguish observed facts
from suggested wording and the owner's personal understanding. Network
deadlines are explicitly qualified for local filesystem and scheduling stalls.
The README installation sequence was also executed against the actual default
service: six readings and three heartbeats persisted, UID 999, zero rejected
frames, clean stop. The service was disabled afterward for an idle test bus.

The final release run and optional extension disposition are recorded in
[FINAL_VERIFICATION.md](FINAL_VERIFICATION.md), with generated core measurements
in [VERIFICATION.md](VERIFICATION.md). The original owner-first coding process
deviation and queue/in-flight/libpq refinements remain explicit.

## Resilience Stage 1 — local outbox (2026-10-22)

The owner explicitly authorized the SQLite durable-outbox implementation,
daemon/service configuration integration, fault tests and a safe local commit.
This supersedes Stage 0B's proposed module-only slice. One Codex agent performed
all work; no delegation. The brief and required documentation were read in full;
main was clean at 2bc113b. The original Release gate passed on an archived copy
before edits, including all original PG integration scenarios. The owner-first
coding process deviation from the brief remains explicit.

The new owner worker performs all SQLite/directory-sync work; main keeps live
alarms, CAN/timer/signals and watchdog ownership. Queue tickets are volatile;
transactional sequences and complete frozen format2 batches become durable only
after FULL/WAL and directory sync. The worker enforces signed allocator bounds,
exact schema/JSON/cross-row validation, native ext4/XFS, file ownership/modes and
exclusive ownership. Uncertain writes retain the immutable candidate and recover
through a real generation-changing FULL transaction before publishing success.

No PostgreSQL worker/replay/retirement or migration 002 was implemented. All
local batches remain pending, remote-written counters stay zero and degraded
remains raised. This explicit intermediate behavior avoids retiring data through
the v1 function, which cannot verify content/origin. Existing v1 migration and
its original database contracts remain unchanged. The full remote contract is
reserved for Stage 2, which is not authorized by this checkpoint.

CMake requires SQLite >=3.51.3; scoped provisioning verifies the official source
hash and installs an isolated dependency under /opt without replacing distro
SQLite. Foreground/service outboxes require explicit empty-directory bootstrap;
normal startup never initializes missing history. The service gains only its
private writable StateDirectory and a 30-second startup allowance. Production
services/data were not deployed or changed; tests use disposable stores, mounts,
clusters and a temporary unprivileged hardened unit.

The current clean gate replaces obsolete PG-outage queue-fill assumptions with
local admission/capacity/crash assertions while preserving original monitoring,
protocol, queue, Python, vcan/load/fuzz/manual and standalone v1 SQL contracts.
Original PG tests/demo/verifier remain historical fixtures for the archived
pre-outbox binary. OUTBOX.md explains current operation and acceptance limits;
RESILIENCE_STATE.md records actual final evidence and stop boundary.

Final review made checkpoint freezing a store invariant: failed checkpoints
block all later mutations even below the WAL trigger and during shutdown or
recovery. A VFS test holds only the checkpoint lock busy, leaving the writer
lock available, proving the append refusal and unchanged usage/allocators.
The final clean Linux gate passed all 55 commands/240 C case executions;
RESILIENCE_STATE.md records the final source digest and raw evidence.

## Resilience Stage 2 — replay and reconciliation (2026-10-23)

The owner authorized only Stage 2, including local commit, with one Codex agent.
No delegation, deployment, production change, push or publication. Starting main
was clean at dd2773c. The full v3 brief, state/contracts/architecture, Stage 1
changes and actual source were independently inspected. The unchanged Stage 1
Debug core durability suite passed before remote implementation; the complete
55-command baseline finished on its detached native Linux worktree while
implementation continued. The brief's owner-first coding deviation remains explicit.

A separate PG owner preserves asynchronous libpq deadlines and uses a bounded
mailbox independent of SQLite admission. Replay selects the frozen durable FIFO
head; unknown outcomes retry exact bytes. Migration 002 adds original origins,
exact receipts and invoker verification around unchanged deployed 001. Equal
content succeeds, conflicts poison, and no unconfirmed remote effect can retire
local history. SQLite's existing schema supports transactional deletion/prefix,
usage, origin-cursor and lifetime counter updates; uncertain retirement recovers
and seals before acknowledgement. Current-run health cursors remain separate
from old-run spool retirement. Main keeps the monitoring and watchdog policies.

OUTBOX.md and the architecture/contract amendments explain grants, capability
checks, retry classification and the safe late-shutdown-result refinement.
Stage 2 tests cover controlled A-J boundaries, direct local/PG content and
accounting, and original local/monitor/supervision regressions. Actual results
and development findings are recorded in RESILIENCE_STATE.md. Stage 3 benchmark,
new products, hardware/power-cut qualification and retention remain deferred.

The Stage 2 maximum-snapshot gate exposed self-generated degraded alarm chatter:
Stage 1's unconditional pending-batch warning re-raised while persisting the
clear transition. Healthy buffering now only holds a pre-existing degradation;
failures/new losses still raise it. PG startup waits for the complete initial
snapshot, preventing a health probe racing the first main-owned alarm update.
The queue policy has a direct regression and maximum-snapshot remote evidence.

## Resilience Stage 3 — adversarial proof (2026-10-23 local)

The owner authorized Stage 3 fault testing, minimal safely scoped fixes and a
local checkpoint commit, superseding Stage 2's historical stop. One agent only.
The full brief v3 and required records, actual SQLite/libpq/SQL implementation,
previous tests and Stage 2 changes were read independently. A fresh native
Debug build/C gate and an independently written CAN/local/restart/PG smoke
passed before harness changes. No production migration/data/service operation,
deployment, push or publication is authorized.

Reuse the existing Python/Linux fixtures and test-only pipe/VFS dispatcher.
Add 60 distinct composed schedules, actual server-transaction daemon kill,
corrupt/truncated offline clones and a 6000-reading sustained backlog with
concurrent arrivals on replay. Every stopped inventory checks the union of
exact SQLite payloads and exact remote receipts; seed combinations test the
SQLite owner pause separately from the PG owner pause. PG dumps use one
statement snapshot to avoid exporting mixed states around server commit.
Production control isolation remains a mandatory assertion. See
[RESILIENCE_FAULT_TESTS.md](RESILIENCE_FAULT_TESTS.md) for observed evidence,
coverage, reproduction, pilot corrections and boundaries still unqualified.

A 60-second/100-per-second stream can exhaust 4096 batches before 65536 records
because batching is scheduler-dependent. That is correct capacity behavior,
not a guarantee of fitting every 6000-record arrival shape. The backlog/drain
fixture uses handshaked groups of 32, with a test-only release-and-disable
command, leaving all production caps/algorithms unchanged. Ordinary overflow
and FULL tests retain their exact accounting assertions.

The complete gate reproduced an evidence-export timeout after the large replay
had drained. Single-snapshot bulk JSON export inherited the helper's 2-second
small-query statement timeout. Its administrative statement now gets 15 seconds,
inside the existing 20-second subprocess bound; daemon SQL/deadlines and all
content assertions are unchanged. Export errors retain server stderr. Failed
attempts and the successful focused rerun remain separate from final acceptance.

## Resilience Stage 4 — measured release candidate

The owner authorized the release/performance checkpoint, full regression/fault
and benchmark gates, a production-binary demonstration, concise documentation
and local commits. This supersedes the historical Stage 3 stop. One agent only;
no delegation, production-data/service modification, deployment, push or remote
release. Initial main was clean at 51582d1. The full v3 brief, state/contracts,
source/tests and diff against original d179681 were reviewed. Fresh initial
Release regressions passed before implementation changes.

Use the recorded original baseline commit in a newly allocated detached native
Linux worktree, without moving branches. Alternate original/Resilience trial
order, with identical compiler/environment/configuration and simulated workload,
an excluded warmup and multiple measured runs. Separate core-call probes,
production-daemon paced/recovery observations and single-owner outbox API timing;
no conversion of a microbenchmark into a universal service-rate claim. Linker
wrapping measures actual BEGIN-to-COMMIT returns in a standalone probe; directory
sync is included in the wider append API timing. Production algorithms, schema,
protocol and service behavior are unchanged in this stage.

The old volatile-writer interview fixture stays available for the pinned original
binary. `scripts/demo` now selects a production Resilience story using disposable
native SQLite/PG, crash/restart, old-run replay, observed server commit with lost
response, identical retransmission, live alarms and exact final inventories.
A brief clean stop after restart permits supported offline byte verification.
New measurement/demo resources clean up on errors, SIGINT and SIGTERM; killing
the harness itself with SIGKILL cannot execute cleanup. A preliminary benchmark
launch was deliberately interrupted before accepting numbers to keep vcan work
sequential; its incomplete evidence remains separate.

Existing hosted CI already ran safe unprivileged outbox units and disposable
v1/v2 SQL contracts. Preserve those gates and retain the v2 contract JSON as an
artifact. Full vcan/systemd/physical-filesystem faults remain explicit Linux-local
requirements. The full verifier now adds optional strict measurement probes,
the new demo and three >=60-second measured trials per binary. No hosted run or
independent-review result is inferred from local success. Current outcomes,
limitations and candidate identity are maintained in RESILIENCE_STATE.md.

Stage 4 review tightened the new harness cleanup so transcript-write failures
cannot bypass PG teardown. The first clean attempt was intentionally interrupted
before acceptance for that correction. A subsequent clean attempt passed all 60
Debug schedules plus server-kill/damaged-copy cases, then failed the administrative
backlog PG export at the unchanged 15-second statement timeout. That attempt's
UTC elapsed times contain large jumps relative to its monotonic durations; the
precise cause of the cancellation is unproven. The unchanged-budget focused
server/damage/6128-reading case passed. The next complete clean run uses a
process-scoped macOS idle-sleep inhibitor, with no persistent power setting or
integrity/deadline relaxation. Raw attempts remain separately classified.

The final complete clean attempt passed all 68 commands and the unchanged-source
check. The final measured table retains all three trials and the slower/higher
resource-cost Resilience results; it does not rebrand API/core probes as daemon
capacity. Independent verification, actual power/hardware work and the explicit
remaining design-matrix injections are unclaimed. State records the accepted
source digest, evidence archive and exact candidate in a subsequent local
documentation receipt, avoiding a circular self-referencing commit hash.
