#!/usr/bin/env python3
"""Builds data/rotations.txt from a rotation spec (spells by NAME) and the core's ability dump, so every spell id and level is the real one.

Why: spell ids differ between versions and a wrong id fails silently. The core can log what each class gets at each level (Bot.Dump.Abilities = 1 in
worldserver.conf, one dump per race and class, lines `BotAbilities: race R class C level L: N new spell(s), M in total: <id> <name>; <id> <name>; ...`
in Server.log). This tool reads those lines, finds each spell of the spec by name, and writes the rotation file (src/combat_rotation.h) with the id of
the lowest rank (the core casts the highest rank the bot knows) and the level the spell is learned at (a lower bound: the dump looks at levels 1, 5, 10,
15 ... so the level is the one after the previous dump level).

Spec: the same records as the rotation file, with the spell written as a name in quotes:
    class 8
    buff "Frost Armor" duration 1800 self
    row "Frostbolt" always target range 0 30
    supersede "Frost Armor" by "Ice Armor"       # the better spell replaces the other from the level it is learned (maxlevel on the old one)
A number in place of a name is used as it is.

Usage: make_rotations.py --dump Server.log --spec data/rotation_spec.txt > data/rotations.txt
"""
import argparse
import re
import shlex
import sys
from collections import defaultdict

DUMP = re.compile(r"BotAbilities: race (\d+) class (\d+) level (\d+): \d+ new spell\(s\), \d+ in total: ?(.*)$")


def read_dump(paths):
    # class -> name -> {spell id: lowest level bound}
    spells = defaultdict(lambda: defaultdict(dict))
    previous = {}
    entries = []
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as handle:
            for line in handle:
                match = DUMP.search(line)
                if match:
                    entries.append((int(match.group(1)), int(match.group(2)), int(match.group(3)), match.group(4)))
    entries.sort(key=lambda e: (e[0], e[1], e[2]))
    for race, cls, level, text in entries:
        low = previous.get((race, cls), 0) + 1
        previous[(race, cls)] = level
        for item in text.split("; "):
            item = item.strip()
            m = re.match(r"(\d+) (.+)$", item)
            if not m or m.group(2) == "?":
                continue
            sid, name = int(m.group(1)), m.group(2)
            known = spells[cls][name]
            known[sid] = min(known.get(sid, 10**6), low)
    return spells


def resolve(cls_spells, token, warnings, where):
    if token.isdigit():
        return int(token), None, None
    ids = cls_spells.get(token)
    if not ids:
        warnings.append(f"{where}: no spell named '{token}' for this class in the dump")
        return None, None, None
    rank1 = min(ids, key=lambda sid: (ids[sid], sid))   # the rank learned first; a later rank may carry a lower id (Consecration 26573 at 20, 20116 at 30)
    return rank1, ids[rank1], sorted(ids)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dump", action="append", required=True, help="Server.log with BotAbilities lines (repeatable)")
    parser.add_argument("--spec", required=True)
    args = parser.parse_args()
    spells = read_dump(args.dump)
    if not spells:
        sys.exit("no BotAbilities lines found in the dump (set Bot.Dump.Abilities = 1 and create a bot of each race and class)")

    out = ["# Rotations. Made by tools/make_rotations.py from the spec and the core's ability dump. Format: src/combat_rotation.h"]
    warnings = []
    cls = 0
    spec_key = ""        # the spec of the current block (class N spec KEY); empty: the class default
    pending = []          # the records of the current class, resolved at its end
    supersede = []        # (old name, new name)

    def flush():
        nonlocal pending, supersede
        if cls == 0:
            return
        out.append("")
        out.append(f"class {cls}" + (f" spec {spec_key}" if spec_key else ""))
        levels = {}
        for kind, tokens, name_token in pending:
            sid, level, ranks = resolve(spells.get(cls, {}), name_token, warnings, f"class {cls}")
            if sid is None:
                continue
            levels[name_token] = level
            extra = tokens
            comment = f"   # {name_token}: ranks {ranks}" if ranks else ""
            out.append(f"{kind} {sid}{extra}{comment}")
        # The supersede pairs: the old spell stops at the level before the new one is learned.
        for old, new in supersede:
            old_level, new_level = levels.get(old), levels.get(new)
            if old_level is None or new_level is None:
                warnings.append(f"class {cls}: supersede '{old}' by '{new}' needs both spells found")
                continue
            sid_old = resolve(spells.get(cls, {}), old, [], "")[0]
            for i, line in enumerate(out):
                if line.startswith(("row ", "buff ")) and line.split()[1] == str(sid_old):
                    parts = line.split("#")[0].rstrip()
                    out[i] = parts + f" maxlevel {max(new_level - 1, 1)}" + (("   #" + line.split("#", 1)[1]) if "#" in line else "")
        pending, supersede = [], []

    with open(args.spec, encoding="utf-8") as handle:
        for number, raw in enumerate(handle, 1):
            line = raw.split("#")[0].strip()
            if not line:
                continue
            words = shlex.split(line)
            if words[0] == "class":
                flush()
                cls = int(words[1])
                spec_key = words[3] if len(words) == 4 and words[2] == "spec" else ""
                continue
            if words[0] == "supersede" and len(words) == 4 and words[2] == "by":
                supersede.append((words[1], words[3]))
                continue
            if words[0] in ("row", "buff") and len(words) >= 2:
                name = words[1]
                rest = words[2:]
                # Add the level the spell is learned at, unless the spec gave one.
                info = resolve(spells.get(cls, {}), name, [], "")
                level = info[1]
                if "level" not in rest and level:
                    rest = ["level", str(level)] + rest
                pending.append((words[0], " " + " ".join(rest) if rest else "", name))
                continue
            warnings.append(f"spec line {number}: not understood")
    flush()

    print("\n".join(out))
    for w in warnings:
        print("WARNING " + w, file=sys.stderr)
    print(f"{sum(1 for l in out if l.startswith(('row ', 'buff ')))} records, {len(warnings)} warning(s)", file=sys.stderr)


if __name__ == "__main__":
    main()
