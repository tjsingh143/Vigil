# Final engineering verification — Vigil 1.0

## Scope and ground truth

One Codex agent performed this audit, without subagents or other Codex instances.
All eight pages of `PROJECT_BRIEF.pdf` v3, `AGENTS.md`, `CODEX_STATE.md`, every
project document, all tracked source/tests/scripts, the full repository file
inventory, git status and four-commit history were inspected before edits.
The starting tree was clean at `8ed444a`; no remote was configured.

The core meets the brief's monitoring, persistence, service and evidence scope
with the already-documented immutable in-flight batch and asynchronous-libpq
refinements. Section 7's owner-first coding process was explicitly replaced
by agent implementation at the owner's direction. Owner understanding remains
personal work, supported by the learning/interview guides. Hosted CI and
hardware execution are outside the observed evidence.

## Independently established baseline

Before changing code, this command passed all six original gate commands:

```sh
./scripts/dev ./scripts/verify --clean --output artifacts/checkpoint-5-baseline
```

UTC interval: 2026-09-27 14:32:44.759–14:38:17.506. The original five C builds
passed 45 cases each, six Python cases passed twice, all ten brief fault
scenarios and the demo/manual exchange passed. The full real Linux
PostgreSQL/systemd/vcan, ASan/UBSan, TSan, Helgrind and GCC analyzer gates passed.
Baseline libFuzzer: 72,011,954 executions in 121 seconds, no finding.
Raw results: `artifacts/checkpoint-5-baseline/gate.json` and adjacent logs.
A separate Release build in `/tmp/vigil-release-baseline` also passed all 45
C cases before the release gate was expanded.

## Audit findings and changes

- **Fixed evidence fidelity:** the Python diagnostic parser coerced SQLSTATE
  `08001` into integer `8001`. A direct regression failed before the fix.
  `last_error` now remains a string, covered by a new unit test and real
  connection/permission failure assertions. Production output was correct.
- **Fixed evidence identity coverage:** the implementation digest omitted
  `tools/decode.c` and the environment definition. It now covers `tools/`,
  `env/` and any retained embedded packaging. A controlled decoder-source
  change was verified to change the digest. Earlier hashes retain their
  original algorithm and must not be compared as if their coverage were equal.
- **Closed coverage gaps:** added optimized Release integration, repeated
  same-process database outages, rapid counter restart/silence recovery,
  denied-function recovery, all 193 alarm snapshot entries, and ten invalid
  startup settings. The five new scenarios pass under Release, ASan/UBSan and
  TSan, with counts, SQL state and diagnostic output retained.
- **Corrected documentation:** an old diagnostics paragraph said no event
  history was retained; it now distinguishes the diagnostic replay limitation
  from PostgreSQL history. Historical checkpoint scope is labeled historical.
  Client network deadlines are qualified for local filesystem/scheduler stalls.
- Added `INTERVIEW_GUIDE.md`, `RESUME_EVIDENCE.md`, and concrete code-path and
  hand-tracing exercises in `LEARNING_GUIDE.md`. Set the CMake release version
  to 1.0.0. Wire version and all literal fixtures remain unchanged.
- No production C defect was confirmed by this audit or the completed dynamic
  checks. No production C or SQL behavior was changed. A finite clean run is
  evidence, not proof of no possible defect.

## Review coverage and conclusions

| Area | Reviewed contract and evidence |
| --- | --- |
| C correctness | Length-before-indexing; error-output immutability; CRC parameters; explicit endian/signed conversions; wrap/half-range ordering; bounded rate arithmetic; allocation/descriptors/cleanup; strict GCC/Clang and analyzer |
| Monitoring | Separate per-channel/heartbeat freshness, decoded-traffic reset policy, invalid-value retention, rate and flat boundaries, lifecycle/severity/hysteresis, timer quantization and recovery |
| Concurrency | Main-owned alarms, sole libpq owner, copied payloads/snapshots, mutex scopes, condition predicates/backoff, synchronized counters, stop/join before destruction; TSan and Helgrind |
| Persistence | N+256+32 bound, oldest pending eviction, exact conservation, ambiguous COMMIT retry, atomic SQL, grants, gap counts/times, reconnect snapshots, client/server deadlines, crash and abandonment |
| Service | Actual Type=notify, unprivileged UID, watchdog fed by main, SIGKILL/watchdog restart, missing-CAN failure, DB-down readiness and startup settings |
| Tests | Assertions enabled; explicit written expectations; independent wire vectors/can-utils observations; SQL queries rather than log-only persistence claims; complete snapshots; exact frame/drop accounting |
| Claims | No CRC authentication, flatness proof, physical-vcan overload, crash-durable queue, A-Link compatibility, unmeasured throughput or broad hardware claim |

## Expanded clean core gate

