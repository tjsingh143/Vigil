# Resilience Stage 3 fault tests

Stage 4 reuses this entire fault gate and adds the production demo and matched
benchmarks; current commands/results and handoff are in
[RESILIENCE_RELEASE.md](RESILIENCE_RELEASE.md) and
[RESILIENCE_STATE.md](RESILIENCE_STATE.md). Stage 3 stopping statements below
record that earlier authorization, now superseded by the owner’s Stage 4 request.

**Stage 3 PASS for the software failure contract.** All 21 scenario categories
passed the final clean Linux gate; no mandatory command was skipped. Stage 4,
deployment and publication are outside this checkpoint.
One Codex agent; starting commit `4c179bc`, initially clean working tree.

The independent baseline built Debug on native Ubuntu ARM64 and passed all four
C suites. An independently written smoke scenario sent values 2200/2800/2200
over SocketCAN with PG stopped, confirmed three local admissions, restarted
through the outage, and verified exact original receipt bytes, ordered remote
readings and zero pending after recovery. Evidence:
`artifacts/resilience-stage3/smoke.log`; initial build log in the native source
copy's `/tmp/vigil-stage3-baseline-build.log`.

## Final observed results

The complete clean gate at `artifacts/resilience-stage3/final3/gate.json`
finished with **65/65 zero command exits, zero failed commands**, recorded UTC
2026-10-24T03:39:42.514284 through 04:26:17.973237. Tested parent:
`4c179bceffa731302324cf5a35ad6ccd89408adc`. Implementation SHA-256:
`00e7ccce3b54300f78110fee75ad29268e899205908e01ee30f0a73412c75c00`.
The unchanged-source assertion passed and the workspace digest matches. No
production C, protocol, schema, service unit or installed artifact changed.
The reviewed local checkpoint is identified by `git log -1 --oneline`; its
hash is supplied in the delivery response. Docs/state are outside the digest.

| Final acceptance measure | Observed result |
| --- | --- |
| Scenario inventory | 21/21 requested categories covered and passed by the table below |
| Unique seeded schedules | 60, seeds **0–59**, all passed in Debug |
| Cross-build seed repeats | **0, 7, 47, 48** each passed in Release, ASan/UBSan and TSan |
| Final seed executions / kills | **72 passed, 0 failed; 156 selected SIGKILLs** |
| New case groups | **75 passed, 0 failed**: 63 Debug and four per other build; five damaged-copy subcases are one group |
| Earlier seed executions | 127 passed in pilots/incomplete gates, retained separately; **199 total seeded executions**, 60 unique seeds across all attempts |
| Original local cuts | 130 admission/origin kills; 13 case groups in each build |
| Original remote suites | 14 Debug / 12 each other build; 48 confirmation/retirement cuts, 26 repeated observer-confirmed response-gap kills, plus same-process retry controls |
| Exact ambiguity attempts | 23 Debug, five per other build, all byte-identical to the original head |
| Sustained backlog | 6000 readings, 189 frozen batches, plus 128 new readings during replay; **6128 remotely verified**, zero reading/event drops and zero pending |
| Backlog restart / drain | **0.674 s / 5.359 s** in this run |
| Resource samples | Max daemon RSS **11812864 bytes**; sampled WAL **1363752 bytes**; DB/WAL/SHM file ceilings asserted in stopped inventories and live samples |
| Monitoring under paused workers | Seeded stale ages **501–522 ms**; backlog **509 ms**, configured 500 ms, 20 ms tick, 200 ms fixture scheduling allowance |
| Outage gaps | Debug/Release/ASan: 41 remote + 59 known reading losses = 100; TSan: 40 + 60 = 100; event losses zero; remote gaps exactly match |
| Shutdown under remote faults | Largest lock/statement/blackhole stop **1.076 s**, all <1.25 s |
| Physical storage | Real loop ext4 ENOSPC: **SQLITE_FULL=13**, failed-call local admission unchanged; stop **1.031 s**. Actual read-only startup: exit 2/no READY, five prior records preserved |
| Supervision | Hardened temporary unit UID501: READY, SIGKILL restart, watchdog restart, clean stop; nine committed records retained |
| C/Python/SQL regression | **390 C case executions**, seven Python cases twice, five v1 and ten v2 PG contracts twice; strict GCC/Clang and all sanitizer builds passed |
| Additional regression | Real CAN/monitoring/load/manual exchange, scoped Helgrind, GCC analyzer, shell syntax and CI command subset passed; no sanitizer finding observed |
| Finite CAN load | Natural: 50001 received / zero kernel drops; forced: 223 received + 49778 drops = 50001; both no-persistence, zero missed ticks |
| Decoder fuzz | **90815924 inputs, 121 reported seconds, 288 MiB RSS**, no finding; CI fuzz smoke also passed |

