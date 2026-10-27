# Vigil 2.0 performance comparison

Acceptance results for the release are summarized in
[RESILIENCE_AUDIT.md](RESILIENCE_AUDIT.md). This report describes finite Linux
application workloads, not hard real-time behavior, physical CAN, power-loss
qualification or universal throughput limits. Final measured tables below are
populated only from the completed candidate gate.

## F5 requalification measurements (2026-10-27)

The F5 remediation changed only the SQLite installer and Python tests; the
benchmarked production binaries are byte-identical to every earlier repaired run
(Resilience `vigild` SHA-256
`5c423d2fd742ec59c6a557ce8f5a4be4b9cbc02b751f889eb528550a9de68be7`, original
`8e60df7dfc0545c16e885065b2f4f35932baf9303aa6a8ace01a5ae1223a8e6c`). Its clean gate
(`artifacts/remediation-f5-20261027/linux-full/benchmark/`, source digest
`e755cbdd42ad02b96dff060bb66b0ccf59eb5c0491f6cbad91137238ee966250`) passed all six
paced trials and six finite bursts; every paced trial reconciled 6,800 readings
with no known reading/event loss and every repaired store drained to zero.

| F5 gate: median (range), three trials | Original Vigil | Repaired Resilience |
| --- | --- | --- |
| Paced monitoring readings/s | 100.00 (100.00–100.00) | 100.00 (100.00–100.00) |
| Remote window readings/s | 100.00 (99.99–100.01) | 74.21 (73.47–75.84) |
| Remote completion incl. catch-up readings/s | 100.00 (99.96–100.00) | 75.74 (75.57–76.65) |
| 600-reading drain, s | 0.90 (0.85–0.95) | 8.87 (8.66–9.11) |
| Daemon CPU, % of one core | 2.55 (2.42–2.87) | 19.31 (17.61–20.03) |
| Peak sampled RSS, MiB | 7.77 (7.77–7.78) | 11.68 (11.65–11.70) |
| Alarm lateness beyond 500 ms, ms | 8.00 (8.00–9.00) | 6.00 (5.00–8.00) |
| Core frame p50, ns | 83.00 (83.00–125.00) | 83.00 (83.00–166.00) |
| Core frame p95, ns | 84.00 (84.00–208.00) | 166.00 (84.00–250.00) |
| Core frame p99, ns | 84.00 (84.00–209.00) | 167.00 (84.00–250.00) |
| Local admission readings/s | N/A | 99.99 (99.98–100.00) |

The same Resilience executable has now measured remote window medians of 74.37,
83.86, 81.20 (independent re-audit) and 74.21 readings/s, and daemon CPU from 11.52%
to 19.31% of one core, on one shared VM. The final independent audit's fresh
rerun measured a 73.71 readings/s median (73.59–77.20). Because the binary did not
change, no F5 runtime-code change explains these spreads; these non-isolated runs
cannot establish their cause. They also show that one three-trial set cannot
characterize this workload's capacity. The slower
and costlier result is reported, not replaced by an earlier faster one. Remote
replay still does not sustain the offered 100 readings/s for the natural batch
shape; no sustainable-delivery guarantee is inferred.

## Audit remediation measurements

The original Stage 4 tables below are historical measurements of the candidate
rejected by independent audit. Keep their remote throughput deficit visible.
The first repaired gate (`artifacts/remediation-final-gate/benchmark`) completed
three matched trials per binary on 2026-10-25. Median remote status-window rate
was **74.37 readings/s** versus **99.995/s** for original Vigil and the previous
candidate's 82.50/s. Local admission was 99.997/s, with no known losses and exact
6,800-reading final reconciliation in each trial. The 600-reading drain measured
10.12 s versus original 0.86 s; daemon CPU 11.52% versus 2.25% of one core; sampled
peak RSS 11.67 versus 7.77 MiB. Monitoring accepted the offered 100/s; same-run
core p50/p95/p99 medians matched at 83/84/84 ns, and alarm lateness medians were
7 ms versus 5 ms. The remote decrease of about 9.9% relative to the earlier
candidate is an observed comparison, not an attributed cause. These finite,
non-isolated VM runs do not establish a throughput guarantee or causal regression.

