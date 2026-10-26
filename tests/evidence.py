"""Small evidence recorder: expectations first, observed results, failure tracebacks."""
from verification import require  # also refuses optimized execution before fixture imports
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import time
import traceback

ROOT = Path(__file__).resolve().parents[1]


def utc():
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def source_digest():
    """Hash implementation paths, including new files; exclude generated artifacts/docs."""
    names = subprocess.check_output(["git", "ls-files", "--cached", "--others", "--exclude-standard"],
                                    cwd=ROOT, text=True).splitlines()
    prefixes = ("src/", "include/", "tests/", "scripts/", "simulator/", "migrations/",
                "systemd/", "config/", ".github/", "fuzz/", "tools/", "env/", "embedded/")
    digest = hashlib.sha256()
    for name in sorted(set(names)):
        if name == "CMakeLists.txt" or name.startswith(prefixes):
            digest.update(name.encode() + b"\0" + (ROOT / name).read_bytes() + b"\0")
    return digest.hexdigest()


class Evidence:
    def __init__(self, directory):
        self.directory = Path(directory).resolve()
        self.directory.mkdir(parents=True, exist_ok=False)
        self.old_env = os.environ.get("VIGIL_EVIDENCE_DIR")
        self.report = dict(started_utc=utc(), command=sys.argv, platform=platform.platform(),
                           python=sys.version, python_optimize=sys.flags.optimize, commit=subprocess.check_output(
                               ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                           git_status=subprocess.check_output(
                               ["git", "status", "--porcelain"], cwd=ROOT, text=True),
                           source_sha256=source_digest(), scenarios=[])
        self.save()

    def save(self):
        self.report["passed"] = sum(r["result"] == "PASS" for r in self.report["scenarios"])
        self.report["failed"] = sum(r["result"] == "FAIL" for r in self.report["scenarios"])
        (self.directory / "results.json").write_text(json.dumps(self.report, indent=2) + "\n")

    def run(self, name, expected, action):
        directory = self.directory / name
        directory.mkdir()
        os.environ["VIGIL_EVIDENCE_DIR"] = str(directory)
        record = dict(name=name, expected=expected, started_utc=utc(), result="RUNNING")
        self.report["scenarios"].append(record)
        self.save()
        print(f"\n[{record['started_utc']}] EXPECT {name}: {expected}", flush=True)
        start = time.monotonic()
        try:
            record["measurements"] = action() or {}
            record["result"] = "PASS"
        except BaseException:
            record["result"] = "FAIL"
            record["traceback"] = traceback.format_exc()
            raise
        finally:
            record["elapsed_s"] = round(time.monotonic() - start, 6)
            record["ended_utc"] = utc()
            self.save()
            print(f"[{record['ended_utc']}] {record['result']} {name}: "
                  f"{json.dumps(record.get('measurements', {}), sort_keys=True)}", flush=True)
            if self.old_env is None:
                os.environ.pop("VIGIL_EVIDENCE_DIR", None)
            else:
                os.environ["VIGIL_EVIDENCE_DIR"] = self.old_env

    def finish(self):
        self.report["ended_utc"] = utc()
        self.save()
        print(f"\nEvidence: {self.report['passed']} passed, {self.report['failed']} failed; "
              f"{self.directory / 'results.json'}", flush=True)
