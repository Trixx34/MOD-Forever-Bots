#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Checks tools/gen_gather_nodes.py on a made-up export: names to skill and required value, area level and side from the grind spots, nodes left out.

Usage: gen_gather_nodes_test.py <repo root>
"""
import os
import subprocess
import sys
import tempfile

ROOT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
TOOL = os.path.join(ROOT, "tools", "gen_gather_nodes.py")

failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("FAIL", what)


NODES = "\n".join([
    "0\t-100\t-100\t10\t1618\tPeacebloom",               # near the low spot, Alliance
    "0\t-120\t-90\t10\t1731\tCopper Vein",
    "0\t-110\t-100\t10\t73940\tOoze Covered Silver Vein",
    "0\t5000\t5000\t10\t1617\tSilverleaf",               # no spot near
    "0\t3000\t3000\t10\t176589\tBlack Lotus",            # spot of level 58: above --max-level 50
    "1\t-300\t-300\t10\t1618\tPeacebloom",               # other map: no spot there
    "0\t-100\t-100\t10\t5\tSome Chest",                  # unknown name
    "0\toops\t-100\t10\t1\tPeacebloom",                  # bad row
    "1\t10\t10\t5\t1732\tTin Vein",
])
SPOTS = "\n".join([
    "# comment",
    "spot 0 -100 -100 10 1 5 1 9",
    "spot 0 -150 -100 10 3 7 2 9",
    "spot 0 3000 3000 10 58 60 0 5",
    "spot 1 0 0 0 10 12 2 5",
])

with tempfile.TemporaryDirectory() as tmp:
    nodes, spots, out = (os.path.join(tmp, n) for n in ("nodes.tsv", "spots.txt", "out.txt"))
    open(nodes, "w").write(NODES + "\n")
    open(spots, "w").write(SPOTS + "\n")
    res = subprocess.run([sys.executable, "-I", TOOL, "--nodes", nodes, "--spots", spots, "--out", out, "--max-level", "50"], capture_output=True, text=True)
    check(res.returncode == 0, "tool exits 0: " + res.stderr)
    rows = [l.split() for l in open(out) if not l.startswith("#")]
    by_entry = {r[5]: r for r in rows}
    check(len(rows) == 4, f"4 rows, got {len(rows)}")
    # skill, map, x, y, z, entry, required, level, side
    check(by_entry.get("1618") and by_entry["1618"][:2] == ["182", "0"] and by_entry["1618"][6:] == ["1", "1", "1"], "peacebloom: herbalism, req 1, level 1, side 1")
    check(by_entry.get("1731") and by_entry["1731"][0] == "186" and by_entry["1731"][6] == "1", "copper: mining req 1")
    check(by_entry.get("73940") and by_entry["73940"][6] == "75", "ooze covered silver takes the silver value")
    check(by_entry.get("1732") and by_entry["1732"][1] == "1" and by_entry["1732"][6:] == ["65", "10", "2"], "tin on map 1 with its own spot")
    check("1617" not in by_entry and "176589" not in by_entry and "5" not in by_entry, "far, too high and unknown nodes are left out")
    check("no_spot_near" in res.stdout and "above_max_level" in res.stdout and "unknown_name" in res.stdout, "the summary names the reasons")

sys.exit(1 if failures else 0)
