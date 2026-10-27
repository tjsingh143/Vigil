# Vigil documentation

The [project README](../README.md) covers the architecture, setup and results.
The documents below hold the detail, grouped by use.

## Design and contracts

| Document | Contents |
| --- | --- |
| [RESILIENCE_CONTRACT.md](RESILIENCE_CONTRACT.md) | Failure contract for 2.0: event states and transaction boundaries, invariants, accounting equations, deterministic failure controls, acceptance matrix |
| [RESILIENCE_ARCHITECTURE.md](RESILIENCE_ARCHITECTURE.md) | Outbox design: thread owners and handoffs, identity, SQLite schema, transactions, bounds, PostgreSQL migration 002 and the replay algorithm, with dated implementation amendments |
| [PROTOCOL.md](PROTOCOL.md) | CAN wire format, CRC-8, counter ambiguity, cross-language vectors |
| [MONITORING.md](MONITORING.md) | Configuration, freshness, progress, plausibility, alarm lifecycle, diagnostic counts |

## Operation

| Document | Contents |
| --- | --- |
| [OUTBOX.md](OUTBOX.md) | Provisioning, startup validation, limits, status counters, error classes, shutdown accounting |
| [RESILIENCE_RELEASE.md](RESILIENCE_RELEASE.md) | Reproducing from a clean checkout, upgrade and rollback, capacity planning, power-loss assumptions |
| [MANUAL_CAN.md](MANUAL_CAN.md) | Direct `can-utils` checks of frames and decoder behaviour |
| [PERSISTENCE.md](PERSISTENCE.md) | The 1.0 PostgreSQL writer contract, superseded by OUTBOX.md and kept for reference |

## Verification and evidence

| Document | Contents |
| --- | --- |
| [VERIFICATION.md](VERIFICATION.md) | Which gate covers what, hosted versus local, how to reproduce |
| [RESILIENCE_AUDIT.md](RESILIENCE_AUDIT.md) | The three independent audits, findings F1–F5, final approval and its limits |
| [RESILIENCE_FAULT_TESTS.md](RESILIENCE_FAULT_TESTS.md) | Adversarial fault gate: crash cuts, storage faults, scenario coverage |
| [RESILIENCE_PERFORMANCE.md](RESILIENCE_PERFORMANCE.md) | Matched 1.0 versus 2.0 benchmark, method and measurement boundaries |
| [CODE_SAMPLES.md](CODE_SAMPLES.md) | Verbatim 1.0 source excerpts pinned to a commit |
| [fuzz/README.md](../fuzz/README.md) | Decoder libFuzzer target and corpus |

## Historical records

These are dated records of how 2.0 was qualified. Status lines inside them
describe the point at which each was written; later results are in
RESILIENCE_AUDIT.md.

| Document | Contents |
| --- | --- |
| [RESILIENCE_BASELINE.md](RESILIENCE_BASELINE.md) | Stage 0 audit of the 1.0 code before the extension began |
| [RESILIENCE_REMEDIATION.md](RESILIENCE_REMEDIATION.md) | Correction of audit findings F1–F4 (2026-10-25/26) |
| [RESILIENCE_F5_REMEDIATION.md](RESILIENCE_F5_REMEDIATION.md) | Correction of re-audit finding F5 (2026-10-27) |

## Evidence paths

Many documents cite paths under `artifacts/`. That directory is the local
evidence archive: gate logs, `gate.json` files, SQLite and PostgreSQL
inventories and audit probes. It is excluded from Git because of its size
(about 3.4 GB). The final release gate's complete output is attached to the
[2.0.0 release](https://github.com/tjsingh143/Vigil/releases/tag/vigil-v2.0.0);
earlier runs are retained locally.

Development-process files are not part of the published tree: state journals,
the decision log, the project brief and study notes. Their full history remains
in Git; for example, `git show e244c72:docs/DECISIONS.md` prints the final
decision log.
