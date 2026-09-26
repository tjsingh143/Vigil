# Vigil state

Checkpoint 3 is implemented, reviewed and documented. Its final complete clean
Linux regression passed on 2026-09-26. Checkpoint 4 has not started.
One Codex agent performed all work without delegation or other Codex instances.
The full eight-page version 3 brief and all required records were reread before
code changes. Starting commit `aa785cb` was clean; its complete clean gate and
a short real vcan simulator/daemon smoke passed before implementation.

## Implemented through Checkpoint 3

Checkpoint 1/2 protocol, monitoring decisions, cross-language vectors and real
Linux integration remain. Persistence now adds:

- Dedicated C pthread owns libpq. Main's poll/CAN/timer/signal loop and current
  alarms never call PostgreSQL or consult it as the source of current truth.
- Mutex-protected value-owned FIFO lanes: N pending readings (default 1024),
  256 events, plus one immutable writer batch of at most 32 records. Exact
  oldest-pending drops, high water, depths and in-flight counters; no queue
  lock covers network/SQL/formatting I/O. Condition wait predicates share the
  publication lock; retry wakeups cannot bypass the one-second backoff.
- PostgreSQL schema v1 with run UUIDs, readings/heartbeat/rejection/alarm
  history, and atomic batch checkpoints carrying gaps, outage evidence and
  complete current-alarm snapshots. Lost responses retry the identical payload
  and batch ID without double writes or double acknowledgement.
- Client connection/query deadlines of two seconds, server statement timeout
  1000 ms and lock timeout 500 ms, numeric/Unix endpoints. All database calls
  stay in the writer. Async libpq explicitly enforces connect_timeout intent.
- Main owns persistence-degraded, raised on failure/overflow/startup and cleared
  after catch-up. Readable status exposes exact queue/write/drop/failure/outage/
  gap/snapshot counters, SQLSTATE, and shutdown abandonment.
- One-second shutdown drain, join-before-destroy ordering. A crash loses memory;
  new runs never pretend to restore vanished history or exact old-run losses.
- Real systemd Type=notify/WatchdogSec=4s service, unprivileged account, hardening,
  on-failure restart, reproducible install/schema setup with preserved config.
- Six queue/health cmocka cases (45 total), including 20,000 concurrent publications;
  real PostgreSQL/vcan/outage/slow-query/lost-response/crash/service gates.
  Strict Debug, ASan/UBSan, TSan and Helgrind are part of scripts/check.

The pending capacity plus immutable batch is an explicit refinement of the
brief's queue-capacity wording, required to handle uncertain COMMIT outcomes
honestly. The switch from blocking to deadline-driven libpq within the writer
is also recorded in docs/DECISIONS.md. Neither changes the central ownership
invariant. Detailed contract: docs/PERSISTENCE.md.

## Environment and commands

Host: Apple Silicon macOS. Real Linux gate: existing `vigil` Lima VM,
Ubuntu 24.04 ARM64, Linux 6.8.0-139-generic, GCC 13.3.0, cmocka 1.1.7,
Python 3.12.3/python-can 4.6.1, PostgreSQL/libpq 16.15, systemd 255,
Valgrind 3.22.0. No macOS SocketCAN mock or Python virtual bus.

```sh
./scripts/dev ./scripts/check --clean
# Monitoring-only foreground demo:
./scripts/dev build/vigild --no-persistence
./scripts/dev .venv/bin/python -m simulator.pod
# Reproducible default service/database installation:
./scripts/dev ./scripts/install-service
./scripts/dev ./scripts/setup-database
./scripts/dev ./scripts/vcan-up
./scripts/dev sudo systemctl enable --now vigild
```

Do not run the service/simulator concurrently with tests on the same vcan0.
Tests create disposable clusters (including a local trust-auth test endpoint),
never stop the default/user cluster, and clean up their services/processes.
The default local installation uses Unix peer auth as OS/DB account vigil.
The installer and default database provisioning were exercised (schema setup
twice) without enabling the installed service. The VM and default PostgreSQL
package service remain available. No remote publication.

TSan requires `setarch aarch64 -R` for its test subprocesses on this VM; the
clean gate applies it. Global ASLR remains enabled. Shared .venv/build outputs
are Linux files and must not be executed directly on macOS.

## Observed verification

The final `./scripts/dev ./scripts/check --clean` exited 0 after all review
fixes. Debug, ASan/UBSan and TSan passed all 45 cmocka cases; all six Python
unit tests passed. Both Checkpoint 1/2 real vcan variants passed unchanged
monitoring expectations. All three builds passed the PostgreSQL/vcan suite,
including direct SQL assertions, and real systemd tests passed. Helgrind
reported zero errors on the six queue/health tests. No final sanitizer findings.

- Capacity eight/five outage readings: all five backfilled, no drops/gap.
- Capacity four/eleven outage readings: four newest retained, exactly seven
  dropped and recorded in one gap. Current high/stale alarms restored; a
  cooler-reading outage also verified correctly cleared alarm state.
- Observed outages 2021..2037 ms across the final matrix; communication alarms
  fired at 511..521 ms with a 500 ms timeout while PostgreSQL was stopped.
- Table-lock and server-statement timeouts produced SQLSTATE 55P03 and 57014;
  sensor/timeouts continued and retained records backfilled afterward.
- Lost real COMMIT response: idempotent retry, exactly five SQL readings and
  five acknowledged readings. Event overflow: 300 paced readings/capacity four,
  exactly 296 reading drops; 46/47/46 event drops matched SQL per build.
- Unresponsive TCP shutdown 1.043/1.052/1.072 s, exactly three abandoned readings.
  SIGKILL during a known DB outage lost exactly three unsubmitted readings,
  retained two earlier SQL rows, and started a distinct new run without backfill.
- systemd UID 999, readiness/watchdog, two restarts (SIGKILL and stopped-main
  watchdog), clean outage stop in 1.058 s. Installed default peer-auth service
  then persisted six readings/three heartbeats and stopped with exit 0.
- Test clusters/temporary units/processes cleaned up. Installed vigild remains
  disabled/inactive, final binary installed; default PostgreSQL remains online.
  Smoke history is retained. Global ASLR remains enabled (value 2).

Full expectations, measurements, commands, corrected harness assumptions,
review fixes and limitations: `docs/CHECKPOINT_3_VERIFICATION.md`.
Local full final transcript: `artifacts/checkpoint-3-final.log` (ignored).

## Limits and exact stopping boundary

Memory-only buffering, finite event history, at most 32 additional in-flight
records, one-second best-effort shutdown drain, and possible uncertain COMMIT
outcomes are explicit. Database row durability depends on PostgreSQL/storage.
No spool, config reload, cross-process alarm restoration, retention/analytics,
authenticated CAN, physical bus testing or hard real-time guarantee. Diagnostic
regular-file output can still block as documented in Checkpoint 2; full pipes
drop counted lines. CRC/counters cannot authenticate sources; flatness is only
suspicion. vcan does not model physical arbitration/bitrate/electrical behavior.

Checkpoint 3 is ready for the next authorized checkpoint. Identify its local
checkpoint commit with `git log -1 --oneline`. Do not start Checkpoint 4. Later work requires owner authorization;
likely evidence work includes the brief's decoder fuzzer, CI and scripted demo,
not automatically a dashboard/API/Buildroot extension.

Use per-command Git identity `Codex <codex@localhost>` as prior checkpoints did;
do not change global identity or publish. Identify the final checkpoint commit
with `git log -1 --oneline`.
