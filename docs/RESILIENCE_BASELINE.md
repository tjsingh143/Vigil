# Vigil Resilience — Stage 0 baseline audit

Audit date: 2026-10-22 America/Los_Angeles (2026-10-23 UTC). One Codex agent;
no delegation. Scope is investigation and documentation only. The owner's
Stage 0 request supersedes the earlier release stop only for this audit.
No SQLite implementation, daemon change, migration change or Stage 1 is authorized.

## Exact starting state and preservation

- Repository root: `/Users/tj/Desktop/Vigil`.
- Audited HEAD: `d179681a40497ada533d2f61e50b5ac60a6ca293`, branch `main`.
- Initial tracked/index/untracked status: clean. Ignored builds, Python venv,
  bytecode and substantial earlier `artifacts/` evidence already existed.
- Origin fetch/push URL: `https://github.com/tjsingh143/Vigil.git`.
- `main` was three commits behind cached `origin/main`; read-only
  `git ls-remote origin refs/heads/main` confirmed
  `f7a2208e9c98ed9905dbb2ee62267a388ca6129c`. No fetch, pull or branch update.
- The three commits are `c66b28d`, `cd579fb`, `f7a2208`. Their aggregate diff
  changes documentation/instructions, including deletion of the PDF and
  several engineering records. It changes no C, SQL, test, build or unit file.
  This audit follows the actual local instructions and eight-page v3 brief.
- Local history also includes release tag `vigil-v1` at `3ec973e`, then
  checkpoints 4/3/2/1 at `8ed444a`, `89f13f5`, `aa785cb`, `d37e81c`.
- Initial inspection was read-only: Git status/index/history/remotes, complete
  brief, state/decisions/learning guide, required source/contracts, fixtures,
  scripts and workflow preceded project edits or test execution.

Fresh evidence root (ignored, local):
`artifacts/resilience-stage0-20261023T000920Z/` (called **E** below).
`initial-git.txt`, `expectations.md`, `ci-runs.json` and logs retain this audit's
identity. Tests use detached disposable worktrees at the same HEAD under E;
their `.venv` links use the existing pinned Linux dependencies. Existing build
directories, reports and evidence in the original worktree were preserved.
No reset, discard, push, publication or deployment occurred.

## Source integration map

Line references below refer to the audited HEAD, before documentation edits.

