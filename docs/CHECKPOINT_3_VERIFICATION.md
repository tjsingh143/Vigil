# Checkpoint 3 verification

## Expectations recorded before implementation

Baseline: clean Debug and ASan/UBSan builds, all 39 cmocka cases per build,
six Python tests, both Checkpoint 1 decoder and Checkpoint 2 daemon vcan suites
must pass. Short simulator smoke: nine frames, six readings, three heartbeats,
then three communication timeouts and clean SIGTERM exit.

Checkpoint 3 must retain that gate and add:

- Deterministic queue tests: FIFO, wrap, full capacity evicts exactly the
  oldest pending reading, exact independent event overflow counters, copied
  ownership, batch acknowledgement, failure retry without duplicate accounting,
  high water, shutdown and concurrent producer/consumer conservation.
- Real PostgreSQL/vcan: available writes; stopped DB does not delay CAN or
  timeout alarms; recovery before overflow backfills all readings in order;
  N+K readings during a settled outage retain the newest N and drop exactly K.
- Overflow raises persistence-degraded; recovery records exact gap counts and
  latest alarm state, then clears degraded after catch-up. SQL contents, not
  diagnostics alone, must substantiate every history assertion.
- Slow/locked server queries and a nonresponsive connection hit deadlines;
  retries are paced. Monitoring and clean shutdown remain responsive.
- SIGTERM drains reachable history with a bounded grace period; outage exit
  reports abandoned memory. SIGKILL during outage loses unpersisted memory;
  a new run must never claim to restore it.
- Real systemd Type=notify readiness, unprivileged runtime, watchdog pings,
  SIGKILL restart, watchdog restart of a stopped process, clean service stop.
- Strict warnings, ASan/UBSan, and ThreadSanitizer if usable; otherwise a real
  concurrent race-detector alternative with its limitations recorded.

All Linux integration uses the existing Ubuntu ARM64 Lima VM and real vcan0.
Tests use an isolated disposable PostgreSQL cluster, never stop a user's DB.
No physical CAN, durable spool, Checkpoint 4 fuzz/CI/extension claim.

Additional expectations before expanded runs: a server trigger sleeping five
seconds must be cancelled by statement_timeout (SQLSTATE 57014); a real table
lock by lock_timeout (55P03). A local TCP forwarding fixture will discard a
committed reading batch's response: SQL must show committed rows, while retry
must acknowledge exactly once without duplicate history. Event-lane overflow
must match the reported event drops in SQL and restore current alarm state.
The forwarding fixture still uses real PostgreSQL and real vcan, not a DB mock.

Final review regression expectation: a newly observed drop must raise degraded
even if the writer acknowledged its gap before main sampled counters; normal
buffering alone does not raise it, and an active warning waits for queue and
alarm revision catch-up. Server NOTICE output must not use blocking stderr.

## Observed final clean gate — 2026-09-26

`./scripts/dev ./scripts/check --clean` exited 0 after the complete checkpoint
review and final overflow-observation fix. It removed and rebuilt Debug,
ASan/UBSan and TSan outputs. All work ran in the existing Ubuntu ARM64 Lima VM,
with real vcan0, PostgreSQL 16.15, systemd 255 and GCC 13.3.0. The full local
transcript is `artifacts/checkpoint-3-final.log` (ignored generated output).
This committed record contains the durable summary of its observed results.

| Check | Observed |
| --- | --- |
| Strict builds | All three built with warnings as errors |
| CTest | 13 protocol + 26 monitor/config + 6 queue/health cases passed in every build: 45 each |
| Python unittest | All six passed |
| Checkpoint 1 decoder integration | Both Debug/ASan passed, 18 exact simulator frames, malformed traffic, can-utils and signals |
| Checkpoint 2 monitor integration | Both Debug/ASan passed all original scenarios, exact counts, faults, signals and stdout backpressure |
| Checkpoint 2 silent-pod timing | Debug communication alarm ages [603,603,603] ms; ASan [600,600,600] ms, for 600 ms timeout |
| Queue concurrency | 20,000 publications; ordering, payload identity, written+dropped conservation and empty final queue verified |
| Health policy | New loss raises degraded even when already acknowledged; startup/outage, backlog and snapshot catch-up boundaries passed |
| PostgreSQL history | SQL verified identity/channel, readings/heartbeat, CRC/range rejection evidence, validity, UTC and monotonic timestamps |
| Lock timeout | Real AccessExclusiveLock produced SQLSTATE 55P03; eight readings backfilled; all three source timeouts continued |
| Statement timeout | Real five-second sleep trigger cancelled at the configured timeout with SQLSTATE 57014; three readings backfilled; NOTICE did not block/log to stderr |
| Lost response | Real PostgreSQL committed a reading batch; forwarding fixture discarded its response; retry yielded exactly five rows and five confirmed readings |
| Crash during known DB outage | Two pre-outage rows remained; exactly three known queued/unsubmitted readings vanished; next UUID had no invented backfill |
| systemd | Type=notify reached running; UID 999 (vigil), healthy watchdog sustained, SIGKILL restart and SIGSTOP/watchdog restart verified; NRestarts=2 |
| systemd stop with DB unavailable | Clean stop observed in 1.058 s |
| Sanitizers | No ASan/UBSan/TSan findings in the final unit and integration gates |
| Helgrind | Six queue/health cases; 0 errors from 0 contexts (standard library suppressions reported separately) |
| Shell/diff checks | Script syntax and git diff --check passed |

