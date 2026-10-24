# Current work — Resilience Stage 4 RELEASE CANDIDATE

**Complete for independent-review handoff; not independently verified. STOP.**
One Codex agent; no delegation. Started clean main 51582d1. Owner authorized
benchmarks, full gates, demo/docs and local commits; no deployment, production
change, push, history rewrite or publication. Full v3 brief, required records,
source/tests and original-baseline diff reviewed; initial Release gate passed.

Release-candidate commit: `29b049ea8e5c8a978fb2125b517fd8a17b70e2ee`.
Tested implementation SHA-256:
`88f09dabc79ede8a750f1e68659ed478b0fb2e8787c6459bcf107f8504e58cd0`.
This documentation-only receipt changes no tested implementation. Exact results/handoff: [docs/RESILIENCE_STATE.md](docs/RESILIENCE_STATE.md).
Full clean native Linux gate **68/68 zero exits**, 2026-10-24T17:28:50.960947+00:00 through
2026-10-24T17:55:45.543621+00:00; source unchanged. Raw: artifacts/resilience-stage4/final3/.
GCC/Clang, Debug/Release, ASan/UBSan/TSan, SQL v1/v2, all existing local/remote
cuts, 72 seed executions, actual ext4/systemd, Helgrind/analyzer, CAN/manual,
95,304,612 decoder fuzz inputs/121 s, local CI commands, production demo and
three matched benchmark trials per binary passed. No hosted candidate run claim.

Demo: 20/20 readings, 43 history rows/28 exact receipts, duplicate suppression,
zero final pending/gap losses/volatile abandonment; arbitrary unsaved event loss
remains unknowable. Large case: 6128 exact readings, 0.446 s restart, 4.481 s drain.
Benchmark medians: local 99.99/s; remote original100.00/Resilience82.50 readings/s;
600-reading drain0.84/11.13s; CPU1.42/10.53% one core; RSS7.77/11.28MiB.
Remote could not keep pace at this offered natural batch shape; no universal
throughput or per-record daemon latency claim. API/core probes have narrower
boundaries documented in docs/RESILIENCE_PERFORMANCE.md.

New probes/demo/benchmark and optional CMake targets; existing tests preserved.
Production C/protocol/SQL/service unchanged. Cleanup hardened for transcript-save
errors. Interrupted attempts and the final2 administrative export timeout amid
clock discontinuities are retained separately; unchanged-budget focused and
complete final3 gates passed, without relaxed assertions/deadlines.

Native archive: 10,588,436 bytes, SHA-256
8703ef6773bf29c45e1739313694f24f81f5917dfcd51ca36800905b54f585eb.
No test process/mount/unit or detached baseline worktree remains; default service inactive, installed
PG online, ASLR2; process-scoped sleep inhibition ended. Diff/links/secrets/build
artifact review precedes local commits. See release docs for exact setup,
rollback, power/storage assumptions, fault reproduction and remaining evidence
gaps. Power cuts, XFS, physical CAN, failover, full maxima, forced entropy/UUID/OOM
injection, heap/lock/asynchronous per-record timing remain unqualified.
**STOP after local release-candidate/handoff commits. No independent-verification declaration.**

---

# Historical Stage 3 checkpoint

# Current work — Resilience Stage 3 COMPLETE

**Stage 3 PASS. STOP; do not start Stage 4.** One agent; no delegation.
Started clean main at `4c179bc`. Owner authorized tests/fixes/docs/local commit;
no production change, deployment, push or publication. Full brief/required
records/source/previous changes independently read. Fresh Debug C and exact
three-reading CAN/outage/restart/PG smoke passed first.

Final clean Linux gate: **65/65 zero exits**, 2026-10-24T03:39:42.514284 through
04:26:17.973237 UTC. Source digest
`00e7ccce3b54300f78110fee75ad29268e899205908e01ee30f0a73412c75c00`.
Raw evidence: artifacts/resilience-stage3/final3/. Full scenario/defect/limits
report: docs/RESILIENCE_FAULT_TESTS.md; persistent result: docs/RESILIENCE_STATE.md.
All 21 categories passed. Seeds 0–59 Debug plus 0/7/47/48 each Release/ASan/TSan:
72 final executions, 156 selected kills; 75 new case groups, zero failures.
Including development: 199 seed executions/60 unique. Existing 130 local cuts,
48 retirement cuts, 26 remote response-gap kills and regressions passed.
6000 backlog + 128 new readings = 6128 exact remote readings, zero loss/pending;
189 batches, 0.674 s restart, 5.359 s drain, 509 ms stale at 500 ms timeout. Six unit
builds: 390 C cases; sanitizers/Helgrind/analyzer/CAN/manual/CI commands passed.
Fuzz: 90815924 inputs / 121 s / 288 MiB, no finding. No hosted CI claim.

