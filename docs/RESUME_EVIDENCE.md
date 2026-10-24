# Evidence-backed Vigil wording

The owner directed one Codex agent to implement and test this project. Describe
that contribution honestly and explain the code personally before using these
suggestions. Agent-run engineering checks are not independent verification or
proof of personal understanding. The brief's process deviation remains in
[DECISIONS.md](DECISIONS.md).

| Supported project fact | Source and evidence |
| --- | --- |
| C/Linux SocketCAN sensor monitoring separates freshness, progress and plausibility; main owns live alarms | `src/{vigild,monitor,protocol}.c`; C tests and real vcan regressions |
| SQLite owner commits bounded immutable history with original identity, FULL/WAL and directory sync | `src/outbox.c`; exact crash cuts, recovered DB/WAL inventories and capacity/fault gates |
| Separate PG owner verifies exact receipts and history before local transactional retirement | `src/replay.c`, migration 002; actual PG concurrency/rollback/ambiguous-response/restart contracts |
| Durable data is retained at capacity; RAM overflow remains counted and volatile | Queue/outbox tests and adversarial local/remote union assertions |
| Unprivileged systemd readiness/watchdog/restart and real ext4 failure behavior exercised | Temporary isolated unit, ENOSPC/read-only Linux-local gates |
| Reproducible demo uses production daemon and disposable resources | `scripts/demo`: 20 offered readings reconcile, old-run replay, identical retries, current alarms, exact pending/loss counters |
| Matched original/Resilience measurements with multiple runs | [Performance report](RESILIENCE_PERFORMANCE.md), raw per-trial/probe evidence and pinned original commit |
| Strict diagnostics, sanitizers, analyzer, Helgrind and decoder fuzzing | Exact fresh commands/outcomes in [state](RESILIENCE_STATE.md) |
| Hosted CI workflow includes safe outbox units and disposable PG v1/v2 contracts | `scripts/check-ci`; local command results are separate from the published original's historical hosted status |

Suggested wording, after personal review:

- Built an agent-assisted C/Linux sensor-monitoring project using SocketCAN,
  with separate freshness, publisher progress and plausibility checks and
  main-owned live alarms.
- Added a bounded SQLite durable outbox and separate PostgreSQL replay worker;
  exercised process crashes, database outages and ambiguous commits with exact
  payload/identity comparisons and counter reconciliation.
- Prepared reproducible fault, sanitizer and benchmark gates plus a disposable
  crash/replay demonstration, documenting storage assumptions and permitted
  volatile losses.

Choose verbs matching actual personal work. A compact heading is **Vigil
Resilience — C, Linux/SocketCAN, SQLite, PostgreSQL, Python, systemd**. A numerical
performance bullet should cite the measured workload, environment and variability
in the current report, not a favorable single run or a microbenchmark as daemon
capacity. Avoid stale test counts: repeated build executions are not unique
cases, and successful finite fuzzing does not prove absence of defects.

Unsupported: independently verified, production deployed, physical CAN/bitrate
validation, power-cut qualification, hard real-time guarantees, sender
authentication, unlimited retention, exactly-once transmission, universal
throughput, Buildroot image completion or a newly green hosted candidate run.
