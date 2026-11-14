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
    1: {},   # Shockwave, Spearing Strike, Reckless Swings: no trainer teaches them, so no rows
    2: {"Holy Strike": 678, "Hammer of the Righteous": 407632},   # Rebuke 425609: a rune (AcquireMethod 3), nothing teaches it, so no row
    4: {},   # Saber Slash: no trainer teaches it, so no row
    5: {},   # Void Plague 425204, Vampiric Touch 402668: runes, nothing teaches them, so no rows
    7: {"Water Shield": 408510},   # Lava Burst is gone from the 70790 client
    8: {"Ice Lance": 400640},   # Icy Veins 425121: a rune, nothing teaches it, so no row
    9: {},  # no Forever row: Fel Armor is left out (no trainer teaches it)
    11: {},  # Starsurge and Sunfire: Season of Discovery runes (AcquireMethod 3), nothing teaches them, so no rows
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
block_order = lambda cls, spec, a, b: ids(blocks[(cls, spec)]).index(a) < ids(blocks[(cls, spec)]).index(b)
check(order(8, 400640, 116), "mage: Ice Lance before Frostbolt")
# Season of Discovery runes (SkillLineAbility AcquireMethod 3, no trainer, quest, item, learn spell or talent): no rows even when the dump has them
RUNES = {2: {"Rebuke": 425609}, 5: {"Void Plague": 425204, "Vampiric Touch": 402668}, 8: {"Icy Veins": 425121}}
runes_dump = "\n".join(dump_line(c, {**FOREVER[c], **VANILLA[c], **RUNES.get(c, {})}) for c in FOREVER) + "\n"
rune_classes, _ = run(runes_dump)
check(not any(int(r.split()[1]) in (425609, 425204, 402668, 425121) for c in rune_classes for r in rune_classes[c]), "no Rebuke, Void Plague, Vampiric Touch or Icy Veins row")
classes, err = run(full)

row = lambda cls, sid: next(r for r in classes[cls] if r.split()[1] == str(sid))
check(not any(r.split()[1] in ("424785", "1310222", "440488", "1329555", "1328548") for c in classes for r in classes[c]), "spells no trainer teaches are no rows")
check("range 0 5" in row(2, 678), "Holy Strike is a melee strike")
check(not any(r.split()[1] in ("417157", "414684") for r in classes.get(11, [])), "no Starsurge or Sunfire row")
# Water Shield is a talent (trait node 104732) that only the resto talent profile takes: only the resto block has it
check(any(r.split()[1] == "408510" and r.startswith("buff") and "duration 600" in r for r in blocks[(7, "resto")]), "resto: Water Shield is a 10 minute upkeep buff")
check(not any(r.split()[1] == "408510" for spec in ("", "enh", "ele") for r in blocks[(7, spec)]), "no Water Shield outside the resto block")
# Fel Armor (403619) is left out: no trainer teaches it, so a bot never knows it, and a supersede to it would stop Demon Armor for good
check(not any(r.split()[1] == "403619" for r in classes[9]), "Fel Armor is not a warlock row")
# the better shield retires the old one from the level it is learned
block_row = lambda cls, spec, sid: next(r for r in blocks[(cls, spec)] if r.split()[1] == str(sid))
check("maxlevel" in block_row(7, "resto", 324), "resto: Lightning Shield is retired by Water Shield")
# a supersede caps the old spell in its own block only (it used to cap Lightning Shield in every shaman block before the resto one)
check(all("maxlevel" not in block_row(7, spec, 324) for spec in ("", "enh", "ele")), "Lightning Shield stays in the blocks with no Water Shield")
check(all(r.count("maxlevel") <= 1 for c in classes for r in classes[c]), "no row is capped twice")
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

# 3. the minimum range from a range file (as --dbc reads it from SpellMisc and SpellRange): a row below it is raised, a row with no range gets one,
# a self row and a row already at the minimum are left alone, and each change is a warning.
sys.path.insert(0, os.path.join(ROOT, "tools"))
import make_rotations  # noqa: E402
ranges = {2643: (8.0, 35.0), 20900: (8.0, 35.0), 585: (0.0, 30.0)}
warnings = []
check(make_rotations.apply_min_range(" level 18 enemies 2 target range 0 35", [2643], ranges, "a", warnings) == " level 18 enemies 2 target range 8 35", "range 0 raised to 8")
check(make_rotations.apply_min_range(" level 20 always target range 0 35", [19434, 20900], ranges, "b", warnings) == " level 20 always target range 8 35", "any rank's minimum counts")
check(make_rotations.apply_min_range(" level 18 always target", [2643], ranges, "c", warnings) == " level 18 always target range 8 35", "a row with no range gets one")
check(make_rotations.apply_min_range(" level 18 always target range 8 35", [2643], ranges, "d", warnings) == " level 18 always target range 8 35", "already right")
check(make_rotations.apply_min_range(" level 1 always target range 0 30", [585], ranges, "e", warnings) == " level 1 always target range 0 30", "no minimum range")
check(make_rotations.apply_min_range(" level 1 self_hp_below 40 self", [2643], ranges, "f", warnings) == " level 1 self_hp_below 40 self", "a self row is left alone")
check(len(warnings) == 3, "a warning for each change")
with tempfile.TemporaryDirectory() as tmp:
    log, spec, rng = (os.path.join(tmp, n) for n in ("Server.log", "spec.txt", "ranges.txt"))
    with open(log, "w", encoding="utf-8") as handle:
        handle.write(dump_line(3, {"Multi-Shot": 2643}) + "\n")
    with open(spec, "w", encoding="utf-8") as handle:
        handle.write('class 3\nrow "Multi-Shot" enemies 2 target range 0 35\n')
    with open(rng, "w", encoding="utf-8") as handle:
        handle.write("2643 8 35\n")
    done = subprocess.run([sys.executable, "-I", TOOL, "--dump", log, "--spec", spec, "--ranges", rng], capture_output=True, text=True, check=True)
    check("target range 8 35" in done.stdout and "raised to the minimum range 8" in done.stderr, "--ranges raises the row")

if failures:
    print(f"{failures} failure(s)")
    sys.exit(1)
print("make_rotations_test: all passed")
