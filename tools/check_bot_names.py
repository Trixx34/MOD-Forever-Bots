#!/usr/bin/env python3
"""Rule check for data/bot_names.txt (see doc/bot-names.md). Exit 1 on any problem."""
import re, sys
path = sys.argv[1] if len(sys.argv) > 1 else "data/bot_names.txt"
RACES = {1, 2, 3, 4, 5, 6, 7, 8, 10, 11}
CLASSES = {1, 2, 3, 4, 5, 7, 8, 9, 11}
STYLES = {"normal", "funny", "lore"}
first_re = re.compile(r"^[A-Za-z]{2,12}$")
last_re = re.compile(r"^[A-Za-z][A-Za-z '\-]{1,23}$")
bad = 0
seen = set()
counts = {}
def csv(text, allowed, what, n):
    global bad
    if text == "*":
        return True
    for part in text.split(","):
        if not part.isdigit() or int(part) not in allowed:
            print(f"line {n}: bad {what} '{text}'"); bad += 1; return False
    return True
for n, line in enumerate(open(path, encoding="utf-8"), 1):
    line = line.rstrip("\n")
    if not line.strip() or line.lstrip().startswith("#"):
        continue
    f = line.split("|")
    kind = f[0]
    need = 8 if kind == "pair" else 7
    if kind not in ("first", "last", "pair") or len(f) != need:
        print(f"line {n}: expected {'pair|style|races|classes|sex|weight|first|last' if kind == 'pair' else 'kind|style|races|classes|sex|weight|text'}"); bad += 1; continue
    if f[1] not in STYLES: print(f"line {n}: bad style '{f[1]}'"); bad += 1
    csv(f[2], RACES, "races", n); csv(f[3], CLASSES, "classes", n)
    if f[4] not in ("*", "m", "f"): print(f"line {n}: bad sex '{f[4]}'"); bad += 1
    if not f[5].isdigit() or not 1 <= int(f[5]) <= 100: print(f"line {n}: bad weight '{f[5]}'"); bad += 1
    texts = f[6:]
    for i, t in enumerate(texts):
        ok = first_re.match(t) if (kind == "first" or (kind == "pair" and i == 0)) else last_re.match(t)
        if not ok: print(f"line {n}: bad name '{t}'"); bad += 1
    key = (kind, f[1], f[2], f[3], f[4], tuple(texts))
    if key in seen: print(f"line {n}: duplicate row"); bad += 1
    seen.add(key)
    counts[(kind, f[1])] = counts.get((kind, f[1]), 0) + 1
for k in sorted(counts): print(k[0], k[1], counts[k])
sys.exit(1 if bad else 0)
