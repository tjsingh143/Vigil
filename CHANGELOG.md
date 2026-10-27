# Changelog

## 2.0.0 (2026-10-27), tag `vigil-v2.0.0`

History becomes durable across process crashes and PostgreSQL outages. The
wire protocol, decoder, monitoring policy and alarm semantics are unchanged.
Startup and provisioning change, so this is a major release.

### Added

- SQLite durable outbox (`src/outbox.c`) on a dedicated owner thread: WAL with
  `synchronous=FULL` and parent-directory fsync, immutable batches carrying
  run, order and sequence identity, fixed limits of 65,536 records, 4,096
  batches and 64 MiB. Startup validation covers schema, `quick_check`, foreign
  keys, allocators and usage. Corrupt or inconsistent stores fail closed.
- PostgreSQL replay worker (`src/replay.c`): oldest batch first through one
  replay slot, asynchronous libpq with two-second deadlines, 1–30 s backoff,
  identical-byte retry after an unknown outcome, and a poison latch for
  conflicting or malformed content.
- Migration `002_resilience.sql`: `replay_batch(bytea)` and
  `validate_replay(bytea)`, origin verification, exact byte receipts and
  `VG001`/`VG002` errors. It is additive; `001_history.sql` is unchanged.
- `vigil-outbox-init` and `scripts/setup-outbox` for explicit store provisioning.
- `scripts/setup-sqlite`: builds SQLite 3.51.3 under `/opt/vigil-sqlite-3.51.3`
  after verifying the SHA3-256 of `sqlite3.c`, and refuses optimized Python.
- `--outbox-path` option and an `outbox` status line.
- Test suites: outbox units, a SQLite fault VFS and fail points, local and
  remote crash-cut integration, adversarial fault schedules, storage faults
  (`ENOSPC`, read-only), recovery-coordinator cases, PostgreSQL v2 contracts.
- `scripts/check-resilience`, the 72-command release gate. `scripts/demo` and
  `scripts/benchmark` provide the production-binary demonstration and the matched
  1.0 versus 2.0 benchmark.

### Changed

- The volatile PostgreSQL writer is replaced. All history passes through the
  outbox, and PostgreSQL receives it only by verified replay.
- `vigild` requires an existing, validated outbox (default
  `/var/lib/vigil/outbox.sqlite3`) and never creates one implicitly. An invalid
  store exits with status 2 before systemd readiness.
- systemd unit: `StateDirectory=vigil` (mode 0700), start timeout 30 s.
- `scripts/setup-database` applies migrations 001 and 002 and grants only
  `SELECT`/`INSERT` plus `EXECUTE` on the replay functions.
- Python verification uses explicit checks instead of `assert`. Every test
  entry point refuses an optimized interpreter.
- Hosted CI builds SQLite and runs the PostgreSQL v2 contracts.

### Upgrade

Apply migration 002 and its grants, provision an outbox, then start the new
binary. The 1.0 binary cannot read an outbox, so rolling back needs an explicit
plan to drain or archive it. See
[RESILIENCE_RELEASE.md](docs/RESILIENCE_RELEASE.md#upgrade-and-rollback).

### Known limitations

No power-cut, physical CAN or XFS qualification. Replay measured about 74
readings/s against a 100 readings/s offered load on the test VM. The SQLite
installer checks `sqlite3.c`, not the whole archive. See the
[README](README.md#limitations) and the
[audit summary](docs/RESILIENCE_AUDIT.md#nonblocking-findings).

## 1.0.0 (2026-09-28), tag `vigil-v1`

- Linux monitoring daemon in C: SocketCAN, `timerfd` and `signalfd` poll loop;
  systemd `Type=notify` and watchdog.
- Decoder with CRC-8 and rolling counters. Freshness, progress and
  plausibility tracked separately; hysteresis alarms.
- Bounded in-memory queue and a PostgreSQL writer thread with backfill and gap
  records (`001_history.sql`). A process crash could lose queued records.
- Python CAN simulator and cross-language protocol vectors.
- cmocka units, fault scenarios, PostgreSQL contracts, ASan/UBSan, TSan,
  Helgrind, GCC analyzer, libFuzzer and hosted CI.
