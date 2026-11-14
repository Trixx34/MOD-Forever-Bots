#!/usr/bin/env python3
"""Checks data/grind_spots.txt and its filtered copy: well-formed rows, and the custom start map 2991 ('Skyborne') has spots for levels 3 to 14.
A map missing from the file makes its bots log grind_none and wander (sim 2026-11: ~531 events per 30 minutes). Usage: grind_data_test.py <module source dir>"""
import os
import sys

root = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("FAIL:", what)


for name in ("grind_spots.txt", "grind_spots.filtered.txt"):
    spots = []
    for number, line in enumerate(open(os.path.join(root, "data", name), encoding="utf-8"), 1):
        f = line.split()
        if not f or f[0].startswith("#"):
            continue
        ok = f[0] == "spot" and len(f) >= 8
        check(ok, f"{name}:{number} is not a spot row")
        if not ok:
            continue
        try:
            row = (int(f[1]), float(f[2]), float(f[3]), float(f[4]), int(f[5]), int(f[6]), int(f[7]))
        except ValueError:
            check(False, f"{name}:{number} has a non-numeric field")
            continue
        check(row[4] <= row[5], f"{name}:{number} min level above max level")
        spots.append(row)
    for map_id in (0, 1, 2991):
        check(any(s[0] == map_id for s in spots), f"{name} has no spot for map {map_id}")
    skyborne = [s for s in spots if s[0] == 2991]
    for level in range(3, 15):
        check(any(s[4] <= level <= s[5] for s in skyborne), f"{name}: no spot on map 2991 for level {level}")

print("grind data test:", "FAILED" if failures else "ok")
sys.exit(1 if failures else 0)
