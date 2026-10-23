# Current work — Resilience Stage 0 audit

The owner authorized a documentation-only baseline audit on 2026-10-22.
Current stage evidence, blockers and stop boundary live in
[docs/RESILIENCE_STATE.md](docs/RESILIENCE_STATE.md); source integration and
failure contracts live in [docs/RESILIENCE_BASELINE.md](docs/RESILIENCE_BASELINE.md).
No SQLite, daemon or PostgreSQL schema implementation is authorized. The
completed release record below remains historical and is preserved.

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