| Boundary | Exact implementation and contract |
| --- | --- |
| CAN receive → monitor | `src/vigild.c:270–328`: nonblocking classic SocketCAN, `recvmsg`, exact CAN_MTU/truncation checks, cumulative SO_RXQ_OVFL accounting, at most 64 receive attempts per turn. Tick before frame processing; infer rejection reason by counter delta; then persist frame and publish alarm state. |
| Decode → state | `src/protocol.c:20–80`: local decoded value; exact lengths precede payload access; explicit endian/signed conversion; publish `*out` only on success. `src/monitor.c:172–215`: decode, configured pod, progress, source policy or heartbeat. Malformed input changes rejection counters only; a preceding independent tick can expire alarms. |
| Monitoring policies | `src/monitor.c:11–21,57–81,91–169`: synchronous alarm callback, per-source freshness, decoded-stream silence reset, advancing counters before physical checks, range/rate validity, flat suspicion and inclusive hysteresis. High alarm survives stale/invalid input. |
| Alarm event → queue | `src/vigild.c:59–87`: copy lifecycle/severity/times to an owned JSON-detail record and call `vigil_queue_push`. Callbacks occur during monitoring, before that frame's history record is queued. Writer never receives a monitor pointer. |
| Frame → queue | `src/vigild.c:238–268`: copy accepted reading/heartbeat or rejected-frame evidence, UTC and monotonic time. Only accepted readings enter the reading lane; heartbeat/rejection/alarm records share the event lane. Rejected frames are decoded again for metadata. |
| Record shape and bounds | `include/vigil/queue.h:8–46`: owned record (`seq`, times, pod/channel, kind, 384-byte detail), 256 events, 32-record batch, 193 alarm entries; allocated N-reading ring. `src/queue.c:16–35`: N must be 1..65536, zero-initialized counters and sequence. |
| Sequence/overflow | `src/queue.c:45–72`: under mutex, ignore pushes after stopping; choose lane; increment queued count; evict oldest pending entry if full; count reading/event loss independently and retain cumulative lost-observation time bounds; assign `record.seq = ++next_seq`; signal. Caller-supplied sequence is overwritten; API returns no admission/durability result. |
| Batch transfer | `src/queue.c:74–99`: merge lane heads by global enqueue sequence, remove/copy at most 32 records under mutex, mark in-flight counts. No second record batch until acknowledgement. In-flight payload is held by writer, outside evictable rings. |
| Batch serialization | `src/persistence.c:17–23,114–181`: 160000-byte JSON buffer; copy queue health/revision/alarms under a separate short lock, serialize outside it. Batch contains run/start metadata, records, drop deltas, health and optional complete alarms. Internal bound violation aborts. |
| PostgreSQL transaction | `src/persistence.c:80–112`: parameterized asynchronous `SELECT vigil.write_batch($1::jsonb)`; require one successful result and healthy connection. `migrations/001_history.sql:3–65`: run/history/checkpoint written atomically by invoker function; checkpoint lookup skips an already committed batch; history conflicts use DO NOTHING. |
| Retry and acknowledgement | `src/persistence.c:183–298`: failure starts outage/disconnect status; fixed one-second backoff cannot be shortened by enqueues. Pending JSON and batch ID remain identical across failures. On confirmed success `vigil_queue_ack` counts/removes in-flight work, then writer acknowledges captured drops/revision and outage/checkpoint counters. These updates use two separate lock acquisitions. |
| Connection/time limits | `src/persistence.c:14–16,25–78,198–216`: two-second client connect/query deadlines, 50 ms socket-poll slices; server statement/lock timeouts 1000/500 ms. Numeric IP/Unix path only; no DNS/multi-host/PGSERVICE/PGHOSTADDR indirection (`309–329`). Local libpq file stalls and scheduling are outside these bounds. |
| Identity lifetime | `include/vigil/persistence.h:4–11`; `src/persistence.c:218–226,255,309–347`: new random v4 run UUID per persistence startup; writer-local batch counter starts at zero and increments when creating a batch. Queue-global sequence starts at zero and increments on publication. All disappear with process memory. |
| Alarm snapshot/gap | `src/vigild.c:197–235`: main computes revision from monitor + degraded transitions + 1 and copies all configured active/cleared alarms. `src/queue.c:118–142`: mutex-protected snapshot publication; main-owned degraded latch waits for connection/backlog/drop/revision catch-up. `src/persistence.c:148–180,282–286`: gap uses unacknowledged drop deltas; recovery/gap/revision changes include snapshot. |
| Startup/readiness | `src/vigild.c:337–416`: CLI/config/watchdog validation; block signals before thread creation; open signalfd/CAN/timerfd and nonblocking diagnostics; start writer; initialize monitor; publish state; send READY independently of DB availability. Setup failures return 2 and join a started writer. |
| Poll/watchdog/shutdown | `src/vigild.c:419–510`: signals/ticks precede bounded CAN work; main alone sends watchdog. SIGINT/SIGTERM stop monitoring and publish final state; one-second best-effort drain then join; final abandonment status; destroy queue and close descriptors. `src/persistence.c:350–363` implements stop/deadline/join. SIGKILL executes none of this cleanup. |
| Supervision/filesystem | `systemd/vigild.service:1–36`: Type=notify, User/Group vigil, watchdog 4 s, Restart=on-failure after 1 s, ten starts/30 s, start/stop timeouts 5/3 s. ProtectSystem=strict, ProtectHome, private tmp/devices, no capabilities, UMask=0077, constrained families. No StateDirectory, writable outbox path or recovery ExecStart step. PostgreSQL After= is ordering only. |
| Build and checks | `CMakeLists.txt:1–81`: C11 strict warnings/errors, queue Threads, libpq writer, libsystemd daemon, cmocka, separate ASan/UBSan/TSan and Clang decoder fuzz target. `scripts/check`, `check-ci`, `check-release`, `verify`; `tests/verify_checkpoint.py:190–255` runs ten commands and overwrites its checkout's `docs/VERIFICATION.md`. |