Raw summary is `final3/summary.json`; every command has its own log. Two earlier
clean attempts (`final/`, `final2/`) failed during administrative evidence export;
they are not accepted gates. Harness defects/corrections are detailed below.
No confirmed implementation defect was found in this tested scope; no production
algorithm was changed. Final source review checked unsafe targeting, exact byte
oracles, transaction cuts, worker ownership, volatile loss interpretation,
nonnegative pacing waits, startup budgets and bounded evidence export. No
legitimate failing integrity assertion was removed.

Cleanup inspection found no test daemon, loop mount or transient test unit;
default vigild remained inactive and installed PG 16/main remained online.
No deployment, production-data change, push, publication or Stage 4 work occurred.

## Failure-boundary coverage

All rows passed in the final gate. Scenario numbers refer to the owner's
Stage 3 inventory, not a count of individual assertions or process executions.

| # | Scenario | Deterministic control and oracle |
| --- | --- | --- |
| 1 | Normal telemetry/events | Existing maximum-snapshot case: 64 readings, 32 heartbeats, high/degraded transitions and full 193-entry snapshots; exact remote projection |
| 2 | Kill before local commit | Three precommit pipe cuts: before BEGIN, after BEGIN, INSERT before COMMIT; candidate absent, prior durable bytes unchanged |
| 3 | Kill after local commit | After FULL/directory sync before publication; candidate/origin/allocator/payload present |
| 4 | Kill during remote send | `after_remote_send` pipe; outcome allowed whole or absent, local head retained; server advisory-wait case kills daemon during actual PG transaction then terminates that isolated backend |
| 5 | PG outage through restart | Every mixed schedule first restarts with its own PG server stopped; baseline survives and live values start unknown |
| 6 | Commit before observed acknowledgement | Real-PG proxy suppresses response through ReadyForQuery; independent SQL confirms exact receipt |
| 7 | Kill after confirmation before local acknowledgement | `after_remote_confirm_before_result` and `before_retire_begin`; local head retained and equal retry |
| 8 | Kill within acknowledgement transaction | `after_retire_delete_before_commit`; rollback restores exact head/prefix; after-commit cut leaves it retired |
| 9 | Repeated crashes with pending data | Seeds 48–59 combine four local cuts with one/two/three response-gap crashes; prior durable union checked at each restart |
| 10 | Identical duplicate | v2 invoker concurrent callers, proxy retries and restarted daemon; equal receipts, one logical effect |
| 11 | Same ID, unequal payload | v2 concurrent batch/event conflicts, origin conflicts and daemon poison-head case; VG001, no partial mutation |
| 12 | New events during replay | PG-owned cuts permit concurrent local commits; SQLite-owned cuts allow monitoring/volatile ingestion; 128 new readings admitted behind 6000 recovered readings |
| 13 | Sustained backlog/drain | 6000 readings at scheduled 100/s with handshaked groups of 32 during PG outage; restart validation, file/RSS samples, exact 6128-reading drain |
| 14 | Volatile overflow | Existing capture race, four-reading outage lane, independent 256-event overflow C test; separate exact gaps/lane conservation |
| 15 | Durable capacity | Record/batch/byte unit ceilings and daemon FULL/return cases; no durable eviction or false admission |
| 16 | Physical FULL | Fresh sparse loop-mounted ext4, real block allocation to ENOSPC; SQLITE_FULL and failed-call admission unchanged |
| 17 | Write/read-only failures | Runtime test VFS xWrite/xSync/directory failures; actual read-only ext4 startup refuses READY and preserves prior rows |
| 18 | Corrupt/truncated DB/WAL | Five disposable clone cases: header/truncated standalone DB refused; partial uncommitted suffix ignored; destroyed final commit yields exact valid prefix under corruption exclusion |
| 19 | Alarms/known gaps | Live high/stale/degraded lifecycle; copied capture race; remote gap sums equal saved known drops; old-run acknowledgement cannot clear new-run health |
| 20 | Shutdown/watchdog/restart | Lock/statement/blackhole drain bounds; temporary hardened unprivileged systemd SIGKILL/watchdog restarts and preserved local bytes |
| 21 | Regression | Strict Debug/Release/ASan/UBSan/TSan, protocol/monitor/queue/outbox, v1/v2 SQL, real CAN/load/manual, Helgrind, analyzer, fuzz, CI command subset |

