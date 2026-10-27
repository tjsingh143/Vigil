# Contributing

## Environment

SocketCAN/`vcan`, `timerfd`, `signalfd` and systemd behaviour must be checked on
real Linux. On Apple Silicon, use the repository's Lima VM (`./scripts/dev up`,
then prefix Linux commands with `./scripts/dev`). A virtual Python bus or mocked
macOS integration is not a substitute for the Linux gates. Outbox stores and
release-gate runs need native ext4 or XFS, not the shared repository mount.

## Engineering rules

- **C**: builds use `-Wall -Wextra -Wpedantic -Werror -Wconversion` with GCC and
  Clang. Decoding is length-checked and integer and endian conversions are
  explicit. An invalid frame must not be able to modify a decoded result.
- **Protocol**: a wire change updates [docs/PROTOCOL.md](docs/PROTOCOL.md) and
  [tests/protocol_vectors.txt](tests/protocol_vectors.txt) together, so the C
  and Python implementations stay in step.
- **Thread ownership**: the main thread owns monitoring and live alarms. SQLite
  and fsync calls belong to the storage owner; libpq calls belong to the replay
  owner. Queue and mailbox locks never cover I/O.
- **Monitoring semantics**: freshness, progress and plausibility remain separate
  dimensions. Flat readings are suspicious, not proof of sensor failure.
- **Claims**: CRC and counters do not authenticate senders. `vcan` does not
  simulate physical CAN. SIGKILL tests are not power-loss tests.

## Checks

Record the expected outcome before running a check, and report only observed
results.

| When | Command |
| --- | --- |
| Routine change | `./scripts/check-ci --clean` (also runs in GitHub Actions) |
| Monitoring or bus behaviour | `./scripts/check-release --clean` |
| Release evidence | `./scripts/check-resilience --clean --output artifacts/<new-dir>` |

For release evidence, run the gate from a fresh native-filesystem clone with
`TMPDIR` inside it, and keep the host awake (`caffeinate -i` on macOS). Retain
`gate.json` and the numbered logs. Tests create disposable outboxes, PostgreSQL
clusters, loop mounts and transient units. They must never touch a developer's
real data or the installed service.

## Documentation

Documentation describes the implementation, methods, observed results and
limitations without author or tool bylines. A documentation edit must not read
as a claim that verification was re-run or that an open result was fixed.
Frozen evidence and dated records keep their original results. Behaviour
changes update [CHANGELOG.md](CHANGELOG.md) and the relevant document in
[docs/](docs/README.md).