The final archived rerun passed all six paced trials and six finite bursts on
Linux 6.8.0-147; raw data is in `artifacts/remediation-final-gate-3/benchmark/`.
Each paced trial reconciled all 6,800 readings without known reading/event loss;
all repaired stores ended with zero pending records, batches and bytes. The
source digest is `353462a3e1bd7db82284adfbba09e81ad903c5a5540e7f964f42fc6843c96e06`.

| Final archived rerun: median (range), three trials | Original Vigil | Repaired Resilience |
| --- | --- | --- |
| Paced monitoring readings/s | 100.00 (100.00–100.00) | 100.00 (100.00–100.00) |
| Remote window readings/s | 100.01 (100.00–100.01) | 83.86 (82.18–84.30) |
| Remote completion incl. catch-up readings/s | 100.00 (99.96–100.00) | 81.97 (81.11–83.13) |
| 600-reading drain, s | 0.94 (0.83–0.94) | 8.87 (8.85–9.17) |
| Daemon CPU, % of one core | 1.97 (1.93–2.13) | 12.86 (12.25–13.26) |
| Peak sampled RSS, MiB | 7.76 (7.75–7.78) | 11.16 (11.12–11.27) |
| Alarm lateness beyond 500 ms, ms | 7.00 (6.00–10.00) | 8.00 (7.00–8.00) |
| Core frame p50, ns | 85.00 (85.00–167.00) | 167.00 (84.00–169.00) |
| Core frame p95, ns | 86.00 (85.00–209.00) | 209.00 (84.00–211.00) |
| Core frame p99, ns | 86.00 (86.00–210.00) | 209.00 (84.00–212.00) |
| Local admission readings/s | N/A | 99.98 (99.95–99.98) |

Both completed repaired runs are retained. The kernel changed after VM reboot;
shared-host scheduling/storage also vary. The results do not isolate a causal
performance effect of this small recovery repair. Normal monitoring met the
same offered load and alarm checks, while remote replay still failed to sustain
100 readings/s for the natural batch shape. Neither the faster archived rerun
nor its API measurements erase the slower first result. No sustainable delivery,
physical CAN or asynchronous per-record latency guarantee is inferred.

The final direct-core probe's median p50 is slower (167 ns versus 85 ns), and
its bulk median is also lower; this is retained in the raw results, not treated
as an improvement. Trial 2 reverses the ordering (84 ns Resilience, 167 ns
original), and clock-pair p50 spans 0–42 ns with p95 up to 85 ns. The monitor,
config and protocol sources are byte-identical to original d179681. These
non-isolated, quantized measurements cannot attribute a core-code regression;
no tight core-latency guarantee is claimed. The production workload still meets
the offered 100/s monitoring load, exact counts and alarm timing checks.

## Matched method and measurement boundaries

`tests/resilience_benchmark.py` builds original commit
`d179681a40497ada533d2f61e50b5ac60a6ca293` in a disposable detached worktree and
compares it with the candidate production Release binary. Existing branches are
not modified. Baseline and candidate use the same Ubuntu ARM64 VM, native ext4,
GCC Release settings, disposable PostgreSQL 16 settings with fsync,
full_page_writes and synchronous_commit enabled, 1024-reading queue, 256-event
lane, 500-ms source timeout, 20-ms tick, 100-ms diagnostic cadence, rate checking
disabled and identical Python-generated SocketCAN inputs. Original uses its
unchanged migration 001 writer; Resilience adds 002 and its required exact-content
verification and SQLite work. That changed persistence contract is part of the
comparison, not an equal-cost algorithm contest.