The 60 schedules are distinct fault combinations: seeds 0–47 cover four local
cuts × six remote cuts × ingress absent/present. Seeds 48–59 cover four local
cuts × one/two/three consecutive observer-confirmed response-gap kills. Seeded
payload counts vary but do not replace fault-boundary coverage. UUIDs, times,
batch grouping and actual measurements may differ on reproduction; the ordered
boundary plan and invariant assertions remain the same. No sleep selects a kill.

## Reproduction and evidence

```sh
./scripts/dev sh -c 'cd /tmp/vigil-stage3-source; ./scripts/check-resilience --clean --output /Users/tj/Desktop/Vigil/artifacts/resilience-stage3/reproduce-full'
./scripts/dev sh -c 'cd /tmp/vigil-stage3-source; .venv/bin/python tests/resilience_adversarial.py --build build-resilience-debug --seed 59 --output /Users/tj/Desktop/Vigil/artifacts/resilience-stage3/reproduce-59'
```

Use a fresh output directory and the tested source. The gate records parent
commit, implementation digest, commands, expectations, exits and timings.
`*-adversarial/results.json` records build hash, seeds, expected assertions and
failures. Each seed's `run.json` records failpoint tokens, kill/stop exits,
monitor/status counters and reproduction command. Its native directory contains
stopped DB/WAL/SHM copies, inventories and local/PG union JSON at each cut.
Final union comparison checks every previously known durable order, exact bytes,
retired prefix, original origins, event sequences, complete history projection,
snapshot and gap contents; row counts alone cannot satisfy it. Raw daemon
stdout/stderr/config is retained per case. No test database target is accepted
from CLI. Each schedule allocates a fresh PostgreSQL cluster and local spool.

The 13 native evidence roots are preserved in `final3/native-evidence-paths.json`.
A portable offline copy is `final3/native-inventories.tar.gz`, **14194488 bytes**,
SHA-256 `bea05534fc0971baedb755959be4e7ed4b60b9120e9a3b9a607dcc75fa21fb19`.
Its DB/WAL/SHM files are archival test copies, not active stores.

## Review findings and assumptions

The old limits-only fixture environment could use installed PostgreSQL defaults.
It now selects an explicit offline endpoint. The complete gate strips inherited
PG routing/options/credentials and test controls. New PG destructive operations
first compare the server's `data_directory` with the freshly allocated fixture.
Loop mounts and transient service operations remain confined to their existing
unique test paths/units. Production hook strings/VFS controls and symbols must
be absent; the installer copies only the production executable.

