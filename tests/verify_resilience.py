#!/usr/bin/env python3
"""Clean release-candidate gate: regressions, faults, demo and matched benchmarks
on isolated real Linux/SQLite/PostgreSQL. No independent-verification claim.
"""
from verification import require  # also refuses optimized execution before fixture imports
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys
import time
from evidence import source_digest

ROOT = Path(__file__).resolve().parents[1]


def run_command(out, report, env, expected, cmd):
    commands = report["commands"]
    print("EXPECT:", expected, flush=True)
    print("COMMAND:", shlex.join(cmd), flush=True)
    start = time.monotonic()
    entry = dict(expected=expected, command=cmd, started_utc=datetime.now(timezone.utc).isoformat(),
                 log=f"{len(commands):02d}.log")
    with (out / entry["log"]).open("w") as log:
        result = subprocess.run(cmd, env=env, stdout=log, stderr=subprocess.STDOUT)
    entry.update(exit_code=result.returncode, duration_s=time.monotonic() - start)
    commands.append(entry)
    (out / "gate.json").write_text(json.dumps(report, indent=2))
    require((result.returncode == 0), lambda: (f"failed {shlex.join(cmd)}: {out / entry['log']}"))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--clean", action="store_true")
    ap.add_argument("--output", type=Path, default=Path("artifacts") / ("resilience-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")))
    args = ap.parse_args()
    require((sys.platform == "linux" and not sys.flags.optimize))
    os.chdir(ROOT)
    # Inherited service/options/credentials or fault controls cannot select test
    # endpoints. Each PG fixture allocates its own cluster and explicit route.
    for key in list(os.environ):
        if key.startswith(("PG", "VIGIL_DB_", "VIGIL_TEST_")):
            os.environ.pop(key)
    args.output.mkdir(parents=True, exist_ok=False)
    out = args.output.resolve()
    commands = []
    report = dict(started_utc=datetime.now(timezone.utc).isoformat(), source_sha256=source_digest(),
                  head=subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
                  kernel=platform.platform(), python_optimize=sys.flags.optimize, commands=commands)
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
               TSAN_OPTIONS="halt_on_error=1")
    def run(expected, cmd):
        run_command(out, report, env, expected, cmd)
    run("real Linux vcan0, otherwise idle", ["./scripts/vcan-up"])
    run("existing Python cases and unconditional verification negative controls", [".venv/bin/python", "-m", "unittest", "discover", "-s", "tests", "-p", "test_*.py", "-v"])
    run("five original PostgreSQL v1 schema/transaction/permission contracts on disposable server", [".venv/bin/python", "tests/database_contract.py"])
    run("ten PostgreSQL v2 origin/content/concurrency/atomicity contracts on 001+002", [".venv/bin/python", "tests/resilience_database_contract.py", "--output", str(out / "sql-v2")])
    builds = [("debug", "Debug", "gcc", []), ("release", "Release", "gcc", []),
              ("asan", "Debug", "clang", ["-DVIGIL_SANITIZERS=ON"]),
              ("tsan", "Debug", "gcc", ["-DVIGIL_TSAN=ON"])]
    for name, kind, cc, flags in builds:
        build = "build-resilience-" + name
        flags = [*flags, "-DVIGIL_BENCHMARKS=ON"]
        if args.clean and (ROOT / build).exists():
            shutil.rmtree(ROOT / build)
        run(f"strict {name} configure", ["cmake", "-S", ".", "-B", build, "-DCMAKE_BUILD_TYPE=" + kind, "-DCMAKE_C_COMPILER=" + cc, *flags])
        run(f"strict {name} build, no warnings", ["cmake", "--build", build, "--parallel", "2"])
        prefix = ["setarch", platform.machine(), "-R"] if name == "tsan" else []
        run(f"67 C cases: 13 protocol, 26 monitor, 6 queue, 22 outbox; zero sanitizer findings ({name})",
            [*prefix, "ctest", "--test-dir", build, "-V"])
        if name != "tsan":
            run("unchanged real SocketCAN decoder regression (" + name + ")", [".venv/bin/python", "tests/integration.py", "--decoder", build + "/vigil-decode"])
        run("unchanged live monitoring regression (" + name + ")", [*prefix, ".venv/bin/python", "tests/monitor_integration.py", "--daemon", build + "/vigild"])
        run("exact crash cuts, stable DB/WAL payloads, capture races, faults, priority and startup (" + name + ")",
            [*prefix, ".venv/bin/python", "tests/resilience_integration.py", "--build", build, "--output", str(out / name),
             "--restarts", "20" if name == "debug" else "2"])
        run("A-J exact remote crash/commit cuts, FIFO/origin/poison/outage/deadline assertions (" + name + ")",
            [*prefix, ".venv/bin/python", "tests/resilience_replay.py", "--build", build, "--output", str(out / (name + "-replay")),
             "--restarts", "20" if name == "debug" else "2", *(["--timing"] if name == "debug" else [])])
        run("F1/F2 overlapping confirmed replay/uncertain append, allocation reloads, quiet progress, exact accounting and reconciliation SIGKILL cuts (" + name + ")",
            [*prefix, ".venv/bin/python", "tests/resilience_remediation.py", "--build", build,
             "--output", str(out / (name + "-remediation"))])
        run("Stage 3 deterministic mixed crashes, exact durable union and replay resource assertions (" + name + ")",
            [*prefix, ".venv/bin/python", "tests/resilience_adversarial.py", "--build", build,
             "--output", str(out / (name + "-adversarial")),
             *([] if name == "debug" else ["--seed", "0", "--seed", "7", "--seed", "47", "--seed", "48"])])
    run("real ext4 ENOSPC/read-only and unprivileged hardened systemd SIGKILL/watchdog recovery",
        [".venv/bin/python", "tests/resilience_storage.py", "--build", "build-resilience-debug", "--output", str(out / "physical")])
    for scenario in ("silent-pod", "malformed", "non-advancing", "counter-jump", "hysteresis", "high-rate", "receive-overflow"):
        run("original asserted " + scenario + " workload; no-persistence retains its original meaning",
            [".venv/bin/python", "tests/fault_scenarios.py", "--daemon", "build-resilience-debug/vigild", "--scenario", scenario,
             "--output", str(out / scenario)])
    for suite in ("test_queue", "test_outbox"):
        run("Helgrind " + suite + ": zero unsuppressed races", ["valgrind", "--tool=helgrind", "--error-exitcode=99", "--fair-sched=yes", "--suppressions=tests/helgrind.supp", "build-resilience-debug/" + suite, *(["--handoff-only"] if suite == "test_outbox" else [])])
    analyzer = ROOT / "build-resilience-analyze"
    if args.clean and analyzer.exists():
        shutil.rmtree(analyzer)
    run("strict GCC analyzer configure", ["cmake", "-S", ".", "-B", str(analyzer), "-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_C_COMPILER=gcc", "-DCMAKE_C_FLAGS=-fanalyzer", "-DBUILD_TESTING=OFF"])
    run("GCC analyzer: zero unsuppressed findings", ["cmake", "--build", str(analyzer), "--parallel", "2"])
    run("original independent can-utils exchange", [".venv/bin/python", "tests/manual_commands.py", "--decoder", "build-resilience-debug/vigil-decode", "--output", str(out / "manual")])
    run("original decoder libFuzzer/ASan/UBSan 120-second budget", ["./scripts/fuzz", "120", str(out / "fuzz")])
    run("hosted CI commands including v1/v2 PG contracts", ["./scripts/check-ci", "--clean"])
    run("production-binary interview: outage, durable backlog, SIGKILL/restart, old-run replay, ambiguous duplicate suppression and exact final inventory",
        ["./scripts/demo", "--build", "build-resilience-release", "--output", str(out / "demo")])
    run("three alternating matched original/resilience runs: excluded 2s warmup, >=60s measured workload, exact counts, latency distributions and resource samples",
        ["./scripts/benchmark", "--build", "build-resilience-release", "--runs", "3", "--seconds", "60", "--output", str(out / "benchmark")])
    for path in sorted((ROOT / "scripts").iterdir()):
        run("shell syntax " + path.name, ["bash", "-n", str(path)])
    run("diff whitespace", ["git", "diff", "--check"])
    require((report["source_sha256"] == source_digest()), lambda: ("implementation changed during gate"))
    report["completed_utc"] = datetime.now(timezone.utc).isoformat()
    (out / "gate.json").write_text(json.dumps(report, indent=2))
    print(f"PASS release-candidate clean gate: {len(commands)} commands; {out / 'gate.json'}", flush=True)


if __name__ == "__main__":
    main()
