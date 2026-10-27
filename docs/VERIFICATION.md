# Verification

Vigil has separate portable, hosted-Linux, and local integration gates. A passing
subset does not imply that the other gates or physical hardware were exercised.

## Hosted Linux CI

The published source at `c66b28d705e44e957279dc3c168979d11b16f7a3` passed
[the hosted workflow](https://github.com/tjsingh143/Vigil/actions/runs/36601359319).
The [workflow](../.github/workflows/ci.yml) runs [scripts/check-ci](../scripts/check-ci),
covering strict GCC/Clang builds, 45 C cases per tested build, seven Python cases,
decoder CLI controls, five PostgreSQL contracts, ASan/UBSan, GCC analysis, and a
short libFuzzer budget. These counts describe the recorded hosted gate.

## Commands

```sh
./scripts/setup-linux
./scripts/check-ci --clean
./scripts/check-release --clean
./scripts/fuzz 120
```

The [fuzz target](../fuzz/README.md) is the decoder, not the full daemon. Fuzz
commands save their exact invocation, log, corpus, and any failure input under
`artifacts/fuzz-*`. A clean finite run is not a proof of correctness.

## Local integration

The release gate adds real `vcan` traffic, systemd readiness/watchdog/restart
checks, fault scenarios, and concurrency tools. See [manual CAN checks](MANUAL_CAN.md),
[monitoring](MONITORING.md), [persistence](PERSISTENCE.md), and the
[release checker](../scripts/check-release). Physical CAN timing, wiring,
arbitration, bus-off behavior, and hardware-board testing remain outside this release.

## October 11, 2026 documentation audit

The seven Python unit checks passed. The protocol/decoder compiled with GCC
strict warnings, and the literal decoder CLI vectors plus invalid-argument
controls passed. Hosted CI was checked directly on GitHub. PostgreSQL, `vcan`,
systemd, sanitizers, and full release gates were not re-run for this documentation update.
