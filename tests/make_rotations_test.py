#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Checks tools/make_rotations.py against data/rotation_spec.txt with a made-up ability dump: the Forever-specific rows of the spec must resolve to the
real spell ids (ids as in the Forever client data) in the right priority order, and a dump without those spells must give warnings and no rows (no guessed ids).

Usage: make_rotations_test.py <repo root>
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
TOOL = os.path.join(ROOT, "tools", "make_rotations.py")
SPEC = os.path.join(ROOT, "data", "rotation_spec.txt")

# class -> {name: id}; ids read from the Forever client data (rank 1 / the lowest id of the name in the class's skill lines)
FOREVER = {
    1: {"Shockwave": 440488, "Spearing Strike": 1310222},
    2: {"Rebuke": 425609, "Holy Strike": 678, "Hammer of the Righteous": 407632},
    4: {"Saber Slash": 424785},
    5: {"Void Plague": 425204, "Vampiric Touch": 402668},
    7: {"Lava Burst": 408490, "Water Shield": 408510},
    8: {"Icy Veins": 425121, "Ice Lance": 400640},
    9: {},  # no Forever row: Fel Armor is left out (no trainer teaches it)
    11: {"Starsurge": 417157, "Sunfire": 414684},
}
VANILLA = {
    1: {"Heroic Strike": 78, "Thunder Clap": 6343, "Cleave": 845},
    2: {"Judgement": 20271, "Hammer of Justice": 853},
    3: {"Multi-Shot": 2643},
    4: {"Sinister Strike": 1752, "Eviscerate": 2098},
    5: {"Mind Blast": 8092, "Smite": 585},
    7: {"Lightning Bolt": 403, "Lightning Shield": 324},
    8: {"Frostbolt": 116},
    9: {"Shadow Bolt": 686, "Demon Armor": 706, "Demon Skin": 687, "Summon Imp": 688, "Summon Voidwalker": 697, "Summon Succubus": 712},
    11: {"Wrath": 5176},
}

failures = 0
blocks = {}   # (class, spec) -> the rows of that block, of the last run


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("FAIL", what)


def dump_line(cls, spells):
    text = "; ".join(f"{sid} {name}" for name, sid in spells.items())
    return f"BotAbilities: race 1 class {cls} level 60: {len(spells)} new spell(s), {len(spells)} in total: {text}"


def run(dump_text):
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "Server.log")
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(dump_text)
        done = subprocess.run([sys.executable, "-I", TOOL, "--dump", path, "--spec", SPEC], capture_output=True, text=True, check=True)
    classes = {}
    blocks.clear()
    cls = 0
    spec = ""
    for line in done.stdout.splitlines():
        words = line.split("#")[0].split()
        if len(words) in (2, 4) and words[0] == "class":
            # `class N` is the class default, `class N spec KEY` one spec: the checks below look at all the blocks of a class together
            cls = int(words[1])
            spec = words[3] if len(words) == 4 else ""
            classes.setdefault(cls, [])
            blocks[(cls, spec)] = []
        elif words and words[0] in ("row", "buff"):
            classes[cls].append(line.split("#")[0].strip())
            blocks[(cls, spec)].append(line.split("#")[0].strip())
    return classes, done.stderr


def ids(rows):
    return [int(r.split()[1]) for r in rows]


# 1. every Forever row resolves, with the ids of the client data, in front of the vanilla filler it is meant to precede
full = "\n".join(dump_line(c, {**FOREVER[c], **VANILLA[c]}) for c in FOREVER) + "\n"
classes, err = run(full)
for cls, spells in FOREVER.items():
    got = ids(classes.get(cls, []))
    for name, sid in spells.items():
        check(sid in got, f"class {cls}: {name} ({sid}) is in the rotation")
order = lambda cls, a, b: ids(classes[cls]).index(a) < ids(classes[cls]).index(b)
check(order(2, 678, 20271), "paladin: Holy Strike before Judgement")
check(order(4, 424785, 1752), "rogue: Saber Slash before Sinister Strike")
block_order = lambda cls, spec, a, b: ids(blocks[(cls, spec)]).index(a) < ids(blocks[(cls, spec)]).index(b)
check(block_order(1, "prot", 1310222, 845), "warrior protection: Spearing Strike before Cleave")
check(order(8, 400640, 116), "mage: Ice Lance before Frostbolt")
check(block_order(5, "shadow", 402668, 8092), "priest shadow: Vampiric Touch before Mind Blast")
check(order(11, 417157, 5176), "druid: Starsurge before Wrath")

row = lambda cls, sid: next(r for r in classes[cls] if r.split()[1] == str(sid))
check("duration 12" in row(4, 424785), "Saber Slash is a 12 s bleed")
check("duration 18" in row(5, 425204) and "duration 15" in row(5, 402668), "priest damage over time lengths")
check("range 0 5" in row(2, 678), "Holy Strike is a melee strike")
check(row(7, 408510).startswith("buff") and "duration 600" in row(7, 408510), "Water Shield is a 10 minute upkeep buff")
# Fel Armor (403619) is left out: no trainer teaches it, so a bot never knows it, and a supersede to it would stop Demon Armor for good
check(not any(r.split()[1] == "403619" for r in classes[9]), "Fel Armor is not a warlock row")
# the better shield retires the old one from the level it is learned
check("maxlevel" in row(7, 324), "Lightning Shield is retired by Water Shield")
check("maxlevel" not in row(9, 706), "Demon Armor is not retired: it stays the armor")

# 1b. the spec blocks: a block per spec with its own rows, the class default block kept, and the pet summons as no_pet upkeep buffs
with tempfile.TemporaryDirectory() as tmp:
    path = os.path.join(tmp, "Server.log")
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(full)
    text = subprocess.run([sys.executable, "-I", TOOL, "--dump", path, "--spec", SPEC], capture_output=True, text=True, check=True).stdout
headers = [line.split("#")[0].strip() for line in text.splitlines() if line.startswith("class ")]
check("class 3" in headers and "class 3 spec bm" in headers and "class 3 spec sv" in headers, "hunter: a default block and a block per spec")
check("class 9 spec demonology" in headers and "class 9 spec destruction" in headers, "warlock: demonology and destruction blocks")
check("class 8 spec arcane" in headers and "class 8 spec frost" in headers, "mage: arcane and frost blocks")
check(len(headers) == len(set(headers)), "no block twice")
demonology = text.split("class 9 spec demonology")[1].split("\nclass ")[0]
check(re.search(r"^buff \d+ .*self no_pet", demonology, re.M) is not None, "the demon is a no_pet upkeep buff")

# 2. a dump with only the vanilla spells: no Forever row, no guessed id, a warning for each
vanilla = "\n".join(dump_line(c, VANILLA[c]) for c in VANILLA) + "\n"
classes, err = run(vanilla)
for cls, spells in FOREVER.items():
    got = ids(classes.get(cls, []))
    for name, sid in spells.items():
        check(sid not in got, f"class {cls}: {name} is dropped when the dump lacks it")
        check(f"no spell named '{name}'" in err, f"a warning for {name}")
for cls, spells in VANILLA.items():
    check(all(sid in ids(classes.get(cls, [])) for sid in spells.values()), f"class {cls}: vanilla rows are kept")

if failures:
    print(f"{failures} failure(s)")
    sys.exit(1)
print("make_rotations_test: all passed")
