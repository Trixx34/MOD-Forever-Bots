"""Tests for tools/soak_report.py: runs whose clock starts again are not merged, and counters that reset at a restart are added up."""
import os
import subprocess
import sys
import tempfile

HEADER = "time_ms,kind,bot,level,health_pct,power_pct,in_fight,fights,targets_gone,deaths,casts_applied,refusals,stuck_episodes,longest_fight_ms,health_lost,detail\n"
TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "soak_report.py")


def row(time_ms, kind, bot, level, fights=0, deaths=0, detail=""):
    return f"{time_ms},{kind},{bot},{level},100,100,0,{fights},{fights},{deaths},0,0,0,0,0,{detail}\n"


def report(files):
    with tempfile.TemporaryDirectory() as directory:
        for name, text in files.items():
            with open(os.path.join(directory, name), "w") as handle:
                handle.write(HEADER + text)
        done = subprocess.run([sys.executable, "-I", TOOL, "--telemetry", os.path.join(directory, "t.csv")], capture_output=True, text=True)
        assert done.returncode == 0, done.stderr
        return done.stdout


failures = 0


def check(condition, message):
    global failures
    if not condition:
        failures += 1
        print("FAIL", message)


MIN = 60000

# An archived run with a high clock (booted long ago), then a run after a reboot that carries on from the same levels, then the live file after a
# module restart on the same boot (counters start again).
old = "".join(row(16_000_000 + i * MIN, "summary", 1, 1 + i // 5, fights=10 * i) for i in range(10))
old += row(16_000_000 + 9 * MIN + 1, "level", 1, 2, detail="from 1 to 2")
rebooted = "".join(row(100_000 + i * MIN, "summary", 1, 3 + i // 10, fights=10 * i) for i in range(30))
live = "".join(row(100_000 + 31 * MIN + i * MIN, "summary", 1, 4, fights=3 * i) for i in range(10))
text = report({"t.csv": live, "t.csv.20261014-120000": old, "t.csv.1": rebooted})
check("2 separate runs" in text, "two runs: " + text)
check("after a restart of the machine" in text, "clock restart noted")
# 9 + 29 + 2 + 9 minutes = 49 minutes (a quick module restart is not a hole), not the span between the largest and the smallest clock value.
check("0.82 h of runs" in text, "active time: " + text)
# fights: 90 (old, 10*9) + 290 + 27 added up across the resets.
check("Fights 407" in text, "fights added across restarts: " + text)

# Rows in one run with a long hole in them are two stretches, and the hole is not counted.
hole = "".join(row(i * MIN, "summary", 1, 1) for i in range(5)) + "".join(row(60 * MIN + i * MIN, "summary", 1, 1) for i in range(5))
text = report({"t.csv": hole})
check("2 separate runs" in text and "after a 56 min stretch with no rows" in text, "hole: " + text)

# A single run is reported as before.
text = report({"t.csv": "".join(row(i * MIN, "summary", 1, 1) for i in range(61))})
check("separate runs" not in text and "1.00 h of runs" in text, "single run: " + text)

if failures == 0:
    print("soak_report_test: all checks passed")
sys.exit(1 if failures else 0)