Pilot corrections are retained in `artifacts/resilience-stage3/`: a source
archive initially lacked Git metadata for evidence identity; an intact WAL
legitimately recovered a damaged DB header; the large-store startup needed the
contract's startup allowance instead of the small fixture's five seconds; a
pacing deadline crossed between two clock reads and needed a nonnegative poll
timeout; a free-running 100/s pilot correctly reached the 4096-batch ceiling
with 5807 durable readings, so the drain fixture now uses handshaked groups of
32 with unchanged limits. One-statement PG exports avoid mixed snapshots at
a concurrent server commit. These are harness findings, not implementation
durability failures.
No legitimate invariant assertion is removed to obtain a pass.

The first full-gate attempt (`final/`) passed all 60 seeds and the damage/server
cuts, then stopped on a nonzero administrative PG snapshot-export command after
backlog drain. Its original server stderr was not retained, so the precise cause
remains unknown. The exporter now surfaces that stderr. An isolated full backlog
rerun passed, and a fresh disposable server reconstructed the same 6128 readings
from exact receipts: 20 exports matched the full original inventory, with observed
export durations 0.088–1.350 seconds. No daemon deadline or integrity assertion
was relaxed. A second clean attempt (`final2/`) reproduced the same call-site failure and
retained the precise error: PostgreSQL canceled the export at the fixture's
2-second statement timeout. The bulk administrative export now has a bounded
15-second statement budget inside the existing 20-second subprocess limit.
This changes neither daemon query deadlines nor integrity assertions. The
failed scenario and full clean gate are rerun after that fix. The final complete
clean rerun is reported separately below.

SQLite recovery stops at an invalid WAL checksum and uses the last valid commit;
a damaged commit frame can therefore look like a valid shorter history. Tests
assert that outcome explicitly instead of claiming all corruption is detectable.
This follows [SQLite WAL recovery](https://www.sqlite.org/walformat.html#recovery).
The pause fixture uses real server-scoped advisory locks, whose ownership is
specified by [PostgreSQL 16 locking](https://www.postgresql.org/docs/16/explicit-locking.html#ADVISORY-LOCKS).
Media corruption, destructive storage rollback/deletion and dishonored flushes
remain outside survival assumptions. Process SIGKILL is not a power cut.

No physical CAN, XFS, hardware/power-cut, failover, maximum simultaneous storage
ceiling, universal delivery deadline or exactly-once transmission qualification.
RSS/file observations are finite samples, not proof of every resource bound.
This fault stage does not provide enqueue-to-commit/retire latency percentiles,
SQLite heap instrumentation or queue-lock timing. Those performance measurements
remain unverified. Remote history/receipts still have no automatic retention.

## Independent audit overlap regressions (2026-10-25)

Earlier successful gates did not cover the coordinator dependency between a
confirmed old replay and an uncertain new append. The independent audit exposed
that livelock and stale usage after postcommit retirement NOMEM. Preserve the
historical evidence above; it does not qualify the rejected candidate against
these new cases. The [remediation report](RESILIENCE_REMEDIATION.md) records the
unmodified C counterexamples and new production-coordinator negative controls.

`tests/resilience_remediation.py --build BUILD --output FRESH_DIRECTORY` runs
30 deterministic scenarios on disposable real SQLite/PG and vcan, including
16 reconciliation SIGKILL cuts. Independent indexed pipes hold both workers at
actual production boundaries; a successful PG result is explicitly published
before storage resumes. Eight overlap cases and six retirement-NOMEM cases
require quiet same-PID progress. Tests compare consistent DB/WAL copies, exact
payloads, real PG receipts/history and all published counter pairs. One-shot
API wrappers simulate usage-query NOMEM at prepare, first step and completion;
these are targeted failures, not exhaustive heap-exhaustion proof. Dispatcher,
wrappers and controls are excluded from the production binary. The clean gate
runs the new scenarios in Debug, Release, ASan/UBSan and TSan builds.