Harness fixes: default DB targeting, atomic PG inventory, startup/pacing/batching
oracles, bounded bulk-export budget and error diagnostics. Two failed complete
attempts are retained; focused fix rerun and full final gate passed. Production
C/schema/protocol/service unchanged; no confirmed implementation defect found.
Native archive: 14194488 bytes, SHA-256
`bea05534fc0971baedb755959be4e7ed4b60b9120e9a3b9a607dcc75fa21fb19`.
No test daemon/mount/unit remains; default vigild inactive, PG 16/main online.
Power cuts, XFS, physical CAN, failover, simultaneous maxima and latency/heap/lock
instrumentation remain unqualified. Corruption exclusions and volatile queued/
held loss remain explicit. Reviewed checkpoint identity: git log -1 --oneline.

---

# Historical Stage 2 state

# Current work — Resilience Stage 2 COMPLETE

**Stage 2 PASS. Stop; do not start Stage 3.** The owner authorized implementation,
isolated Linux/PG tests, docs and a local commit. One Codex agent only; no
subagents/delegation. Started clean main at dd2773c. No production migration,
service/data change, deployment, push or publication.

The separate asynchronous libpq owner replays immutable SQLite FIFO heads with
original run metadata and exact receipts via additive migration 002, reusing
unchanged write_batch/001. Equal content succeeds; conflicts poison. Only
validated PG completion permits FULL/directory-synced transactional retirement.
Unknown outcomes retry unchanged; old-run cursors never become current live truth.
Local retirement uncertainty is reconciled before new mutations/publication.
Main retains sensors/alarms/watchdog; storage and network never hold queue locks.

Independent Stage 1 core durability passed before remote work; all 55 unchanged
baseline commands completed successfully. Final complete clean Stage 2 gate:
**61 zero command exits**, 2026-10-24T02:25:29.975538+00:00–2026-10-24T02:38:55.042373+00:00.
Source SHA-256: `d67373a3641414533e7189648a2dd2c8946929a61c16598ebe5edb5a103e8399`.
Actual report: [docs/RESILIENCE_STATE.md](docs/RESILIENCE_STATE.md); operation:
[docs/OUTBOX.md](docs/OUTBOX.md). Raw evidence: artifacts/resilience-stage2/final7/.
Six unit builds passed 390 C cases; v1/v2 SQL, all four local/remote/monitor
variants, 26 lost-PG-response kills, 48 ack cuts, 130 local cuts, storage/systemd,
ASan/UBSan/TSan, scoped Helgrind, analyzer, CAN/load/manual/fuzz and CI commands
passed. Debug proved 23 identical submissions, 8 preserved replay readings;
64 new arrivals while recovering yielded 72 exact remote readings. Max snapshot:
193 entries, 162 rows, 32 high raises/clears. Unknown-retirement status equations
passed with twelve arrivals. Natural load 46839+3162=50001; forced223+49778=50001.
Fuzzer: 89032894 inputs/121 s/313 MiB, no finding.

Review fixed degraded-alarm feedback, retirement publication ordering, query
failure classification and recovery/allocator guards; documented fixture/tool
limits and retained failed attempts. Native evidence archive:
6511398 bytes, SHA-256 110d1a6dd688562d5e8bcafe90c8afa5eb5adb5783ee496548cc907d4ec564fd.
No test unit/mount/daemon remains; default vigild inactive, 16/main online.
No power-cut, XFS, physical CAN, throughput, unlimited retention or hosted CI
claim. Queued/held work remains volatile. The reviewed checkpoint hash is
`git log -1 --oneline`, also supplied in the final delivery response.

---

# Historical Stage 1 state

# Current work — Resilience Stage 1 local outbox

The owner authorized SQLite local durability, daemon/service configuration,
recovery/fault tests and a safe local commit. This supersedes Stage 0B's proposed
module-only scope. One Codex agent; no delegation. Work started on clean main at
`2bc113b`. No deployment, production-data change, push or publication.

The dedicated SQLite owner and explicit bootstrap now implement bounded local
admission, WAL/FULL + directory sync, stable transactional identity, reopening,
local/volatile accounting and failure degradation. Main retains live monitoring
and alarms with no SQLite I/O. PostgreSQL replay/verification/retirement and
migration 002 are not implemented; durable backlog remains pending and degraded
stays active. Current operation: [docs/OUTBOX.md](docs/OUTBOX.md). Evidence and
final status: [docs/RESILIENCE_STATE.md](docs/RESILIENCE_STATE.md).