## Ownership, identities and loss accounting

Main exclusively owns descriptors, monitor state, live alarms, diagnostics and
the degraded-health observation cursor. Writer exclusively owns PGconn, batch
JSON, batch counter, retry state and saved snapshot revision. Shared queue
records/health/stop flags/snapshot use one mutex and condition variable. Values
are copied; no borrowed CAN buffers. Serialization, diagnostics and database/
socket I/O occur outside queue locks. Initialization precedes use and join
precedes synchronization destruction.

Outstanding record capacity is **N pending readings + 256 pending events + at
most 32 immutable in-flight records**, plus bounded snapshot/serialization
storage. Taking a batch frees ring slots before PostgreSQL confirms it. Thus
N alone is not the total outstanding-reading bound. On a settled stopped-DB
outage with no in-flight readings, M arrivals retain min(M,N) and evict max(0,M-N).

Existing history identity is `(run_id, seq)`; checkpoint identity is
`(run_id, batch_id)`. These are sufficient for the current single-process
immutable retry, not a recovered identity allocator. Alarm snapshot entries
are not queued events: their sequence is zero from initialization and they live
inside a checkpoint's JSON array. New process startup neither resumes an old
run nor restores last values, counters, alarms, gap totals or pending payloads.

Drop timestamps bound cumulatively evicted observations across the run; they
are not per-gap exact sequence ranges. A gap counts overflow known to that
process, separately for readings/events, since the previous acknowledged totals.
It cannot account for arbitrary crash loss. Abandoned records are the final
pending + unconfirmed subset, not an additional term to add to overflow losses.
An unconfirmed batch may already exist in SQL.

The writer counts confirmed in-flight inputs, not a returned SQL inserted-row
count (`write_batch` returns void). Existing-key history/ledger no-ops therefore
depend on the identity/payload invariants. Also, queue removal and snapshot/
health capture use separate lock scopes: a checkpoint can describe a later
alarm revision than its records. Writer startup precedes initial monitor
publication, so code permits an initial empty alarm capture followed by the
published revision; this race was not independently forced by this audit.

Original limitations remain: no local spool, crash recovery, prior-process
loss reconstruction, automatic history retention or counter-exhaustion
guarantee. A one-second drain can abandon a large reachable backlog. Stdout
backpressure drops counted diagnostics; regular-file output and local libpq
credential/certificate reads can block. CRC/counters do not authenticate;
flatness is suspicion, receive-time rate checks can reject scheduler-batched
data, and vcan cannot establish physical CAN or hard real-time behavior.

## Failure contracts and evidence classification

**Code** means directly traced behavior; fresh execution is recorded in the
verification section. Historic September reports are
context only, never substituted for this run.