```sh
./scripts/dev ./scripts/verify --clean --output artifacts/checkpoint-5-core
```

Passed all ten commands, UTC 2026-09-27 14:42:56.507–14:49:22.901:

- Six strict C builds, including Release, passed 13 protocol + 26 monitor +
  six queue cases each: 270 executions. Seven Python cases passed twice.
- Five non-vcan PostgreSQL contracts, real decoder/monitor integration in
  Debug/ASan/Release, and persistence integration additionally under TSan passed.
- Ten brief fault scenarios, five new scenarios in three variants, the narrated
  interview demo and manual can-utils exchange passed without skipped cases.
- ASan/UBSan/TSan and GCC analyzer reported no findings. Helgrind reported zero
  errors on the concurrent queue suite with its standard library suppressions.
- libFuzzer executed 76,299,139 inputs in 121 seconds, peak RSS 292 MiB,
  without a crash, sanitizer report or invariant failure.
- Silent-source fixture alarm ages: [502, 502, 502] ms at a 500 ms deadline.
  New rapid-restart fixtures measured 501–522 ms across three variants.
- Same-process outage test: three outages, 14 arrivals, nine retained, five
  dropped in one gap; exact cumulative counts and bounded loss times checked.
- Maximum configuration: 16 pods, 193 distinct alarm entries, 64 readings and
  32 heartbeats; all 32 high alarms raised and cleared, zero lost records.
- Natural burst: 50,001 sent = 44,745 received + 5,256 kernel drops. Of received
  frames, 44,615 readings were accepted, 129 rejected and one was the probe.
  Offered rate was 921,034 frames/s for 0.054287 s, with persistence disabled.
  This is a burst/application measurement, not sustained or physical CAN capacity.
- Controlled crash: two SQL readings survived and three known queued readings
  disappeared; actual systemd SIGKILL restart observed after 1247 ms, plus a
  separate watchdog restart; runtime UID 999.

Raw results: `artifacts/checkpoint-5-core/`. Tested implementation digest:
`98587efdcdc0b39d4703370d580932efdf098bce4d7da6fb79ef606c1292a8b2`.
The later final release-gate run is identified in the generated
[VERIFICATION.md](VERIFICATION.md); observations above retain their own run identity.

## Final clean release gate — passed

```sh
./scripts/dev ./scripts/verify --clean --output artifacts/checkpoint-5-final
```

All ten commands exited 0, from **2026-09-28 08:22:47.905 to 08:29:24.811 UTC**.
Six clean C builds passed 45 cases each (270 executions); seven Python cases
passed twice. The five PostgreSQL contracts, all real vcan/monitor/persistence/
systemd integrations, ten fault scenarios, five hardening scenarios in three
variants, demo and can-utils exchange passed. No required test was skipped.
Strict GCC/Clang, GCC analyzer, ASan/UBSan and TSan reported no finding;
Helgrind reported zero queue-suite errors with its normal library suppressions.

| Final-run measurement | Observed result |
| --- | --- |
| Decoder libFuzzer | 84,999,114 executions in 121 seconds; peak RSS 301 MiB; no crash, sanitizer report or invariant failure |
| Silent-source alarm | 500 ms deadline; [504, 504, 504] ms observed ages; three recoveries |
| Rapid counter restarts | 500–523 ms alarm ages across Release/ASan/TSan; six cycles, 18 rejects, 12 stream resets and 18 recoveries per variant |
| Repeated outages | Three outages in one run; 14 arrivals = nine retained + five dropped; one gap, exact cumulative accounting |
| Maximum snapshot | 16 pods; 193 unique alarm entries; 64 readings and 32 heartbeats; 32 high alarms raised/cleared; no loss |
| Controlled crash | Two committed readings survived, three known queued readings lost; SIGKILL restart observed after 1227 ms; separate watchdog restart; UID 999 |
| Natural application burst | 50,001 sent = 50,001 received + zero socket drops; 50,000 accepted readings and one probe; offered 697,550 frames/s over 0.071679 s |
| Forced receive overflow | 50,001 sent = 223 received + 49,778 socket drops |
| Interview demo | 502 ms stale detection; 4036 ms observed outage; eight prior + four backfilled = twelve SQL readings; seven drops in one gap; no event loss/abandonment |

Both burst cases disabled persistence; neither is a sustained-throughput,
physical CAN or database-capacity claim. Full SQL exports contain 32 history
rows and twelve checkpoints for the final demo. Run identity, raw measurements
and exact commands are in [VERIFICATION.md](VERIFICATION.md) and
`artifacts/checkpoint-5-final/`; console transcript:
`artifacts/checkpoint-5-final.log`.