Three trials per binary alternate original/Resilience, Resilience/original,
original/Resilience. Each allocates fresh resources, excludes a two-second
200-reading warmup plus catch-up, then offers 6000 readings over 60 seconds at
100/s and 600 heartbeats at 10/s. After exact healthy reconciliation and a stale
alarm, each stops its own PG, offers 600 more readings over six seconds, restores
PG and waits for zero pending, exact SQL counts and no known loss. A finite
50001-frame `--no-persistence` vcan burst is also repeated per binary, with sent
= received + kernel drops. It is offered burst/delivery evidence, not sustained
monitor capacity. Tests do not compete for vcan. No CPU affinity, host-isolation
or kernel-cache flush is applied; this VM shares the host scheduler/storage.

| Measurement | Actual boundary and limitation |
| --- | --- |
| Sensor core latency | Identical C probe compiled against each tree's actual monitor/config/protocol; 10000 warmup calls, 100000 individually timed accepted constant readings, then 1000000 calls timed in bulk; no callback/CAN/queue/DB/worker scheduling; clock-pair overhead separately reported, not subtracted |
| Alarm deadline | Last valid receive monotonic ms to communication alarm's recorded monotonic ms; lateness = age −500 ms; 20-ms tick plus scheduler delay, never an early alarm |
| Paced monitoring rate | 6000 accepted readings / actual offered interval; this demonstrates the selected load, not a capacity limit |
| Local/remote window rate | Delta of status counters divided by status monotonic timestamps near measured start/end; status publication has 100-ms cadence; exact totals also checked at catch-up |
| Reconciled remote completion rate | 6000 / (offered interval + final catch-up observation time); includes remote drain after the last offered input |
| Replay/drain | 600 / (time caught-up observed − PG available); includes retry backoff, backlog's frozen batch shape and status observation; PG startup separately recorded; no standalone network bandwidth claim |
| CPU/RSS | Daemon utime+stime delta / wall interval as percent of one core; /proc RSS/HWM samples; excludes generator, PG and filesystem cache, and is not total-system memory |
| DB/WAL | Native live file-length samples and final stopped inventory; not filesystem allocated-block usage or physical write amplification |

`benchmark-outbox` is a separate production-library, single-owner API experiment:
three fresh stores for each batch size (1 and 32 readings), sixteen excluded
warmup batches, then 200 measured batches per store. Inputs have minimal synthetic
JSON detail and one snapshot entry. No network or remote retirement occurs.
The real queue is filled and taken sequentially by the probe, so enqueue-to-commit
omits daemon idle/wakeup/other-worker contention and is not an asynchronous daemon
latency percentile. Actual `BEGIN IMMEDIATE` entry through successful SQLite
COMMIT return is observed by link wrapping `sqlite3_exec`; it excludes earlier
payload validation and the later directory fsync. Wider append timing includes
those operations and any triggered checkpoint. Wrapping adds clock-read overhead;
production code/settings are unchanged. A separately timed TRUNCATE checkpoint
includes directory sync. API throughput divides records by summed append times,
excluding setup, producer work and output; it is not daemon/PG capacity.

Distributions use nearest-rank p50/p95/p99, all samples and no outlier removal.
For daemon totals only three trials exist: ranges and median describe those
three trials, not reliable tail percentiles. API percentiles pool 600 measured
batches per size; individual trial files retain between-run variability.

## Historical Stage 4 candidate results

Final clean gate: **68 zero command exits**, 2026-10-24T17:28:50.960947+00:00 through 2026-10-24T17:55:45.543621+00:00.
Candidate implementation SHA-256: `88f09dabc79ede8a750f1e68659ed478b0fb2e8787c6459bcf107f8504e58cd0`. Original implementation SHA-256: `b87b8f78da76c8bcfbf7c07d7f2ec26f795ca27d56bb18eedebf864479cf419b`.
Raw evidence: `artifacts/resilience-stage4/final3/benchmark/`: `results.json`, `summary.json`, each `trial.json`, `*-core.json` and `local-*.json`. Source and production binary identities, compiler commands and selected matching CMake flags are retained.

