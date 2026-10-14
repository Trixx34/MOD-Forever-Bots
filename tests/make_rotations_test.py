#!/usr/bin/env python3
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
    2: {"Rebuke": 425609, "Holy Strike": 678, "Divine Storm": 407778, "Hammer of the Righteous": 407632},
    3: {"Black Arrow": 3674, "Chimera Shot": 409433},
    4: {"Saber Slash": 424785},
    5: {"Void Plague": 425204, "Vampiric Touch": 402668},
    7: {"Lava Burst": 408490, "Water Shield": 408510},
    8: {"Icy Veins": 425121, "Ice Lance": 400640},
    9: {"Wrack": 1316697, "Chaos Bolt": 403629, "Fel Armor": 403619},
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
    9: {"Shadow Bolt": 686, "Demon Armor": 706, "Demon Skin": 687},
    11: {"Wrath": 5176},
}

failures = 0


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
    cls = 0
    for line in done.stdout.splitlines():
        words = line.split("#")[0].split()
        if len(words) == 2 and words[0] == "class":
            cls = int(words[1])
            classes[cls] = []
        elif words and words[0] in ("row", "buff"):
            classes[cls].append(line.split("#")[0].strip())
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
check(order(1, 1310222, 845), "warrior: Spearing Strike before Cleave")
check(order(8, 400640, 116), "mage: Ice Lance before Frostbolt")
check(order(9, 403629, 686), "warlock: Chaos Bolt before Shadow Bolt")
check(order(5, 402668, 8092), "priest: Vampiric Touch before Mind Blast")
check(order(11, 417157, 5176), "druid: Starsurge before Wrath")

row = lambda cls, sid: next(r for r in classes[cls] if r.split()[1] == str(sid))
check("duration 30" in row(3, 3674), "Black Arrow is a 30 s damage over time")
check("duration 12" in row(4, 424785), "Saber Slash is a 12 s bleed")
check("duration 18" in row(5, 425204) and "duration 15" in row(5, 402668), "priest damage over time lengths")
check(re.search(r"\bself\b", row(2, 407778)) is not None, "Divine Storm is cast on the paladin")
check("range 0 5" in row(2, 678), "Holy Strike is a melee strike")
check(row(7, 408510).startswith("buff") and "duration 600" in row(7, 408510), "Water Shield is a 10 minute upkeep buff")
check(row(9, 403619).startswith("buff"), "Fel Armor is an upkeep buff")
# the better shield / armor retires the old one from the level it is learned
check("maxlevel" in row(7, 324), "Lightning Shield is retired by Water Shield")
check("maxlevel" in row(9, 706), "Demon Armor is retired by Fel Armor")

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
