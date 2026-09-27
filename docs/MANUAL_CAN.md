# Manual CAN checklist

Run inside Linux (`./scripts/dev` from macOS), on otherwise idle `vcan0`,
after building. These commands supplement the asserted scenario suite;
a human watching output is not the automated pass/fail gate.

Open three terminals. Start observers before sending. Do not use `|| true`
to hide unexpected exits. The decoder deliberately exits **1** if it sees
malformed input; setup/timeout errors exit **2**.

| Command | Expected result | Inspect | Actual-result record |
| --- | --- | --- | --- |
| `./scripts/vcan-up` | Real Linux vcan0 is up | `ip -details link show vcan0` says vcan | Record UTC date, kernel, interface |
| A: `candump -L -n 3 vcan0,101:7FF` | Observe the three frames below, with timestamps | Exact ID, length and payload | Paste three timestamped lines |
| B: `build/vigil-decode --listen vcan0 3` | `ready` before injection; results ok, length, crc | JSON and exit status 1 | Paste JSON; record `echo $?` immediately after exit |
| C: `cansend vcan0 101#0098080000B7` | Pod 1 temperature 22.00 C, counter 0 | Decoder value=2200, counter=0 | Actual value / timestamp: ___ |
| C: `cansend vcan0 101#00` | Reject wrong length | result=length | Actual reason / timestamp: ___ |
| C: `cansend vcan0 101#0098080000B6` | Reject bad CRC | result=crc | Actual reason / timestamp: ___ |
| A: `build/vigild --no-persistence` | Ready; periodic monitoring status | PID, monotonic times | PID / start: ___ |
| B: `.venv/bin/python -m simulator.pod --interval 0.1` | Two fresh/current channels and a fresh heartbeat | accepted counts grow | Counts / timestamp: ___ |
| B: Ctrl-C simulator | Three communication alarms after each 3000 ms default freshness deadline | retained values have current=0; alarm time minus last-valid time (100 ms tick plus scheduling delay) | Three measured ages: ___ |
| B: restart same simulator | Each channel and heartbeat recovers | fresh/current, communication cleared, reset counters | Recovery counts / times: ___ |
| A: Ctrl-C daemon | Clean exit 0 | stopped line and exit code | Actual: ___ |

Keep each run's filled copy and terminal transcript under `artifacts/`, recording
PASS/FAIL beside each row. The first six rows are also reproduced verbatim by
`tests/manual_commands.py`; the final verification report records its actual
candump timestamps, decoder output and exit status. That is agent-executed
command evidence, not a claim that a human has performed the checklist.

For alarms unaffected by malformed input, duplicate rejection, exact jump loss,
hysteresis, SQL gaps and crash loss, use `tests/fault_scenarios.py`. Interactive
copy/paste timing can expire counter baselines; the automated suite controls
that timing and checks exact states. Database failure should use `scripts/demo`
and its disposable cluster, not stop a shared PostgreSQL service.