Executed on Ubuntu 24.04 ARM64, Linux 6.8.0-142-generic, 2 vCPUs/2 GiB VM, native ext4, GCC 13.3.0, Clang 18.1.3, CMake 3.28.3, PostgreSQL/libpq 16.15, SQLite 3.51.3 and systemd 255. GCC Release flags matched: `/usr/bin/gcc`, no extra C flags, `-O3 -DNDEBUG`. The original digest matches the recorded Stage 0 baseline. Both versions’ paced-persistence trials processed **6800 readings per trial** (200 warmup + 6000 measured + 600 outage), with zero known reading/event losses and exact final SQL counts. Resilience also finished with zero local pending records/batches/bytes.

Values are **median (minimum–maximum)** across three trials. Core-call percentile rows show the median/range of each trial's own percentile, not pooled raw samples. Local admission is absent from the original; its RAM queue is not a durability boundary.

| Measurement | Original Vigil | Vigil Resilience |
| --- | --- | --- |
| Direct core frame call p50, ns | 83 (83–126) | 125 (83–125) |
| Direct core frame call p95, ns | 84 (84–168) | 167 (84–208) |
| Direct core frame call p99, ns | 84 (84–168) | 208 (84–209) |
| Direct core bulk throughput, million calls/s | 27.45 (21.24–28.12) | 23.80 (21.59–27.69) |
| Paced accepted readings/s | 100.00 (100.00–100.00) | 100.00 (100.00–100.00) |
| Locally committed readings/s, status window | N/A | 99.99 (99.98–99.99) |
| Locally committed input records/s, status window | N/A | 110.03 (110.01–110.04) |
| Remote committed readings/s, status window | 100.00 (99.99–100.00) | 82.50 (82.05–82.72) |
| Remote committed input records/s, status window | 110.00 (110.00–110.01) | 90.80 (90.29–91.04) |
| Remote completion readings/s including final catch-up | 99.95 (99.90–100.00) | 77.15 (77.06–77.98) |
| Alarm lateness above 500-ms deadline, ms | 11 (4–16) | 6 (5–7) |
| 600-reading drain from PG available, s | 0.84 (0.82–0.92) | 11.13 (11.02–11.53) |
| Recovery from PG start request, s | 0.99 (0.97–1.03) | 11.27 (11.15–11.67) |
| Replay rate including backoff/status wait, readings/s | 716.89 (653.12–732.80) | 53.91 (52.03–54.46) |
| Daemon CPU, % of one core | 1.42 (1.32–1.43) | 10.53 (10.33–10.61) |
| Peak sampled daemon RSS, MiB | 7.77 (7.76–7.77) | 11.28 (11.27–11.36) |
| Finite burst offered frames/s | 767216 (716541–771851) | 760950 (643099–784324) |
| Finite burst kernel receive drops | 26 (0–756) | 975 (91–1875) |

**Resilience local admission kept pace with this finite offered load; remote delivery did not.** It accumulated a durable backlog during the 60-second window and needed additional time after input stopped. These results do not support indefinite 100-reading/s delivery for this batch shape. The unchanged core algorithm provides no basis for claiming a core-speed improvement from small timing differences.

| Trial | Version | Local readings/s | Remote window readings/s | Drain after PG available, s | CPU % | Peak RSS MiB |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | original | N/A | 100.00 | 0.819 | 1.43 | 7.77 |
| 1 | resilience | 99.99 | 82.72 | 11.532 | 10.53 | 11.28 |
| 2 | resilience | 99.99 | 82.05 | 11.130 | 10.33 | 11.36 |
| 2 | original | N/A | 99.99 | 0.919 | 1.42 | 7.77 |
| 3 | original | N/A | 100.00 | 0.837 | 1.32 | 7.76 |
| 3 | resilience | 99.98 | 82.50 | 11.017 | 10.61 | 11.27 |

