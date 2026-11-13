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
DIED = os.path.join(ROOT, "tests", "data", "died-sample.log")
deaths = run("deaths", DIED)
check(deaths.returncode == 0, "deaths exit code " + deaths.stderr)
rows = [line.split() for line in deaths.stdout.splitlines()]
table = {(r[0], r[1]): r for r in rows if len(r) >= 6 and r[0] != "attacker"}
check(("644", "20566") in table, "the 644/20566 row: " + deaths.stdout)
if ("644", "20566") in table:
    r = table[("644", "20566")]
    check(r[-4:] == ["2", "6000", "300", "50"], "two deaths, 3100+2900 damage, 250+50 overkill, avg hp 50: " + " ".join(r))
check(("644", "0") in table and table[("644", "0")][-4:] == ["1", "800", "10", "20"], "the melee row")
check(("none", "3355") in table, "a death with no attacker entry")
check(("1196", "?") in table and "guess" in deaths.stdout, "the guess row is marked")
check("6 deaths, 2 without a damage event (guess)" in deaths.stdout, "the totals: " + deaths.stdout)
names = os.path.join(os.environ.get("TMPDIR", "/tmp"), f"spells-{os.getpid()}.txt")
with open(names, "w") as handle:
    handle.write("# id name\n20566 Wound Crush\n3355,Freezing Trap\n0 Melee swing\n")
try:
    named = run("deaths", DIED, "--spells", names, "--top", "1")
finally:
    os.unlink(names)
check("Wound Crush" in named.stdout and "Melee swing" not in named.stdout and "Freezing Trap" not in named.stdout, "spell names from --spells, --top 1: " + named.stdout)
nodeaths = subprocess.run([sys.executable, "-I", TOOL, "deaths", "-"], input="nothing here\n", capture_output=True, text=True)
check(nodeaths.returncode == 1, "deaths with no died: lines: exit 1")
dmg_log = "2026-11-12T10:05:00Z forever-bots: event boss_attempt: scenario=dm party=1 variant= t_s=640 boss=Mr._Smite stage=end dur_s=80 deaths=1 outcome=killed dmg_by_spell=20566:4500:2:1 dmg_by_attacker=644:4500:2:1 death_by_spell=20566:1:50 death_by_attacker=644:1:50\n" \
    "2026-11-12T10:05:30Z forever-bots: event party_tl: scenario=dm party=1 variant= t_s=660 leader=1 map=36 pos=0.0,0.0,0.0 step=fight route=1/2 target=0 alive=3/3 fight=1 last=route:1@5 dmg_by_spell=20566:4500:2:1,7:400:1:0 dmg_by_attacker=644:4500:2:1\n"
summary = subprocess.run([sys.executable, "-I", TOOL, "-"], input=dmg_log, capture_output=True, text=True)
check("boss dmg   dmg_by_spell=20566:4500:2:1" in summary.stdout, "the boss attempt's damage in the summary: " + summary.stdout)
check("dmg taken  by spell 20566:4500 7:400  by attacker 644:4500" in summary.stdout, "the party damage in the summary: " + summary.stdout)
print("run_telemetry_test: " + ("all checks passed" if failures == 0 else f"{failures} failed"))
sys.exit(1 if failures else 0)