The persistence suite ran against each of Debug, ASan/UBSan and TSan. It queried
PostgreSQL directly, including exact retained values in sequence order, summed
gap counts, active **and cleared** latest alarm states, and outage end-start
matching reported duration. It never substituted application logs for SQL.
The initial high alarm cleared during the no-loss outage after cooler readings;
it remained active during the overflow outage with continued hot readings.

| Measured scenario | Debug | ASan/UBSan | TSan |
| --- | --- | --- | --- |
| Capacity 8, five outage readings | 5 backfilled, 0 dropped, no gap | same | same |
| Observed DB outage, no loss | 2034 ms | 2037 ms | 2021 ms |
| Communication alarm age during that outage (500 ms timeout) | 515 ms | 521 ms | 514 ms |
| Capacity 4, eleven outage readings | 4 newest retained, exactly 7 dropped, one gap | same | same |
| Observed DB outage with overflow | 2023 ms | 2032 ms | 2028 ms |
| Communication alarm age during overflow | 511 ms | 517 ms | 517 ms |
| Event overflow, 300 paced readings, capacity 4 | reading drops 296; event drops 46 | reading drops 296; event drops 47 | reading drops 296; event drops 46 |
| Unresponsive TCP shutdown | 1.043 s | 1.052 s | 1.072 s |

The 300-reading event test waits for each bounded 20-frame burst; it is a queue
capacity test, not a CAN load benchmark. Extra timeout/lifecycle events can
vary with scheduling, so the event-drop total is checked against that run's
SQL gap sum rather than hard-coded across builds. Read drops are deterministically
296, and the current door-high state is present after event-history loss.
A normal stop after recovery had zero abandoned records. The unresponsive
endpoint stop reported exactly three abandoned readings in each build.
Outage duration starts when the writer observes failure; it does not claim
to measure an unknowable underlying fault instant. All results are local VM
observations, not hard real-time guarantees.

## Installation and provisioning observed

Ran `scripts/install-service`, `scripts/setup-database` twice, and
`systemd-analyze verify` successfully inside the VM. After the final clean
gate, reinstalled the final binary and started the default service against
the default cluster using Unix peer authentication. A three-cycle simulator
produced exactly **six reading rows and three heartbeat rows** in SQL, run
`8df2cd19-a95d-4a4c-afa2-0ffeb4aaa58d`. Service stop returned ExecMainStatus=0.
The installed service is **disabled and inactive** at completion; no vigild
process was left running. The default PostgreSQL package service remains online;
the small smoke history remains available there. Disposable test clusters and
temporary systemd units were removed. The Lima VM remains running.

## Review findings, fixes and test limitations

The entire checkpoint diff was reviewed: queue bounds/ownership/conservation,
all lock scopes, libpq ownership, condition wakeups/backoff, signal inheritance,
client/server deadlines, immutable retry payloads, parameterized SQL and role
grants, shutdown/join/destruction, alarm publication/recovery, protocol immutability,
service hardening, tests, learning guide and persistent state.

Review fixed a possible missed condition wakeup, ensured final diagnostics
cannot create alarms after the writer stops, retained exact completed-outage
start/end times, and sampled status time after shared counters to avoid a
cross-thread timestamp subtraction wrapping. The last review added a regression
for loss already acknowledged before main observes it; every newly observed
drop now still raises degraded. Libpq notices use a discard callback so its
default blocking stderr writer cannot undermine the shutdown bound. The whole
clean gate was rerun after these code changes; later edits only record results.

Initial harness corrections are retained here rather than hidden:

- The ad hoc baseline smoke first used a helper with the wrong Python argument;
  its corrected finite simulator invocation passed. Baseline project tests had
  no regression.
- Settled outage assertions originally required *all* in-flight records to be
  zero. Alarm events can legitimately be in flight; the exact reading-capacity
  tests correctly require zero in-flight **readings** instead.
- TSan initially aborted before tests due to unexpected memory mappings.
  `setarch aarch64 -R` made its unit and real persistence gates practical.
  TSan also adds a runtime thread, so process inspection permits that extra
  thread. Global `/proc/sys/kernel/randomize_va_space` remained 2.
- The first temporary service binary was placed on noexec `/run`; the fixture
  now installs it under `/usr/local/libexec`. `systemctl kill` initially raced
  auxiliary-process enumeration after killing main; targeting main explicitly
  fixes the fixture. Unit verification before installation also correctly
  reported that its configured executable did not yet exist.
- The initial unpaced event-overflow burst overran CAN reception in the ASan
  build. Readiness-driven bounded bursts isolate the intended queue-capacity
  test. No sanitizer memory error was reported. Final suites observe no RX
  overflow or unaccounted diagnostic loss.

Sanitizers do not instrument every system library; finite tests cannot prove
race absence. GCC TSan requires process-local ASLR adjustment in this VM.
Durability stops at PostgreSQL-confirmed/actually committed data; pending and
unconfirmed memory, including exact old-run loss counts, can vanish on crash.
The extra 32-record immutable batch and 256-event reserve are explicit bounds.
No on-disk spool, disk-failure testing, physical CAN, throughput guarantee,
remote TLS/Kerberos failure matrix, config reload, retention product, fuzzer,
CI or Checkpoint 4 work is claimed.
