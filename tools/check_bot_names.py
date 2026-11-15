#!/usr/bin/env python3
"""Rule check for data/bot_names.txt (see doc/bot-names.md). Exit 1 on any problem.

Checks: row format, race/class/sex/weight values, name rules, duplicate rows, a name repeated in one
(kind, style) (merge the races into one row instead), and first names (also the first name of a pair)
that the client DB2 reserved/profanity lists reject. The lists are read from data/name_filters.txt, which
tools/gen_name_filters.py generates from NamesReserved.db2 and NamesProfanity.db2."""
import os, re, sys
path = sys.argv[1] if len(sys.argv) > 1 else "data/bot_names.txt"
RACES = {1, 2, 3, 4, 5, 6, 7, 8, 10, 11}
CLASSES = {1, 2, 3, 4, 5, 7, 8, 9, 11}
STYLES = {"normal", "funny", "lore"}
first_re = re.compile(r"^[A-Za-z]{2,12}$")
last_re = re.compile(r"^[A-Za-z][A-Za-z '\-]{1,23}$")
FILTERS = os.path.join(os.path.dirname(os.path.abspath(path)), "name_filters.txt")
filters = []
if not os.path.exists(FILTERS):
    print(f"missing {FILTERS} (run tools/gen_name_filters.py)"); sys.exit(1)
for fl in open(FILTERS, encoding="utf-8"):
    fl = fl.rstrip("\n")
    if fl and not fl.startswith("#") and "|" in fl:
        kind_, rx = fl.split("|", 1)
        filters.append((kind_, re.compile(rx.replace("\\<", r"\b").replace("\\>", r"\b"), re.I)))
def rejected(name):
    return next((k for k, rx in filters if rx.search(name)), None)
bad = 0
seen = set()
seen_names = {}
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
    if kind in ("first", "pair"):
        why = rejected(texts[0])
        if why: print(f"line {n}: first name '{texts[0]}' is rejected by the client {why} list"); bad += 1
    nkey = (kind, f[1], " ".join(texts).lower())
    if nkey in seen_names: print(f"line {n}: '{' '.join(texts)}' repeats line {seen_names[nkey]} ({kind}, {f[1]})"); bad += 1
    seen_names.setdefault(nkey, n)
    key = (kind, f[1], f[2], f[3], f[4], tuple(texts))
    if key in seen: print(f"line {n}: duplicate row"); bad += 1
    seen.add(key)
    counts[(kind, f[1])] = counts.get((kind, f[1]), 0) + 1
for k in sorted(counts): print(k[0], k[1], counts[k])
sys.exit(1 if bad else 0)
