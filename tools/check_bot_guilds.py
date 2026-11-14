#!/usr/bin/env python3
"""Rule check for data/bot_guilds.txt: guild|style|weight|name. Exit 1 on any problem."""
import re, sys
path = sys.argv[1] if len(sys.argv) > 1 else "data/bot_guilds.txt"
rx = re.compile(r"^[A-Za-z][A-Za-z '\-]{1,23}$")
STYLES = {"lore", "funny", "normal", "reallife"}
bad = 0; seen = set(); counts = {}
for n, line in enumerate(open(path, encoding="utf-8"), 1):
    line = line.rstrip("\n")
    if not line.strip() or line.startswith("#"): continue
    f = line.split("|")
    if len(f) != 4 or f[0] != "guild": print(f"line {n}: expected guild|style|weight|name"); bad += 1; continue
    if f[1] not in STYLES: print(f"line {n}: bad style '{f[1]}'"); bad += 1
    if not f[2].isdigit() or not 1 <= int(f[2]) <= 100: print(f"line {n}: bad weight '{f[2]}'"); bad += 1
    if not rx.match(f[3]): print(f"line {n}: bad name '{f[3]}'"); bad += 1
    if f[3].lower() in seen: print(f"line {n}: duplicate '{f[3]}'"); bad += 1
    seen.add(f[3].lower()); counts[f[1]] = counts.get(f[1], 0) + 1
print(counts); sys.exit(1 if bad else 0)
