# Checkpoint 1 verification

## Written expectations

These expectations are encoded in tests before execution:

| Check | Expected behavior |
| --- | --- |
| Strict Debug build | Compile and link without warnings/errors |
| CRC | SMBUS check F4; zero empty/residue; single-bit changes rejected |
| Shared vectors | Both languages match all ten literal frames |
| Decoder | Exact lengths/IDs/enums, int16 domain, binary door, immutable output on rejection |
| Counters | First, next, repeat, gaps, wrap and half-range ambiguity classified explicitly |
| Two pods | Exactly 18 frames in three cycles; separate pod IDs and expected reading/heartbeat counters |
| Malformed Linux traffic | Length/CRC/ID/EFF/RTR/channel/status fail by specific reason |
| Shutdown | SIGINT and SIGTERM stop the simulator within the test's 2-second deadline |
| Invalid CLI inputs | Fail nonzero without crashes or sanitizer errors |
| ASan/UBSan | Same unit/integration behavior, no reported memory/undefined behavior defects |

## Observed environment

2026-09-25: Apple Silicon Mac, macOS 26.5.1, Lima 2.2.0. Guest reports Ubuntu
24.04.5 LTS, aarch64, Linux `6.8.0-139-generic`, systemd PID 1. GCC 13.3.0,
CMake 3.28.3, cmocka 1.1.7, Python 3.12.3, python-can 4.6.1. vcan module
provided by `linux-modules-extra-6.8.0-139-generic`.

`./scripts/dev up` created/provisioned the VM successfully. A raw Linux CAN
socket independently received `cansend vcan0 123#11223344` byte-for-byte
before simulator integration was attempted. `pip check` found no broken
requirements. No existing code/build was available to baseline.

`limactl stop vigil` followed by `./scripts/dev up` also passed: the same VM
restarted, provisioning was idempotent, and `vcan0` was recreated after reboot.

## Executed gate

`./scripts/dev ./scripts/check --clean` passed the Debug and ASan/UBSan C
builds, 13 cmocka cases in each build, six Python unittest cases, and the full
Linux integration gate for both C builds. Sanitizer settings include leak
detection and halt-on-error. No sanitizer reports occurred.

Both integration runs observed exactly 18 valid simulator frames and all
seven malformed cases with the expected reason. The first cycle included:

```text
101#0098080000B7  pod=1 temperature=2200 counter=0
101#010000000165  pod=1 door=0 counter=1
701#01000000000029 pod=1 operational uptime=0 heartbeat_counter=0
102#0098080000B7  pod=2 temperature=2200 counter=0
102#010000000165  pod=2 door=0 counter=1
702#01000000000029 pod=2 operational uptime=0 heartbeat_counter=0
```

## Manual observer checklist

The automated gate drives can-utils too. Additional manual observer checks
were executed in the same VM on 2026-09-25:

| Commands (inside Linux) | Expected | Actual |
| --- | --- | --- |
| Start `candump -L vcan0`, then `.venv/bin/python -m simulator.pod --pod-id 1 --pod-id 2 --cycles 1` | Six frames: temperature, door and heartbeat for each pod | All six literal first-cycle frames above observed by candump |
| Start `build/vigil-decode --listen vcan0 2`, then `cansend vcan0 101#0098080000B7` and `cansend vcan0 101#0098080000B6` | First reading accepted; second rejected as CRC | JSON results `ok` with value 2200, then `crc`; exit 1 |

The observer processes were terminated after these checks. The README has
the general three-terminal procedure. Integration logs are reproducible by
rerunning the gate; generated build directories/logs are not committed.

Review caught and fixed an optimized-Python hazard: an `assert` cannot guard
against assertions being disabled. The gate now explicitly raises an error
under `-O` or `PYTHONOPTIMIZE`. Numeric CLI parsing also rejects every leading
whitespace character rather than just spaces. Both fixes have regression
checks in the integration gate. The full clean gate was rerun after review
and VM restart.

## Limits of the evidence

Only Ubuntu ARM64 was exercised. No physical bus, load benchmark, CI, fuzzer,
daemon health behavior, database, persistence, systemd service or crash-recovery
claim is supported by these results. The finite checks are a foundation, not
proof that the entire product promise is implemented.