Final tested implementation SHA-256, using the expanded digest coverage:
`b87b8f78da76c8bcfbf7c07d7f2ec26f795ca27d56bb18eedebf864479cf419b`.
Documentation/state edits do not enter this digest. The local release commit
and annotated `vigil-v1` tag identify the finished tree without embedding a
circular commit hash inside its own report.

Final cleanup found no test daemon, simulator or temporary PostgreSQL server;
`vigil-evidence.service` was not found. The installed default service was
inactive/disabled, the existing 16/main cluster online, and global ASLR still 2.
The incomplete Buildroot tree and separate baseline build were removed;
the VM remains running. Cleanup evidence: `artifacts/checkpoint-5-cleanup.json`.
Project link/syntax checks and the release-file scan found no broken relative
links, candidate secret material, unfinished TODO markers or copied placeholder
metrics. Generated builds and raw evidence are ignored; the only intended
tracked binary is the authoritative brief PDF.

## Optional Buildroot disposition — deferred

The extension was attempted only after the expanded core gate and the four
evidence/owner guides were complete. The isolated Linux build used Buildroot
2026.08, an ARM64 external toolchain, systemd, PostgreSQL, Vigil and can-utils.
Its planned replay used a guest-local `vcan0`; no outer-VM bus access was assumed.
Target Python was omitted. PostgreSQL's server is available in the selected
release, so there was no reason to silently replace it with an external database.
Sources: [Buildroot 2026.08 release](https://buildroot.org/download.html),
[PostgreSQL package](https://github.com/buildroot/buildroot/blob/2026.08/package/postgresql/postgresql.mk).

The two-CPU Linux build reached its explicit 1800-second limit and exited 124
while installing dependencies, last at expat 2.8.4. The images directory was
empty: no successful image, QEMU boot or in-guest daemon run occurred. The
decision was to stop the optional packaging attempt and finish the verified
core, following brief section 5. This is a scope/time decision, not evidence
that Buildroot or PostgreSQL cannot work on this machine.

The entire isolated `/tmp/vigil-buildroot-audit` source/package/build tree was
removed. No partial embedded package or image is retained in the release.
Raw log: `artifacts/checkpoint-5-buildroot-build.log`; exact command, exit result,
configuration and disposition: `artifacts/checkpoint-5-buildroot-attempt/`.
The downloaded Buildroot archive SHA-256 was
`87aaca4164ea9d5c8085854953018263f7963f07c22e73a2a2185cc98c581c34`.
QEMU/build prerequisites remain installed in the development VM; that alone is
not an embedded-image result. **No Buildroot resume bullet is supported.**

## Documented operational commands

The README's `scripts/install-service`, `scripts/setup-database`,
`scripts/vcan-up`, and `sudo systemctl enable --now vigild` commands were
executed against the existing Ubuntu default cluster. The current binary was
installed; existing configuration was preserved. Three real Python simulator
cycles produced exactly six SQL readings, three heartbeats and zero rejected
frames in run `eec9b3b3-67d0-4ba7-9ac9-61fe5cb32d7c`. The actual service was
active as UID 999, drained with zero abandonment, and exited 0. It was stopped
and disabled afterward so it cannot interfere with the final test bus.
Evidence: `artifacts/checkpoint-5-readme-service.log`; the asserted smoke script
is retained beside it. This verifies the documented path in the existing VM,
not a fresh macOS/Lima installation on a second machine.

The exact host command `./scripts/dev ./scripts/demo` also passed, with evidence
in `artifacts/demo-20260927T211917Z/` and console log
`artifacts/checkpoint-5-interview-command.log`. It observed 502 ms stale
detection at the configured 500 ms deadline, twelve SQL readings, exactly
seven drops in one gap, and zero event loss or shutdown abandonment. This
standalone command ran while the optional build was compiling; its observed
outage duration was 7205 ms and is not substituted for the clean release-gate
measurement.

## Limitations

Queue/alarm memory is volatile. Bounds are N pending readings, 256 events and
one immutable in-flight batch of at most 32 records; no disk spool. Arbitrary
crash loss cannot be reconstructed; an unconfirmed COMMIT can already be in SQL.
Shutdown has a one-second best-effort drain, not an unlimited delivery promise.
History retention is not automated and database storage can fill.

CRC/counters cannot identify senders or resolve arbitrary wrap/replay/restart
ambiguity. Flatness is suspicion. Rate checks use receive time and can reject
batched samples after scheduling stalls. vcan does not model physical CAN.
Only Ubuntu ARM64 was exercised; board images, drivers, wiring and real-bus
validation are new work. No hard real-time guarantee. Diagnostics can drop
counted lines; filesystem output/authentication files can block. Network
timeouts do not bound every filesystem or scheduler delay.

The GitHub Actions command suite passed locally; no hosted run or remote
publication occurred. The explicit brief-deferred API/dashboard, Modbus,
signed backups, authenticated CAN, hardware and spool features remain deferred.
