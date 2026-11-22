# SPDX-License-Identifier: GPL-2.0-or-later
"""Writes data/profession_recipes.txt (src/professions.h, ProfessionCraftPlan, doc/professions.md) from the client's DB2 files.

One row per recipe of the crafting lines the core's kSkillUp/kCraft take (164 Blacksmithing, 165 Leatherworking, 171 Alchemy, 186 Mining (smelting),
197 Tailoring, 202 Engineering): "skill spell minSkill trivialLow trivialHigh reagent:count,..." and the recipe name as a comment.

  - SkillLineAbility.db2: SkillLine (field 3), Spell (4), MinSkillLineRank (5), TrivialSkillLineRankHigh (9), TrivialSkillLineRankLow (10). The core takes a
    row as a recipe when MinSkillLineRank > 0 (ChooseSkillUpRecipe) and raises the skill while value < TrivialSkillLineRankHigh.
  - SpellEffect.db2: only recipes with a CREATE_ITEM effect (24) are kept, so a recipe that works on an item (a weapon chain, a scope: the core refuses
    those, WorksOnSelf) is never planned.
  - SpellReagents.db2: SpellID (field 0), Reagent[8] (1), ReagentCount[8] (2).
  - SpellName.db2: the name, for the comment.

Read only, offline. Hotfix rows of the server database are not applied (the server may differ for a few recipes; the core checks every kCraft anyway).
Usage: python3 tools/gen_profession_recipes.py --dbc /srv/forever/data/dbc/enUS > data/profession_recipes.txt
"""
from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import db2_reader  # noqa: E402

SKILLS = (164, 165, 171, 186, 197, 202)
EFFECT_CREATE_ITEM = 24
# Classic skills end at 300: a recipe that turns yellow above that is left out (the client data holds the later expansions' recipes too).
MAX_TRIVIAL_LOW = 300


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dbc", required=True)
    args = ap.parse_args()

    ability = db2_reader.read_table(os.path.join(args.dbc, "SkillLineAbility.db2"), strings=(0, 1), skip_encrypted=True)
    effects = db2_reader.read_table(os.path.join(args.dbc, "SpellEffect.db2"), skip_encrypted=True)
    effect_parents = dict(db2_reader.read_table.parents)
    creates = {effect_parents.get(rid) for rid, v in effects.items() if v[3] == EFFECT_CREATE_ITEM}
    reagents_rows = db2_reader.read_table(os.path.join(args.dbc, "SpellReagents.db2"), skip_encrypted=True)
    reagents = {}
    for v in reagents_rows.values():
        pairs = [(item, count) for item, count in zip(v[1], v[2]) if item and count]
        if pairs:
            reagents.setdefault(v[0], pairs)
    names = {k: v[0] for k, v in db2_reader.read_table(os.path.join(args.dbc, "SpellName.db2"), strings=(0,), skip_encrypted=True).items()}

    rows = []
    for v in ability.values():
        skill, spell, min_rank, high, low = v[3], v[4], db2_reader.signed(v[5], 16), db2_reader.signed(v[9], 16), db2_reader.signed(v[10], 16)
        if skill not in SKILLS or spell == 0 or not (0 < min_rank < 1000) or not (0 < high < 1000) or low > MAX_TRIVIAL_LOW:
            continue
        if spell not in creates or spell not in reagents:
            continue
        rows.append((skill, low, spell, min_rank, high))
    rows.sort()
    seen = set()
    print("# Recipes of the crafting lines (src/professions.h, ProfessionCraftPlan). Made by tools/gen_profession_recipes.py from the client's DB2 files; do not edit.")
    print("# skill spell minSkill trivialLow trivialHigh reagent:count,...  (164 Blacksmithing, 165 Leatherworking, 171 Alchemy, 186 Mining/smelting, 197 Tailoring,")
    print("# 202 Engineering). Only recipes that create an item and turn yellow at 300 or below; sorted by skill, then trivialLow. In this client data minSkill is 1")
    print("# for nearly every recipe (the trainer's skill requirement gates learning), so the plan orders by trivialLow. Server hotfixes are not applied.")
    for skill, low, spell, min_rank, high in rows:
        if (skill, spell) in seen:
            continue
        seen.add((skill, spell))
        parts = ",".join(f"{item}:{count}" for item, count in reagents[spell])
        print(f"{skill} {spell} {min_rank} {low} {high} {parts}  # {names.get(spell, '')}")


main()
