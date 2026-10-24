# Demonstrating Vigil Resilience

Vigil is a C/Linux sensor monitor with three owners: main owns live alarms,
SQLite commits local history, and libpq replays it to PostgreSQL. Its queue is
volatile. A successful FULL/WAL transaction plus directory sync establishes the
local crash-survival boundary. Remote history is verified by original identity
and exact receipt bytes before local retirement. Retries can transmit more than
once while producing one logical effect. This is a custom CAN protocol, not
A-Link, CANopen compatibility or sender authentication.

Implementation and tests were agent-authored at the owner's direction. Explain
the code personally before using any resume wording; passing automation does not
certify understanding or independent verification.

## Run and narrate

After [Linux setup](RESILIENCE_RELEASE.md), from the repository root:

```sh
./scripts/dev ./scripts/vcan-up
./scripts/dev cmake -S . -B build-resilience-release -DCMAKE_BUILD_TYPE=Release
./scripts/dev cmake --build build-resilience-release --parallel 2
./scripts/dev ./scripts/demo --build build-resilience-release
```

Keep vcan0 idle except for this command. It uses the production daemon and its
own disposable native SQLite store and PostgreSQL cluster. It never stops the
installed PG server. The closing path contains `results.json`, daemon stdout,
stderr/config, stopped SQLite inventories and `remote.json` with full exact
receipts, history, checkpoints and origins. Assertions fail the command; do not
narrate success after a failed result. Batch/history totals may vary as alarm
transitions and grouping depend on scheduling; the saved final counters are exact
for that run. The offered reading total is fixed at 20.

| Step | Evidence to point at |
| --- | --- |
| 1. Healthy sensors | Three cycles: six readings and three heartbeats, current values |
| 2. SQLite admission | Six locally admitted readings and confirmed remote retirement; distinguish RAM publication from durability |
| 3. Stop PG | Only the disposable cluster stops; degraded is raised |
| 4. Continue monitoring | Twelve high readings enter durable backlog; subsequent silence marks the value stale while high remains active |
| 5. SIGKILL | Local completion precedes kill; there is no shutdown-handler explanation for survival |
| 6. Restart before PG | New run starts with unknown live measurements; durable backlog remains |
| 7. Original records present | A brief clean stop permits supported offline byte/identity audit, followed by reopening the same store |
| 8. Restore PG | Route this fixture through a response-withholding proxy |
| 9. Old-run replay | Independent SQL confirms original receipt bytes while the client receives no success |
| 10. Ambiguous duplicate | At least two identical submissions, one remote logical effect, local head retained; a second SIGKILL demonstrates restart-safe equality |
| 11. Current alarms/history | Direct healthy restart drains old work; two new readings raise then clear current high; replay never initializes live values |
| 12. Exact reconciliation | 20 remote readings; lifetime admitted = retired; pending records/batches/bytes zero; known gap losses zero; final volatile abandonment zero |

Do not infer zero arbitrary event loss across a kill from a zero saved gap.
All offered demo readings reconcile, but unsaved crash-time counters/events may
be unknowable. An observed detection age includes Linux scheduling and timer
quantization. SIGKILL is not power-loss testing. Resource teardown runs on
normal/error/SIGINT/SIGTERM exits; SIGKILL of the harness requires scoped manual
cleanup from fixture paths in the logs.

## Explain the hard parts

- Trace `queue_take_local → outbox_append → outbox_peek → replay_batch → outbox_retire`.
  Copies and synchronization establish ownership; FULL/directory sync establish
  local durability; validated PG success permits retirement.
- Explain an unknown COMMIT: timeout does not prove rollback. Retry original
  bytes, verify `equal`, then retire transactionally. Different bytes under the
  same identity produce VG001 and block FIFO until operator investigation.
- Explain old/current run counters: lifetime retirement includes recovered runs;
  current-run written/loss/revision cursors do not inherit old live health.
- Explain capacity: N readings, 256 events and one held candidate in RAM; durable
  record/batch/payload ceilings are shared. The first exhausted limit stops
  admission; durable history is never evicted. RAM can still overflow behind it.
- Explain stale versus high versus flat: silence expires freshness; only new
  usable evidence clears a high alarm; flat values are suspicion, not proof.
  CRC and counters detect anomalies but do not authenticate.
- Explain performance boundaries using [measured results](RESILIENCE_PERFORMANCE.md):
  direct core-call latency, single-owner SQLite API timing, and paced daemon
  throughput/recovery are different measurements. No hard real-time, physical
  CAN or universal-capacity claim follows.

[RESILIENCE_RELEASE.md](RESILIENCE_RELEASE.md) covers architecture, setup,
capacity, upgrade/rollback, storage assumptions and review handoff;
[RESILIENCE_FAULT_TESTS.md](RESILIENCE_FAULT_TESTS.md) maps failure reproduction.
The original volatile-writer demo remains `tests/interview_demo.py` for the
pinned original binary only; its eleven-arrival/seven-loss story is not the
current outbox contract.
