# Vigil state

Checkpoint 4 and the complete core/evidence scope are finished. The complete
clean Linux gate exited 0 on 2026-09-26 local / 2026-09-27 UTC. One Codex agent
performed all work without subagents, delegation, or other Codex instances.
All eight pages of PROJECT_BRIEF.pdf version 3 and all required records were
read before edits. Initial commit `89f13f5` was clean. Existing clean/unit/real
vcan/PostgreSQL/systemd baselines were run first, harness regressions resolved,
and the complete baseline rerun before final checkpoint verification.

## Implemented core and evidence

- Linux C main poll loop owns CAN, timerfd, signalfd, freshness/progress/
  plausibility decisions and all live alarms. The dedicated writer owns libpq.
- A bounded value-owned queue has N pending readings (default 1024), 256 events
  and one immutable in-flight batch of at most 32 records. Queue locks never
  cover I/O. Oldest pending drops and shutdown abandonment are counted separately.
- PostgreSQL schema v1 records run identities, typed history, exact known gaps,
  outage measurements and alarm snapshots. Batch retry is idempotent after
  an uncertain COMMIT. Current alarms never depend on querying history.
- Real unprivileged systemd notify/watchdog/restart service remains implemented.
  No protocol or wire-vector changes in Checkpoint 4; production C change is
  only removal of a redundant Linux EAGAIN/EWOULDBLOCK comparison.
- First-class ten-case fault harness covers the brief table, including exact
  known crash loss under an actual systemd restart and application load/drop
  accounting. Expectations precede execution; JSON/config/raw output is retained.
- Narrated asserted interview demo exports actual SQL history/checkpoints;
  manual candump/cansend checklist and recorded exchange supplement automation.
- Decoder libFuzzer target compiles target and decoder with ASan/UBSan and uses
  30 deterministic seeds. Stronger GCC/Clang warnings and GCC -fanalyzer.
- GitHub Actions build/unit/non-vcan PostgreSQL/sanitizer/fuzz workflow. Its exact
  script passed locally. No repository publication or hosted CI run is claimed.
- `scripts/verify --clean` runs the whole gate and generates VERIFICATION.md
  from results, CTest logs, fuzzer output and inspected SQL exports. The old
  Checkpoint 1 report is preserved as CHECKPOINT_1_VERIFICATION.md.

## Exact final verification

Command from the host:

```sh
./scripts/dev ./scripts/verify --clean --output artifacts/checkpoint-4-final-4
```

All six gate commands exited 0. Five C builds each passed 13 protocol + 26
monitor + 6 queue cases = 45 (225 executions); six Python cases passed twice;
five non-vcan PostgreSQL contract cases passed. Decoder/monitor real vcan suites
passed in Debug and ASan/UBSan; persistence passed in Debug, ASan/UBSan and TSan.
Ten fault scenarios, one demo, and one can-utils exchange passed. GCC/Clang
strict builds and GCC analyzer passed; ASan/UBSan/TSan had no findings.
Helgrind reported zero errors on the six queue cases (normal library suppressions).

- Silent-pod fixture: 500 ms deadline, three observed alarm ages
  [521, 521, 521] ms; three recoveries; old values current=0 while stale.
- Capacity eight/five outage readings: five backfilled, zero loss/gaps.
- Capacity four/eleven outage readings: newest four retained, exactly seven
  dropped, one SQL gap. Live alarms/timeouts continued through both outages.
- systemd SIGKILL fixture: two committed rows survived, exactly three known
  pending readings disappeared; restart observed after
  1192 ms; watchdog also restarted the process; UID 999.
- Natural load burst: 50001 sent = 48377 received + 1624 kernel drops;
  offered rate 795872 frames/s for 0.062824 s. Not sustained throughput,
  physical CAN or database capacity. Persistence disabled. Forced pause:
  50,001 sent = 223 received + 49,778 drops, all exactly accounted.
