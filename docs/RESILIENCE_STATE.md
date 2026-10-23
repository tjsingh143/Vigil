# Vigil Resilience state

## Authorized scope

Stage 0 baseline audit only, by one Codex agent. The owner has not authorized
Stage 1, SQLite implementation, daemon changes, PostgreSQL schema changes,
push/publication/deployment or live user-database/service testing.

Audited baseline: `d179681a40497ada533d2f61e50b5ac60a6ca293` on `main`,
initially clean and three commits behind origin/main. Original source, earlier
reports, ignored builds and evidence are preserved. See
[RESILIENCE_BASELINE.md](RESILIENCE_BASELINE.md) for source lines and contracts.

## Status

**Stage 0 PASS — audit complete; ready for architecture finalization.**
Read-only starting-state audit, implementation/failure-contract trace and risk
map are complete. The primary complete clean Linux gate ran all ten commands
successfully, 2026-10-23T01:45:02.585 to 01:51:08.021 UTC. Six builds passed
270 C case executions; seven Python cases passed twice; PostgreSQL/vcan/
systemd integrations, ten fault scenarios, five hardening cases in three
variants, demo/manual exchange and 94,125,524 fuzz inputs passed. No existing
mandatory gate was skipped. No sanitizer/analyzer/Helgrind findings reported.
Tested implementation SHA-256:
`b87b8f78da76c8bcfbf7c07d7f2ec26f795ca27d56bb18eedebf864479cf419b`.

Evidence: `artifacts/resilience-stage0-20261023T000920Z/`. First CI attempt is
retained with boot clock-skew warnings. Fresh stable-clock CI/Release/TSan/
Helgrind/fuzz results have separate logs and `unprivileged-gate.json`.
The primary gate uses `full-source/artifacts/stage0-full/` under that root,
with console `full-gate.log`. Expectations preceded test execution.

## Environment, approvals and blockers

Existing Ubuntu ARM64 Lima VM was initially stopped, then started. Kernel
6.8.0-142 lacked vcan after the kernel update. The owner approved disposable
vcan setup, the exact matching module package, and the existing temporary
systemd fixture. Module installation and vcan setup succeeded. No installed
vigild or user PostgreSQL service/database is a test target.

No verification/environment blocker remains. Cleanup verified no temporary
test processes, clusters or service paths; installed vigild remains inactive/
disabled, bus idle, global ASLR 2. VM remains running with the approved module
package and vcan0 up. Original reports/builds and raw evidence are preserved.
Only audit/state/learning documentation changes are made and locally committed;
no production code, SQL, wire contract or workflow is changed.

Architecture questions in the baseline remain decisions for the next stage;
they are not implemented assumptions. Combined ambiguous remote commit plus
process death, local-spool/power-loss behavior and physical CAN are outside
existing tests and are not claimed. The three upstream documentation commits
remain unapplied; reconcile them before selecting a later implementation base.

## Next stage and stop

Architecture finalization must settle durable
admission, stable event/run/batch identities, local/remote scheduling, replay/
acknowledgement, bounded storage/failure policy, service paths and crash tests.
No implementation stage is authorized. Stop after the Stage 0 audit documents
and any safe local documentation commit; never push them without authorization.