The final complete clean Stage 1 gate passed all 55 commands, 2026-10-23
09:24:59–09:29:53 UTC, at `artifacts/resilience-stage1/final/`.
Four builds each passed 60 C cases (240 executions); seven Python cases,
five v1 PG contracts, real vcan/monitor/load/manual tests, 130 transaction/origin
SIGKILLs, 199 direct DB/WAL inventories, real ext4 FULL/read-only, unprivileged
systemd crash/watchdog, ASan/UBSan/TSan, scoped Helgrind, analyzer and fuzz passed.
Debug preserved 160 postcommit test readings across 20 kills; silence alarm ages
were 506 ms for a 500 ms deadline; full-storage shutdown was 1.027906 s.
Natural load accounted 48047 received + 1954 kernel drops = 50001 sends;
forced overflow accounted 223 + 49778. No physical/sustained throughput claim.
Decoder fuzzer: 93210029 inputs, reported 121 s, RSS 302 MiB, no finding.
The final review fixed checkpoint freeze across shutdown/recovery and added
its VFS fault case. Other defects, earlier attempts and limitations are in state.
Implementation SHA-256:
`706261f8fdf7f2302b185bf0ac26b0891ec71f14222c03276d63298dcf3019a2`.
The native source `/tmp/vigil-stage1-final-source` has the same digest; tested
parent is 2bc113b. Original Release baseline passed before edits. Final native
DB/WAL evidence is also archived under the final artifact root. Reviewed local
commit identity is `git log -1 --oneline`. No default service/production data was
changed; no test unit/mount/daemon remains. Historical core records follow.

**Stop after Stage 1. Do not start Stage 2.**

# Vigil state — FINAL/COMPLETE (original core release)

Checkpoint 5 and the Vigil 1.0 core release are complete. The final clean Linux
release gate passed on 2026-09-28 UTC. One Codex agent performed all work;
no subagents, delegation or other Codex instances. No next feature is authorized.

## Finished scope

The core satisfies PROJECT_BRIEF.pdf v3 with the documented architecture and
process refinements. All eight pages, every project document, the repository
tree, git history, source and tests were independently inspected. The original
clean gate passed before edits; starting commit was `8ed444a`, with a clean tree.

- Python python-can pods send temperature, door and heartbeat frames over real
  Linux SocketCAN. Exact-length/endian/CRC decoding and rolling-counter policy
  have independent literal C/Python fixtures.
- Main owns CAN/timerfd/signalfd, freshness/progress/plausibility and every live
  alarm. Hysteresis, stale/current representation, recovery and counters are
  explicit. No main-thread database calls.
- The writer alone owns libpq. Short mutex scopes copy bounded values; no I/O
  under queue locks. Pending capacity is N readings plus 256 events, with one
  additional immutable in-flight batch of at most 32 records.
- PostgreSQL schema v1 records typed history, exact known gaps, outage counters
  and complete alarm snapshots. Same-process ambiguous COMMIT retry is
  idempotent. Main memory remains the live alarm authority.
- Unprivileged systemd Type=notify, main-loop watchdog, restart, shutdown,
  configuration and startup failures are implemented and exercised.
- Ten brief fault scenarios, five added hardening scenarios, an asserted
  interview demo, independent can-utils evidence, decoder fuzzing and a
  GitHub Actions workflow with locally verified commands are complete.
- FINAL_VERIFICATION, INTERVIEW_GUIDE, RESUME_EVIDENCE and LEARNING_GUIDE provide
  actual results, defensible explanations, call paths and worked exercises.

The audit fixed SQLSTATE coercion in the Python evidence parser and expanded
the source digest to cover decoder CLI/environment files. Added Release and
lifecycle/boundary coverage. Corrected documentation claims and set version
1.0.0. No production C or SQL behavior changed; no confirmed production defect
remains unresolved. The agent-implementation deviation from brief section 7
remains explicit; automated tests cannot certify the owner's understanding.

## Final verification

Executed from the macOS repository:

```sh
./scripts/dev ./scripts/verify --clean --output artifacts/checkpoint-5-final
```

All ten commands exited 0, 2026-09-28 08:22:47.905–08:29:24.811 UTC.
Six builds each passed 13 protocol + 26 monitor + six queue cases: 270 C
executions. Seven Python cases passed twice; five PostgreSQL contracts,
real vcan/monitor/persistence/systemd integrations, ten fault cases, five
hardening cases in Release/ASan/TSan, demo and manual exchange all passed.
No required test was skipped. GCC/Clang strict builds, GCC analyzer,
ASan/UBSan/TSan had no findings; Helgrind reported zero queue-suite errors
with normal library suppressions.

