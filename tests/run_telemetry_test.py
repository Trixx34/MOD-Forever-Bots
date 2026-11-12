# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests for tools/run_telemetry.py on tests/data/run-telemetry-sample.log (a run file and a journal line mixed)."""
import os
import subprocess
import sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
TOOL = os.path.join(ROOT, "tools", "run_telemetry.py")
SAMPLE = os.path.join(ROOT, "tests", "data", "run-telemetry-sample.log")
failures = 0


def check(condition, message):
    global failures
    if not condition:
        failures += 1
        print("FAIL", message)


def run(*args):
    return subprocess.run([sys.executable, "-I", TOOL, *args], capture_output=True, text=True)


done = run(SAMPLE)
check(done.returncode == 0, "exit code " + done.stderr)
out = done.stdout
check("== dm#1 ==" in out and "== dm#2 ==" in out, "both parties listed")
check("walk:30s (25%)" in out and "fight:60s (50%)" in out and "rest:30s (25%)" in out, "time per step of dm#1: " + out)
check("route/5min 0m:3" in out, "route bucket of dm#1")
check("STALL      t=320s cause=fight_churn" in out and "pulls=14" in out, "the stall of dm#2")
check("boss       Rhahk'Zor t=70s dur=30s deaths=1 outcome=killed" in out, "the boss attempt, from a journal line")
only = run(SAMPLE, "--party", "dm#2")
check("== dm#1 ==" not in only.stdout and "== dm#2 ==" in only.stdout, "--party filter")
empty = subprocess.run([sys.executable, "-I", TOOL, "-"], input="nothing here\n", capture_output=True, text=True)
check(empty.returncode == 1, "no lines: exit 1")
print("run_telemetry_test: " + ("all checks passed" if failures == 0 else f"{failures} failed"))
sys.exit(1 if failures else 0)
