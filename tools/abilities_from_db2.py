#!/usr/bin/python3 -I
# SPDX-License-Identifier: GPL-2.0-or-later
"""Writes BotAbilities lines (the core's Bot.Dump.Abilities format) from the client's DB2 files and a trainer export, for tools/make_rotations.py.

Why: the dump needs a sim restart with Bot.Dump.Abilities = 1 and a bot of every class. This reads what the game's own files say instead:
  - SkillLineAbility.db2: which spells belong to a class (the ability's class mask, or the skill line's when the ability has none: a skill line whose
    abilities all carry the same single class bit belongs to that class);
  - SpellName.db2: the names;
  - the level a spell is learned at: the lowest ReqLevel of any trainer that teaches it (--trainer, one line per spell: id, lowest level, trainers; made
    with `select SpellId, min(ReqLevel), count(*) from trainer_spell group by SpellId` on the world database), else SpellLevels.db2's spell level.
A spell no trainer teaches (the core teaches bots trainer spells only) is still listed at its spell level, and --report names them: a bot will not know
them until a trainer row exists, and a rotation row for one is refused ("spell not known") and retried a minute later.

    python3 -I tools/abilities_from_db2.py --dbc /srv/forever/data/dbc/enUS --trainer trainer.tsv > abilities.log
    python3 -I tools/make_rotations.py --dump abilities.log --spec data/rotation_spec.txt > data/rotations.txt
"""
from __future__ import annotations

import argparse
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import db2_reader  # noqa: E402

CLASSES = (1, 2, 3, 4, 5, 7, 8, 9, 11)   # the class ids of the core; the class mask bit is 1 << (class id - 1)
MAX_LEVEL = 60


def read_trainer(path):
    levels = {}
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            parts = line.split()
            if len(parts) >= 2 and parts[0].isdigit():
                levels[int(parts[0])] = int(parts[1])
    return levels


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dbc", required=True)
    parser.add_argument("--trainer", required=True)
    parser.add_argument("--report", action="store_true", help="list the class spells no trainer teaches, on standard error")
    args = parser.parse_args()

    names = db2_reader.read_table(os.path.join(args.dbc, "SpellName.db2"), strings=(0,), skip_encrypted=True)
    ability = db2_reader.read_table(os.path.join(args.dbc, "SkillLineAbility.db2"), skip_encrypted=True)
    spell_levels = db2_reader.read_table(os.path.join(args.dbc, "SpellLevels.db2"), skip_encrypted=True)
    parents = db2_reader.read_table.parents
    level_of = {}
    for record, spell in parents.items():
        if record in spell_levels and spell_levels[record][4]:
            level_of[spell] = spell_levels[record][4]
    trainer = read_trainer(args.trainer)

    # SkillLineAbility row: [RaceMask, RaceMask, ID, SkillLine, Spell, MinRank, ClassMask, Supersedes, AcquireMethod, ...]
    masks = collections.defaultdict(collections.Counter)
    for row in ability.values():
        if row[6]:
            masks[row[3]][row[6]] += 1
    line_class = {line: c.most_common(1)[0][0] for line, c in masks.items() if len(c) == 1}

    by_class = {cls: {} for cls in CLASSES}     # class -> spell id -> learn level
    for row in ability.values():
        mask = row[6] or line_class.get(row[3], 0)
        spell = row[4]
        if not mask or mask & (mask - 1) or spell not in names or not names[spell] or not names[spell][0]:
            continue
        cls = next((c for c in CLASSES if mask == 1 << (c - 1)), 0)
        if cls == 0:
            continue
        learn = trainer.get(spell) or level_of.get(spell)
        by_class[cls][spell] = max(1, min(learn, MAX_LEVEL)) if learn else 0   # 0: no trainer and no spell level (a trigger spell, or one the game gives at once)

    # What a bot can learn: for a name that some trainer teaches, only the ids a trainer teaches (Execute's damage spell 20647 has a client spell level 1, but only
    # 5308 is taught, at 24; a rune or a trigger spell of the same name is not a rank). A name no trainer teaches keeps its spell-level ids (Summon Hawk, Auto Shot),
    # and a spell with no level at all is kept at level 1 only when no spell of its name has one.
    for cls in CLASSES:
        names_taught = {names[s][0] for s in by_class[cls] if s in trainer}
        names_levelled = {names[s][0] for s, learn in by_class[cls].items() if learn}
        by_class[cls] = {s: (learn or 1) for s, learn in by_class[cls].items()
                         if (s in trainer if names[s][0] in names_taught else (learn or names[s][0] not in names_levelled))}

    if args.report:
        for cls in CLASSES:
            untaught = sorted(s for s in by_class[cls] if s not in trainer and s in level_of)
            print(f"class {cls}: {len(by_class[cls])} spells, {len(untaught)} that no trainer teaches", file=sys.stderr)

    for cls in CLASSES:
        known = 0
        for level in range(1, MAX_LEVEL + 1):
            new = sorted(s for s, learn in by_class[cls].items() if learn == level)
            known += len(new)
            text = "; ".join(f"{s} {names[s][0]}" for s in new)
            print(f"BotAbilities: race 1 class {cls} level {level}: {len(new)} new spell(s), {known} in total: {text}")


if __name__ == "__main__":
    main()
