# Resume evidence

These are project facts, followed by optional wording. The owner must be able
to explain the code and accurately describe their contribution. Implementation
and verification were performed by one Codex agent at the owner's direction;
do not describe that as independent hand-written work or personal test execution.
The brief's section-7 process deviation is recorded in [DECISIONS.md](DECISIONS.md).

## Verified capabilities and supporting evidence

| Fact safe to describe | Implementation | Evidence |
| --- | --- | --- |
| C daemon on Linux monitors simulated temperature/door pods over real SocketCAN | `src/vigild.c`, `src/protocol.c`, `simulator/pod.py` | Decoder/monitor vcan gates in [verification](VERIFICATION.md) |
| Freshness, publisher progress and plausibility are separate; stale values lose current status | `src/monitor.c`, `include/vigil/monitor.h` | 26 monitor/config cmocka cases, silent/non-advancing/threshold scenarios |
| Main loop owns alarms; PostgreSQL writes use a separate thread and bounded queue | `src/queue.c`, `src/persistence.c`, `migrations/001_history.sql` | Live DB outage, lock/statement timeout, lost-response and exact SQL recovery checks |
| Capacity-four settled outage with eleven arrivals retains four and drops exactly seven | Queue oldest-pending eviction and batch gap accounting | Fault overflow scenario and interview demo; SQL exports under the final evidence directory |
| Memory loss on daemon crash is explicitly tested | UUID-per-process startup; no disk spool | systemd SIGKILL fixture: two committed readings survive, three known pending readings disappear |
| Unprivileged notify/watchdog/restart service | `systemd/vigild.service`, `src/vigild.c` | Actual systemd UID, readiness, healthy watchdog, SIGKILL and SIGSTOP/watchdog tests |
| Ten reproducible fault scenarios plus decoder fuzzing | `tests/fault_scenarios.py`, `fuzz/decoder.c`, `fuzz/seed.py` | Complete scenario table; actual finite libFuzzer/ASan/UBSan log |
| Strict builds, C/Python tests, concurrency checks and optimized integration | `CMakeLists.txt`, `scripts/check*`, `tests/final_hardening.py` | [Final audit](FINAL_VERIFICATION.md) and generated gate results |
| GitHub Actions workflow authored and its command suite run locally | `.github/workflows/ci.yml`, `scripts/check-ci` | Local CI-equivalent log; **no hosted CI success claimed** |

The core contains 45 cmocka cases (13 decoder/protocol, 26 monitoring/config,
six queue/health), seven Python unit cases, ten brief fault scenarios, and five
additional lifecycle/boundary scenarios. Build variants repeat cases; do not
count repeated executions as new unique tests. Integration scripts also contain
several independently asserted scenarios, so this is not a claim that the entire
project has only those test cases.

## Wording suggestions

Only use these after you can defend them and describe the agent-assisted process
truthfully. They intentionally avoid unmeasured performance numbers.

- Built an agent-assisted C/Linux sensor-monitoring project using SocketCAN,
  separating data freshness, publisher progress and measurement plausibility,
  with in-memory alarms and threshold hysteresis.
- Isolated PostgreSQL persistence behind a bounded queue and dedicated writer;
  verified monitoring during database outages and exact recovery accounting,
  including four retained and seven dropped readings in an overflow fixture.
- Developed and audited reproducible failure evidence covering ten fault
  scenarios, cmocka tests, decoder libFuzzer with ASan/UBSan, and Linux
  systemd watchdog/restart behavior.

Adjust verbs to your actual contribution; the third suggestion does not imply
you personally authored every test. A compact project heading is:
**Vigil — C, Linux/SocketCAN, PostgreSQL, Python, systemd**.

## Numbers and boundaries

Use the final observed counts, dates and commands in
[FINAL_VERIFICATION.md](FINAL_VERIFICATION.md), with the detailed run in
[VERIFICATION.md](VERIFICATION.md). Quote latency with its configured deadline
and tested VM. Quote fuzz counts with duration and “no finding in that run.”
The load experiment is a short offered burst with persistence disabled, not
sustained throughput or a physical CAN stress result. It is usually clearer
to describe the exact drop accounting than to put its offered rate on a resume.

The optional Buildroot attempt was deliberately deferred after its bounded
build expired without an image; incomplete work was removed. No Buildroot/ARM64
image bullet is supported. ARM64 Ubuntu execution alone does not establish
that claim. Also exclude A-Link compatibility, cryptographic
sender identity, physical CAN validation, crash-durable buffering, broad hardware
portability, production deployment and hosted-CI-green claims.