| Failure | Existing contract from code | Existing executable evidence |
| --- | --- | --- |
| PostgreSQL unreachable | Startup still monitors/READY; initial unconnected state degrades. Connect failure records 08001, retries after one second; rings fill/evict while main continues. No batch is removed before a connection is obtained. | `persistence_integration.py:174–227` stopped-cluster backfill/overflow; `394–410` blackhole. |
| Queue full | Drop oldest pending in that lane; allocate new global seq; independent exact drops, bounded state. No in-flight eviction or live-alarm deletion. Every newly observed drop raises degraded even if already acknowledged. | Six `test_queue.c` cases; persistence event-overflow; capacity-four/eleven-arrival scenario. |
| Ambiguous PostgreSQL commit | Keep exact JSON/ID and retry; committed checkpoint returns without duplicate writes. Client confirmation alone advances counts. SQL ledger does not compare payload contents for an existing key. | `persistence_integration.py:264–341` proxy forwards to real PG, discards committed response, verifies five rows/confirmed readings. |
| Daemon killed during outage | No signal cleanup; all pending/unconfirmed/drop/snapshot/monitor memory vanishes. SQL retains committed work subject to PG durability. Exact arbitrary loss cannot be reconstructed. | `persistence_integration.py:413–434`: controlled stopped DB, two committed and three pending, SIGKILL then SQL inspection. |
| Restart after unconfirmed work | New UUID/seq/batch allocation, unknown monitoring state; no disk scan, replay or invented old-run gap. A commit-before-death may remain only in SQL. | Same controlled crash fixture proves no invented backfill. Combined commit-response loss + process death is not exercised by the existing fixtures. |
| systemd restart | SIGKILL/failure or missed main watchdog restarts process after configured delay, subject to start limits. Normal successful termination does not trigger on-failure. Restart does not make memory durable. | `persistence_integration.py:437–553` temporary actual unit, UID/READY, healthy watchdog, SIGKILL restart, SIGSTOP watchdog restart, outage stop. |
| Early startup failure | Invalid CLI/config/DB endpoint/watchdog fails before READY with exit 2; initialized resources are cleaned. Thread/batch allocation failures and time/serialization invariants can fail or abort. Service failures are subject to restart limits. | `monitor_integration.py:379–398`; `final_hardening.py:178–193` ten queue/DB/watchdog invalid settings. Allocation/entropy/descriptor exhaustion is not fault-injected. |
| Slow/stalled persistence | Sole writer may wait; main continues with finite ring capacity. Real lock/statement cancellation errors are retried with pinned work; local file stalls can defeat join's intended time bound. | `persistence_integration.py:230–261,344–362,394–410`: lock 55P03, statement 57014, unresponsive TCP, live timeouts and bounded ordinary shutdown. |

The SQL ledger/history keys do not explicitly detect reused-key content
mismatch. Concurrent replay has not been established by the sequential retry fixtures.
Do not extrapolate sequential same-batch retry tests into a multi-writer or
arbitrary payload-conflict guarantee. No power-loss/local-disk durability test
exists for a spool that has not been implemented.

## Fresh baseline verification

**PASS — complete clean existing Linux checkpoint gate**, all ten commands
exited 0. UTC interval: `2026-10-23T01:45:02.585+00:00` to `2026-10-23T01:51:08.021+00:00`.
Implementation SHA-256 (tests/evidence.py):
`b87b8f78da76c8bcfbf7c07d7f2ec26f795ca27d56bb18eedebf864479cf419b`.
This is fresh execution against the audited HEAD, not historic CI evidence.

Environment: existing Lima `vigil`, Ubuntu 24.04.5 ARM64, Linux
`6.8.0-142-generic`, GCC 13.3.0, Clang 18.1.3, CMake 3.28.3,
PostgreSQL/libpq 16.15, systemd 255, Python 3.12.3. Initial VM was stopped;
it was started without provisioning. Installed vigild was inactive/disabled.
No installed database was queried, provisioned or stopped.

The running kernel initially lacked vcan. The owner separately approved
`sudo apt-get install -y --no-install-recommends --no-upgrade
linux-modules-extra-6.8.0-142-generic` (installed 6.8.0-142.142),
`scripts/vcan-up`, and the reviewed disposable systemd fixture. Package output
reported no service restart needed. Fixture paths were absent; existing vigil
UID 999 was reused. The fixture's own temporary cluster, unit and executable
directory are the only service/database test targets.