- libFuzzer: 84,999,114 inputs in 121 seconds, peak RSS 301 MiB, no finding.
- Silent source: 500 ms deadline; three observed ages of 504 ms.
- Three same-process outages: 14 arrivals, nine retained, five dropped in one
  gap; exact cumulative accounting. Maximum snapshot: 16 pods, 193 entries.
- Capacity-four/eleven-arrival fixture: newest four retained, exactly seven
  dropped; monitoring and alarms continued during the outage.
- systemd crash fixture: two committed readings survived, three known queued
  readings vanished; restart observed after 1227 ms; watchdog restart also passed.
- Natural burst: 50,001 sent/received, zero kernel drops. Forced overflow:
  223 received + 49,778 drops = 50,001 sent. Persistence disabled; no sustained
  throughput, physical-bus or database-capacity claim.
- Final demo: 502 ms stale detection, 4036 ms observed DB outage, twelve SQL
  readings, seven drops in one gap, no event loss or abandonment; SQL exports
  contain 32 history rows and twelve checkpoints.

Actual raw results: `artifacts/checkpoint-5-final/`; console log:
`artifacts/checkpoint-5-final.log`. Earlier successful baseline/expanded runs
and development failures retain their separate identities. See
[final audit](docs/FINAL_VERIFICATION.md) and [generated report](docs/VERIFICATION.md).
Tested implementation SHA-256:
`b87b8f78da76c8bcfbf7c07d7f2ec26f795ca27d56bb18eedebf864479cf419b`.
Docs/state are excluded to avoid a circular report hash. The local release
commit is identified by `git log -1 --oneline` and annotated tag `vigil-v1`.
The release commit and annotated tag `vigil-v1` are pushed to the public GitHub
remote `origin` (`https://github.com/tjsingh143/Vigil.git`), where the hosted
Actions job passed on that commit. Per-command commit/tag identity
is `Codex <codex@localhost>`; global Git identity is unchanged.

## Optional Buildroot — deliberately deferred

Attempted only after the expanded core gate and evidence guides were complete.
The Linux Buildroot 2026.08 build selected PostgreSQL, systemd, Vigil and
can-utils for guest-local vcan replay, without target Python. PostgreSQL is
available; no external-database substitution was made. The explicit 1800-second
build limit expired with exit 124 before producing an image. No QEMU boot or
in-guest daemon run occurred. This is a scope/time decision, not an impossibility
claim. The entire isolated experimental source/package/build tree was removed.
Only ignored attempt logs/configuration remain. No Buildroot resume bullet.
QEMU/build prerequisites remain installed in the development VM.

## Operation and cleanup

Ubuntu 24.04.5 ARM64 Lima VM `vigil`; Linux 6.8.0-139-generic, GCC 13.3,
Clang 18.1.3, PostgreSQL/libpq 16.15, systemd 255. Global ASLR remains 2;
TSan's setarch -R applies only to its subprocess tree.

The exact README install/provision/start sequence passed: UID 999, six readings,
three heartbeats, zero rejects and clean stop. The installed binary matches the
final Debug build. The exact interview command also passed separately:

```sh
cd /Users/tj/Desktop/Vigil
./scripts/dev ./scripts/demo
```

Keep vcan0 otherwise idle. The default service is inactive/disabled; existing
16/main PostgreSQL is online. No test daemon/simulator/temporary database server
remains, and vigil-evidence.service is not found. The VM remains running.
Builds, fuzz corpora and raw evidence are ignored local outputs; no accidental
binary/secret/build output is committed. The authoritative PDF is the intended
tracked binary. The separate baseline build and incomplete Buildroot tree are gone.

## Remaining limitations and stop boundary

Memory buffering and alarms are volatile. Arbitrary crash losses cannot be
reconstructed; an unconfirmed COMMIT may already exist in SQL. No spool,
cross-process state restoration, automatic history retention or counter-exhaustion
guarantee. Shutdown has a one-second best-effort drain. Network deadlines do
not bound local credential/certificate file stalls or arbitrary scheduling.
Diagnostics can drop counted lines; regular-file I/O can block.

CRC/counters do not authenticate. Flat readings are suspicion only. Receive-time
batching can trigger the rate rule. vcan cannot establish physical CAN behavior,
hardware portability or hard real-time performance. Only Ubuntu ARM64 was
executed locally; no board/QEMU image passed. Hosted GitHub Actions passed on
x86-64 `ubuntu-24.04` for the release commit, excluding vcan and systemd.
The owner's personal code understanding remains their work, supported by the
guides; do not misrepresent agent-authored implementation or tests.

**Stop: FINAL/COMPLETE. Do not start another feature.** Buildroot, hardware,
API/dashboard, Modbus, authenticated CAN, signed backups, retention and a disk
spool remain outside this release. The release commit and `vigil-v1` are published
to the public GitHub remote; publication added no scope.