- Demo: stale detection 519 ms; observed database outage 4049 ms;
  eight pre-outage readings + four backfilled = twelve SQL readings,
  seven lost readings in one gap, no event loss or shutdown abandonment.
  Export inspected: 32 history rows and 11 checkpoints with observation times.
- Final libFuzzer run: 56,558,767 executions in 121 seconds,
  no crash, sanitizer report or invariant failure; peak RSS 292 MiB.

Measured values vary by run. Exact commands, environment, per-build results,
run IDs, timestamps and limitations: `docs/VERIFICATION.md`. Full raw evidence:
`artifacts/checkpoint-4-final-4/`; complete console log:
`artifacts/checkpoint-4-final-4.log`. Failed development/final attempts are also
retained, not represented as successful runs.

Tested implementation SHA-256 (defined in tests/evidence.py):
`0bd3bb0dabf7145af68fcb04b78fddc3446f6bce758dbf0afcadea94f8f1d420`.
The report names the pre-commit parent plus this digest to avoid a circular
self-referential commit hash. Identify the finished local checkpoint commit
with `git log -1 --oneline`. Use per-command identity `Codex <codex@localhost>`;
do not change global identity or publish without a new instruction.

## Environment and operation

Apple Silicon macOS host; real Ubuntu 24.04.5 ARM64 Lima VM `vigil`,
Linux 6.8.0-139-generic, GCC 13.3.0, Clang 18.1.3, PostgreSQL/libpq 16.15,
systemd 255, CMake 3.28.3, Valgrind 3.22.0. Dependencies include Clang's runtime.
No Python virtual bus or mocked macOS integration. Global ASLR remains 2;
TSan uses setarch -R only for its subprocess tree.

```sh
./scripts/dev ./scripts/verify --clean
./scripts/dev ./scripts/demo
./scripts/dev ./scripts/fuzz 120
./scripts/dev ./scripts/check-ci --clean
./scripts/dev build/vigild --no-persistence
./scripts/dev .venv/bin/python -m simulator.pod
```

Use separate terminals for daemon and simulator. Do not run them or the default
service during tests on vcan0. Tests only stop their own disposable PostgreSQL
clusters. Final cleanup confirmed no test daemon/simulator/cluster remains;
temporary systemd unit is not found; installed default vigild is disabled and
inactive; the existing default 16/main PostgreSQL cluster is online. The VM
remains running. The default installed service binary was last installed in
Checkpoint 3; `scripts/install-service` installs the current build if desired.

## Findings, refinements and stopping boundary

Harness fixes address startup stale counts, stale snapshot selection and
diagnostic draining during both continuous and finite simulator lifecycle waits.
A VM stall exposed that receive-time batching can trigger the existing rate
rule. Transport/count and silence fixtures disable rate to isolate their
concern; exact zero-time and slope boundaries remain in C tests. Production
rate policy is unchanged. That real arrival-time limitation is documented.
Manual-filter readiness and unnecessary reset-failed cleanup calls were corrected
before the final successful gate. Strict diagnostics found only the Linux errno
alias redundancy; no final sanitizer/analyzer/fuzzer finding remains unresolved.

Memory buffering is not crash-durable. Pending N + 256 event + 32 in-flight
bounds, unknown post-crash loss, uncertain COMMIT, finite history and a one-second
best-effort shutdown drain remain explicit. No spool, cross-process alarm
restoration, authentication, physical CAN validation or hard real-time guarantee.
Diagnostic regular-file I/O may block and full pipes drop counted lines.
CRC/counters cannot authenticate; flatness is suspicion. vcan does not model
arbitration, bitrate or electrical behavior. Hosted CI is not yet observed.

The complete core/evidence now satisfies version 3 within the documented
queue/in-flight and deadline-driven libpq refinements, and the owner-authorized
agent-implementation process deviation. README, decisions and learning guide
explain these rather than claiming NetBotz/A-Link equivalence.

**Stop here. Do not begin Buildroot or other deferred products.** Any extension,
publication, hardware work, spool, API or dashboard requires new owner direction.