Before tests, E/expectations.md recorded case counts and failure behavior.
The first isolated `scripts/check-ci --clean` exited 0 but had VM clock-skew
warnings and distorted fuzzer elapsed time during synchronization; retain
E/ci-local.log and E/source as an anomalous attempt, not timing evidence.
After host/guest clock agreement and NTPSynchronized=yes, a fresh worktree
E/stable-source ran E/unprivileged_gate.py. Its JSON and separate logs record
the exact commands/exits/times. CI, Release/TSan units, Helgrind and 120-second
fuzz passed. This preliminary subset is not a complete checkpoint claim.

The primary gate runs from E/full-source at the audited HEAD:

```sh
# From original repository root on macOS:
./scripts/dev bash -c 'cd artifacts/resilience-stage0-20261023T000920Z/full-source && ./scripts/verify --clean --output artifacts/stage0-full'
```

E/full-gate.log retains the console transcript. The verifier saves exact
per-command results and environment in
E/full-source/artifacts/stage0-full/gate.json, plus scenario JSON, daemon
configs/output, SQL exports, CTest copies and fuzz logs. Only the disposable
worktree's VERIFICATION.md is generated; the earlier original report is intact.

## Exact primary test commands and observed results

All commands below ran inside Linux from E/full-source. Absolute interpreter
and output paths are retained exactly as recorded by the verifier.

| Command | Exit | Elapsed seconds |
| --- | --- | --- |
| `./scripts/check --clean` | 0 | 107.198 |
| `./scripts/check-ci --clean` | 0 | 18.325 |
| `./scripts/check-release --clean` | 0 | 30.624 |
| `/Users/tj/Desktop/Vigil/artifacts/resilience-stage0-20261023T000920Z/full-source/.venv/bin/python tests/final_hardening.py --daemon build-release/vigild --output /Users/tj/Desktop/Vigil/artifacts/resilience-stage0-20261023T000920Z/full-source/artifacts/stage0-full/hardening-release` | 0 | 18.211 |
| `/Users/tj/Desktop/Vigil/artifacts/resilience-stage0-20261023T000920Z/full-source/.venv/bin/python tests/final_hardening.py --daemon build-sanitize/vigild --output /Users/tj/Desktop/Vigil/artifacts/resilience-stage0-20261023T000920Z/full-source/artifacts/stage0-full/hardening-sanitize` | 0 | 18.054 |
| `setarch aarch64 -R /Users/tj/Desktop/Vigil/artifacts/resilience-stage0-20261023T000920Z/full-source/.venv/bin/python tests/final_hardening.py --daemon build-tsan/vigild --output /Users/tj/Desktop/Vigil/artifacts/resilience-stage0-20261023T000920Z/full-source/artifacts/stage0-full/hardening-tsan` | 0 | 18.289 |
| `/Users/tj/Desktop/Vigil/artifacts/resilience-stage0-20261023T000920Z/full-source/.venv/bin/python tests/fault_scenarios.py --output /Users/tj/Desktop/Vigil/artifacts/resilience-stage0-20261023T000920Z/full-source/artifacts/stage0-full/scenarios` | 0 | 25.692 |
| `/Users/tj/Desktop/Vigil/artifacts/resilience-stage0-20261023T000920Z/full-source/.venv/bin/python tests/interview_demo.py --output /Users/tj/Desktop/Vigil/artifacts/resilience-stage0-20261023T000920Z/full-source/artifacts/stage0-full/demo` | 0 | 6.837 |
| `/Users/tj/Desktop/Vigil/artifacts/resilience-stage0-20261023T000920Z/full-source/.venv/bin/python tests/manual_commands.py --output /Users/tj/Desktop/Vigil/artifacts/resilience-stage0-20261023T000920Z/full-source/artifacts/stage0-full/manual` | 0 | 0.087 |
| `./scripts/fuzz 120 /Users/tj/Desktop/Vigil/artifacts/resilience-stage0-20261023T000920Z/full-source/artifacts/stage0-full/fuzz` | 0 | 121.378 |

Observed primary-gate results:

- Six fresh builds each passed 13 protocol + 26 monitor + six queue cases: 270 C case executions. Seven Python cases passed twice; five disposable PostgreSQL contracts passed.
- Decoder/monitor suites passed in Debug, ASan/UBSan and Release; persistence passed in those variants plus TSan. Real systemd notify/UID/watchdog/SIGKILL/SIGSTOP restart and outage shutdown fixtures passed.
- All ten fault scenarios, five hardening scenarios in each of Release/ASan/TSan, interview demo and independent can-utils exchange passed. Ten invalid startups per hardening variant exited 2 before readiness.
- Strict GCC/Clang, GCC analyzer, ASan/UBSan/TSan reported no findings. Helgrind reported zero queue-suite errors with normal library suppressions. Primary logs have no build clock-skew warnings.
- Decoder fuzz: 94,125,524 inputs in 121 seconds, peak RSS 300 MiB; no crash, sanitizer report or invariant failure. Finite evidence, not proof of correctness.

| Reproducible fixture/workload | Fresh observation |
| --- | --- |
| Silent pod, timeout 500 ms, tick 20 ms, explicit scheduling allowance 200 ms | Heartbeat/temperature/door alarm ages [503,503,503] ms; stale/current=0; three recoveries. |
| Stopped disposable PG, capacity 8, five arrivals, no readings in flight | Five retained/backfilled, zero drops/gaps; live alarm age 518 ms; observed outage 2024 ms. |
| Same fixture, capacity 4, eleven arrivals | Newest four retained, exactly seven drops in one gap; live alarm age 500 ms; observed outage 2025 ms. |
| Lost committed response through real PG forwarding fixture | Five rows and five acknowledged readings exactly once; passed in Debug/ASan/Release/TSan persistence runs. |
| Locked table / sleeping trigger | SQLSTATE 55P03 / 57014; eight / three readings backfilled; monitoring and three source timeouts continued. |
| Paced 300 door readings, capacity 4 | Reading drops 296 in all persistence variants; event drops matched direct SQL totals (Debug/ASan/TSan 46/46/48), current door alarm recovered. |
| Unresponsive TCP, three readings | Shutdown 1.045/1.055/1.039 s in Debug/ASan/TSan, exactly three abandoned readings. |
| Controlled stopped-DB crash under actual temporary systemd unit | Two committed rows remain, three known queued readings disappear; new UUID has no invented backfill. Scenario restart observed at 1205 ms, two restarts including watchdog, UID 999. |
| Three outages within one run, arrivals 3/9/2, capacity 4 | Fourteen arrivals = nine retained + five lost; one exact gap; passed in three hardening variants. |
| Maximum snapshot | Sixteen pods, 193 unique entries, 64 readings/32 heartbeats, 32 high alarms raised/cleared; no dropped history, all three hardening variants. |
| Finite natural burst: 50000 constant temperature readings with advancing counters + probe | 50001 sent = 50001 received + zero kernel drops; 50000 accepted, zero rejected, zero missed ticks. Offered 669718 frames/s over 0.074658263 s. |
| Same count, receiver deliberately SIGSTOP-paused during burst | 50001 sent = 223 received + 49778 kernel drops; 222 accepted, one probe, one missed tick. Offered 918655 frames/s over 0.054427368 s. |
| Interview story | 520 ms stale detection; 4039 ms observed DB outage; twelve SQL readings, seven drops in one gap; 32 history rows/eleven checkpoints exported, zero abandonment. |

Both burst workloads disable persistence and use rate=0 to isolate delivery. Offered burst rate is neither sustained throughput nor database throughput; vcan establishes no physical CAN performance. All outage durations start at observed writer failure. Values above identify the fault-scenario run unless a variant is stated; different fixture invocations retain their own measurements.

