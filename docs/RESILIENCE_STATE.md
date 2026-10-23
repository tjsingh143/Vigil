# Vigil Resilience state

## Authorized scope and status

**Stage 0B COMPLETE — architecture/failure contract accepted by technical
self-review; ready for Stage 1 authorization.** One Codex agent, no delegation.
The owner requested architecture finalization and a safe local documentation
commit. No implementation, migration application, production data/service change,
push, publication or deployment is authorized. Owner review/approval of the final
contract is not inferred from the agent's acceptance assessment.

Normative design:
[failure contract](RESILIENCE_CONTRACT.md) (states, guarantees, T01–T20 assertions)
and [architecture](RESILIENCE_ARCHITECTURE.md) (schema, bounds, ownership,
transactions, algorithms and exact future interfaces). They describe intended
behavior, not features of the current binary. Stage 0's historical observations
remain in [RESILIENCE_BASELINE.md](RESILIENCE_BASELINE.md).

## Final decisions

- Main keeps the nonblocking CAN path and owns live alarms. Separate SQLite
  storage and asynchronous PG workers communicate through bounded copied slots.
- Local admission commits the immutable remote batch, original identities,
  sequence allocation and loss/snapshot capture together. Only FULL/WAL commit
  plus directory sync establishes local durability. Queue publication is volatile.
- New process means new origin; old work retains its old run/boot/time identity.
  PG verifies complete origin/content, including exact persisted batch bytes.
- Migration 002 is required and additive; 001 and existing history keys remain.
  Remote commit precedes atomic local retirement. Unknown outcomes retry unchanged;
  no exactly-once transmission promise, durable eviction or poison-head skipping.
- Limits, checkpointing, fault/start/stop behavior and /var/lib/vigil state path
  are fixed in the architecture. Replay does not restore current sensor state.

## Evidence reconciled and checks actually run

Starting Stage 0B HEAD: `60c3be2` on `main`, clean tree. Independently read the
full v3 brief, required repository records, monitoring/persistence contracts and
relevant C/SQL/build/unit/tests. Read the saved Stage 0 gate JSON and recomputed
the current implementation digest using `tests/evidence.py:source_digest`:

`b87b8f78da76c8bcfbf7c07d7f2ec26f795ca27d56bb18eedebf864479cf419b`.

It matches that gate; all ten saved command exit codes are zero. The complete
clean Linux gate **ran in Stage 0**, 2026-10-23T01:45:02.585 to 01:51:08.021 UTC;
it was **not rerun in this documentation-only stage**. No outbox test or power
cut has been executed. Prior measured counts/results are linked in the baseline,
not reclassified as new resilience evidence.

Evidence root: `artifacts/resilience-stage0-20261023T000920Z/`, primary gate at
`full-source/artifacts/stage0-full/gate.json`. Existing builds/reports/raw evidence
are preserved. Stage 0B expectations and documentation validation are under
`artifacts/resilience-stage0b/`. Checks: local Markdown links, complete T01–T20
matrix, source/scope preservation, saved-gate/hash reconciliation, diff review
and `git diff --check`. These are design/document checks, not runtime verification.

Read-only VM inspection confirmed Linux `6.8.0-142-generic`; SQLite CLI and
pkg-config development package are absent. No packages were installed. The
installed runtime library is `libsqlite3-0 3.45.1-1ubuntu2.8`. The
design requires SQLite >=3.51.3 (including WAL-reset fix), native Linux state
storage and later scoped dependency provisioning. The VM's installed vigil/PG
services and user databases were not test targets in Stage 0B.

## Review and remaining work

Reviewed local commit/ack ambiguity, repeated remote-commit SIGKILL, FIFO replay,
mailbox ownership, starvation, disk/WAL/memory ceilings, full/corrupt storage,
restart origins, signed integer exhaustion, snapshot/loss capture and old-run
acknowledgement races. PG uses a transaction advisory run lock so content
verification does not require broadening the runtime role to table UPDATE.
No unresolved product-level architecture issue remains. All durability and
performance claims still require implementation and the specified tests.

The three cached upstream documentation-only commits delete required records.
Reconciliation decision: preserve the local audited source/documents and use
this design commit as the prospective implementation base; do not pull those
deletions into it. No source divergence or code merge is needed for this design.
This resolves the baseline's base-selection question without rewriting history.

Stage 1's proposed scope is only the owner-only SQLite module, build dependency
and local tests; later daemon/replay/schema/service integration requires its own
authorization. Dependency setup and software crash/fault tests remain work, not
evidence blockers for the completed design. Actual power-cut qualification is
separate and unclaimed. **STOP: do not implement SQLite or start Stage 1 without
explicit owner instruction.** Local commit only; no push/publication/deployment.
