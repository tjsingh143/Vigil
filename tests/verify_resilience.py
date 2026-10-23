#!/usr/bin/env python3
"""Clean Stage 1 gate; keeps original sensor/protocol/v1-PG contracts and adds
local durability gates. No Stage 2 replay assertions or generated release report.
"""
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--clean", action="store_true")
    ap.add_argument("--output", type=Path, default=Path("artifacts") / ("resilience-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")))
    args = ap.parse_args()
    assert sys.platform == "linux" and not sys.flags.optimize
    os.chdir(ROOT)
    args.output.mkdir(parents=True, exist_ok=False)
    out = args.output.resolve()
    commands = []
    report = dict(started_utc=datetime.now(timezone.utc).isoformat(), source_sha256=source_digest(),
                  head=subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
                  kernel=platform.platform(), commands=commands)
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
               TSAN_OPTIONS="halt_on_error=1")
    def run(expected, cmd):
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
        assert result.returncode == 0, f"failed {shlex.join(cmd)}: {out / entry['log']}"
    run("real Linux vcan0, otherwise idle", ["./scripts/vcan-up"])
    run("seven existing Python cases", [".venv/bin/python", "-m", "unittest", "discover", "-s", "tests", "-p", "test_*.py", "-v"])
    run("five original PostgreSQL v1 schema/transaction/permission contracts on disposable server", [".venv/bin/python", "tests/database_contract.py"])
    builds = [("debug", "Debug", "gcc", []), ("release", "Release", "gcc", []),
              ("asan", "Debug", "clang", ["-DVIGIL_SANITIZERS=ON"]),
              ("tsan", "Debug", "gcc", ["-DVIGIL_TSAN=ON"])]
    for name, kind, cc, flags in builds:
        build = "build-resilience-" + name
        if args.clean and (ROOT / build).exists():
            shutil.rmtree(ROOT / build)
        run(f"strict {name} configure", ["cmake", "-S", ".", "-B", build, "-DCMAKE_BUILD_TYPE=" + kind, "-DCMAKE_C_COMPILER=" + cc, *flags])
        run(f"strict {name} build, no warnings", ["cmake", "--build", build, "--parallel", "2"])
        prefix = ["setarch", platform.machine(), "-R"] if name == "tsan" else []
        run(f"60 C cases: 13 protocol, 26 monitor, 6 queue, 15 local outbox; zero sanitizer findings ({name})",
            [*prefix, "ctest", "--test-dir", build, "-V"])
        if name != "tsan":
            run("unchanged real SocketCAN decoder regression (" + name + ")", [".venv/bin/python", "tests/integration.py", "--decoder", build + "/vigil-decode"])
        run("unchanged live monitoring regression (" + name + ")", [*prefix, ".venv/bin/python", "tests/monitor_integration.py", "--daemon", build + "/vigild"])
        run("exact crash cuts, stable DB/WAL payloads, capture races, faults, priority and startup (" + name + ")",
            [*prefix, ".venv/bin/python", "tests/resilience_integration.py", "--build", build, "--output", str(out / name),
             "--restarts", "20" if name == "debug" else "2"])
    run("real ext4 ENOSPC/read-only and unprivileged hardened systemd SIGKILL/watchdog recovery",
        [".venv/bin/python", "tests/resilience_storage.py", "--build", "build-resilience-debug", "--output", str(out / "physical")])
    for scenario in ("silent-pod", "malformed", "non-advancing", "counter-jump", "hysteresis", "high-rate", "receive-overflow"):
        run("original asserted " + scenario + " workload; no-persistence retains its original meaning",
            [".venv/bin/python", "tests/fault_scenarios.py", "--daemon", "build-resilience-debug/vigild", "--scenario", scenario,
             "--output", str(out / scenario)])
    for suite in ("test_queue", "test_outbox"):
        run("Helgrind " + suite + ": zero unsuppressed races", ["valgrind", "--tool=helgrind", "--error-exitcode=99", "--fair-sched=yes", "build-resilience-debug/" + suite, *(["--handoff-only"] if suite == "test_outbox" else [])])
    analyzer = ROOT / "build-resilience-analyze"
    if args.clean and analyzer.exists():
        shutil.rmtree(analyzer)
    run("strict GCC analyzer configure", ["cmake", "-S", ".", "-B", str(analyzer), "-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_C_COMPILER=gcc", "-DCMAKE_C_FLAGS=-fanalyzer", "-DBUILD_TESTING=OFF"])
    run("GCC analyzer: zero unsuppressed findings", ["cmake", "--build", str(analyzer), "--parallel", "2"])
    run("original independent can-utils exchange", [".venv/bin/python", "tests/manual_commands.py", "--decoder", "build-resilience-debug/vigil-decode", "--output", str(out / "manual")])
    run("original decoder libFuzzer/ASan/UBSan 120-second budget", ["./scripts/fuzz", "120", str(out / "fuzz")])
    for path in sorted((ROOT / "scripts").iterdir()):
        run("shell syntax " + path.name, ["bash", "-n", str(path)])
    run("diff whitespace", ["git", "diff", "--check"])
    assert report["source_sha256"] == source_digest(), "implementation changed during gate"
    report["completed_utc"] = datetime.now(timezone.utc).isoformat()
    (out / "gate.json").write_text(json.dumps(report, indent=2))
    print(f"PASS Stage 1 clean gate: {len(commands)} commands; {out / 'gate.json'}", flush=True)


if __name__ == "__main__":
    main()