Preliminary execution is also retained: E/source ran `./scripts/check-ci --clean` (exit 0, boot clock-skew anomaly). E/stable-source ran `./scripts/check-ci --clean` (0, 21.127 s), fresh Release configure/build/`ctest --test-dir build-release -V`, fresh TSan configure/build/`setarch aarch64 -R ctest --test-dir build-tsan -V`, `valgrind --tool=helgrind --error-exitcode=99 --fair-sched=yes build-ci/test_queue`, and `./scripts/fuzz 120 artifacts/stage0-fuzz120`; every command exited 0. Exact configure/build argv, UTC times, durations and logs are in E/unprivileged-gate.json and E/unprivileged_gate.py. No preliminary result replaces the primary complete gate.

Cleanup (E/cleanup.json): no test processes or temporary PG directories remain; temporary unit/executable paths are absent and unit LoadState=not-found. Installed vigild remains inactive/disabled, vcan0 is idle, global ASLR remains 2. The VM is left running, with the approved module package retained and vcan0 up; no package rollback or service provisioning was performed. Detached worktrees and raw results remain under ignored E as evidence.

No existing mandatory gate was skipped in the primary run. Existing evidence does not establish combined commit-response loss plus process death, allocator/entropy/descriptor failure injection, local spool failures, power loss, arbitrary multi-writer replay or physical CAN. These remain explicit future evidence requirements.

## CI status checked read-only

`gh api 'repos/tjsingh143/Vigil/actions/runs?per_page=5'` reported completed
success for both the audited HEAD and upstream tip:

- Audited HEAD: [run 36535763609](https://github.com/tjsingh143/Vigil/actions/runs/36535763609), created 2026-09-29T07:17:32Z.
- Upstream tip: [run 38336189402](https://github.com/tjsingh143/Vigil/actions/runs/38336189402), created 2026-10-13T02:17:36Z.

These are API-reported historical hosted statuses, not fresh executions in
this audit. Workflow `.github/workflows/ci.yml:1–40` executes check-ci on
ubuntu-24.04, excluding vcan/systemd/TSan/Helgrind. The old generated
VERIFICATION.md says hosted CI had not run at its generation time; later
README/state and the API record the subsequent run. Preserve those timestamps.

## Proposed change interfaces — for architecture finalization only

| Area requiring design/change | Required contract to resolve before implementation |
| --- | --- |
| Producer handoff (`vigil_queue_push`, record struct, vigild callbacks) | Distinguish accepted by monitoring, admitted to volatile queue, committed to local outbox and confirmed remotely. Current void push is not durable acknowledgement. Keep disk/PG calls out of callbacks and main. |
| Identity allocation (`next_seq`, run_id, writer batch counter) | Stable event identity persisted with payload; preserve original run/observation metadata on replay, while new live monitoring gets a new run. Define allocator exhaustion, restart/reboot scope and collision handling. |
| Persistence owner/scheduling (`writer`, make_batch, write_db) | Local append/recovery must make progress during PG backoff or a pinned ambiguous remote batch. Simply adding SQLite after current retry waits leaves new work volatile. Define local-commit boundary and remote-ack/local-delete ordering. |
| Outbox module/API and build | New bounded storage, transactions, recovery scan and typed errors; SQLite dependency/version/build checks. Decide count/byte limits including WAL/SHM/free pages, disk-full/I/O/corruption/busy behavior, fsync policy and lock ownership. No interface is implemented here. |
| PostgreSQL replay contract | Evaluate existing `(run_id,seq)` history key and batch ledger before selecting a future additive migration. Replay must be idempotent across processes, with stable checkpoint/gap semantics and payload identity. Do not rewrite deployed migration 001. Schema changes are forbidden in Stage 0. |
| Snapshot/gap/health publication | Persist only honest known loss; snapshots are historical, never restored as current sensor truth. Specify durable drop accounting/revision and repeated-replay behavior, old/new run ordering and crash during snapshot acknowledgement. |
| Startup/shutdown/readiness | Define local recovery and failure behavior before READY, bounded recovery work, schema/version checks, ownership/locking against duplicate processes; retain main watchdog and honest best-effort shutdown semantics. |
| systemd/config/provisioning | Persistent service-owned path is required; current strict read-only filesystem and private tmp cannot supply one. Consider StateDirectory/mode/config and SQLite sidecar permissions without broad write access; keep CAN administration separate. |
| Tests/evidence | Add process-death points before/after local commit, before/after remote commit and before/after local acknowledgement; repeat replay, disk-full/read-only/corruption/busy, capacity, large recovery and latency tests against disposable storage. Never use a fake bus as Linux evidence. |

## High-risk regressions and invariants to preserve

1. No fsync/SQLite/PG/serialization I/O under queue locks or in the monitor
   callback. A single persistence thread's local and remote duties need explicit
   scheduling so PG stalls cannot prevent local journaling indefinitely.
2. Preserve freshness/progress/plausibility, decoder error immutability, wire
   version/vectors, inclusive hysteresis and main-owned live alarms. Replay
   must not refresh a sensor or clear an alarm in the new process.
3. Preserve independent event reserve/reading loss semantics; rejected traffic
   and heartbeats can exhaust the event lane. Account separately for loss
   before durable admission versus durable outbox eviction/failure.
4. Preserve immutable ambiguous remote work until its identity/outcome is
   resolved; deleting locally before confirmed PG success loses data. Crashing
   after PG commit before local acknowledgement must yield a safe duplicate retry.
5. Preserve exact counter conservation and snapshot revision catch-up. Current
   ack and metadata updates are separately locked; recovered counters must not
   double count or claim vanished old-process loss.
6. Define the local crash boundary honestly: SIGKILL is not power loss; WAL
   mode alone is not a flushing policy. Bounded rows alone do not bound disk
   allocation/WAL growth. Numeric uint64 counters exceed SQL signed bigint.
7. Preserve signal inheritance, join-before-destroy, client/server deadlines,
   unprivileged service hardening and main-only watchdog. Heavy recovery must
   not produce false readiness, a watchdog loop or unlimited shutdown waits.
8. Keep this checkpoint documentation-only. Monitor/protocol/simulator policy,
   deployed SQL 001 and current default service/database remain untouched.

## Unresolved architecture questions

- What exact event acceptance boundary is promised, and how is still-volatile
  queue loss exposed between monitoring and local commit?
- What survives daemon death versus host/power loss, and which SQLite flush,
  filesystem and storage assumptions support each claim?
- Does replay retain immutable batches or reconstruct batches from stable
  events? How are checkpoints, gaps, snapshots and conflicts made idempotent?
- What are disk/record bounds and full/failed/corrupt/locked storage policies?
  Which failures degrade monitoring versus fail startup, with what diagnostics?
- How will local append progress during remote I/O/backoff, and how will its
  admission latency, recovery cost and queue drops be measured?
- How are prior-boot monotonic times identified and kept separate from current
  freshness clocks? Which histories require retention/migration compatibility?
- Which service state path, deployment migration/grants and operational recovery
  procedure will be approved? None is deployed by this audit.

Learning checkpoint: follow one record through enqueue, local-memory take,
remote commit and client acknowledgement as four different boundaries. Existing
same-process idempotency is useful infrastructure, but crash recovery also
requires surviving payloads, identities and acknowledgement bookkeeping.

## Acceptance and stop boundary

**Stage 0 PASS.** The local implementation boundary, failure contracts, risk
map and baseline verification status are established from code and a fresh
complete clean Linux gate. Environment blockers were resolved with explicit
owner approval; no verification blocker remains. Ready for architecture
finalization with the design questions above still open. Reconcile the three
documentation-only upstream commits before choosing a later implementation
base; this audit did not pull them. No Stage 1 work was started.

Documentation changes are limited to this baseline, RESILIENCE_STATE, and short
persistent-state/learning-guide updates required by AGENTS.md. Review includes
source line references, numerical claims against raw JSON/logs, relative links,
implementation digest equality and `git diff --check`. A local documentation
commit may record this audit; no push/publication/deployment is authorized.
Current stage status is maintained in [RESILIENCE_STATE.md](RESILIENCE_STATE.md).