SQLite API distributions below pool **600 measured batches per size**, excluding 16 warmup batches in each of three fresh stores. Times are milliseconds. These are single-owner synthetic API results, not asynchronous daemon latency or PG throughput.

| Batch size | Boundary | p50 ms | p95 ms | p99 ms | Maximum ms |
| --- | --- | --- | --- | --- | --- |
| 1 | First enqueue → durable append return | 0.878 | 1.146 | 1.596 | 6.629 |
| 1 | Append API | 0.878 | 1.146 | 1.596 | 6.629 |
| 1 | BEGIN entry → SQLite COMMIT return | 0.608 | 0.861 | 1.324 | 3.935 |
| 32 | First enqueue → durable append return | 1.280 | 1.605 | 2.246 | 6.661 |
| 32 | Append API | 1.275 | 1.602 | 2.242 | 6.658 |
| 32 | BEGIN entry → SQLite COMMIT return | 0.634 | 0.952 | 1.625 | 3.186 |

| Batch size | Records / summed append seconds, median (range) | Explicit TRUNCATE checkpoint ms, median (range) | Max sampled DB/WAL bytes |
| --- | --- | --- | --- |
| 1 | 1085 (1050–1101) | 1.487 (1.188–1.495) | 348160 / 4206552 |
| 32 | 24013 (23200–24217) | 1.824 (1.540–1.853) | 827392 / 4206552 |

During healthy production trials, max sampled Resilience DB/WAL/SHM lengths were **1695744/3596792/32768 bytes**. These are file-length samples, not hard-bound stress fills. API stores retained 216 or 6912 records including warmup; all six explicit final TRUNCATE checkpoints left WAL length zero. Automatic trigger checkpoints may be included in append timings.

Clock-pair calibration p50 per core trial (ns): 1-original=41, 1-resilience=83, 2-resilience=41, 2-original=42, 3-original=41, 3-resilience=42. Clock overhead is retained in latency samples. These core latencies are close to clock-pair cost and timer quantization, so their differences do not establish an algorithmic regression or improvement.

Largest absolute UTC-versus-monotonic scenario-duration difference in the accepted benchmark: **0.532 s**. All performance calculations use Linux monotonic time. A prior full-gate attempt failed its administrative PG export at the unchanged 15-second timeout amid much larger clock discontinuities; an unchanged-budget focused rerun and this full gate passed. Earlier pilots/interrupted/failed attempts remain under `artifacts/resilience-stage4/` and are not silently merged into these tables. No outlier within the accepted runs was removed.

Original production binary SHA-256: `8e60df7dfc0545c16e885065b2f4f35932baf9303aa6a8ace01a5ae1223a8e6c`. Resilience production binary SHA-256: `a634746ed921f6f82521297bb2be1087f0fd397645520b7c85eff698068a0bb9`.


## Interpretation and current limits

Durability, JSON/origin/content validation, exact retained receipts and local
retirement add work versus original Vigil. A slower replay result is a measured
tradeoff; it must not be hidden by quoting only a favorable burst or minimal
payload probe. A 600-reading outage shape fits the common RAM lane so both
versions can be compared without an imposed original crash loss. The separate
fault/demo gates cover Resilience crash survival and bounded failures.

The design contract's broader T19 instrumentation target remains only partially
measured: no per-record asynchronous daemon enqueue-to-commit/retire tracing,
queue mutex maximum-hold distribution, SQLite allocator instrumentation,
physical block-write accounting or saturated maximum-throughput search is
claimed. Storage and allocator bounds remain implementation/fault-test
properties, with sampled RSS/files as finite evidence. A future independent
performance investigation can add such instrumentation with its overhead stated.
No guarantee follows for other hosts, filesystems, traffic shapes, maximum
simultaneous limits or remote deployments.
