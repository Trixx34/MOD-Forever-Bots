#!/usr/bin/python3 -I
"""Make data/avoid_spells.txt: the spells of the creatures of the Classic dungeons that leave something harmful on the ground, which a bot should step out of.

    python3 -I tools/gen_avoid_spells.py --dbc /srv/forever/data/dbc/enUS --out data/avoid_spells.txt \\
        --mysql "ssh forever-test sudo -n mysql" [--world forever_test_world] [--hotfixes forever_test_hotfixes] [--map 36 --map 34 ...] [--report]

--mysql is the command that runs a mysql client (the query goes to its standard input, -N -B is added); read only. Our own game data only: the world database, the
client's DB2 tables in --dbc, and the server-side spell tables. Nothing from any addon.

Method
  1. The creatures of a dungeon map are the distinct creature.id spawned on it. Their spells: creature_template_spell, and the casts of their SmartAI scripts
     (smart_scripts source_type 0, action 11 cast, 85 invoker cast, 86 cross cast; action 80 calls a timed action list, source_type 9, whose casts count as well).
  2. A cast spell is followed through the spells it triggers (SpellEffect.EffectTriggerSpell, three levels), because bosses cast a small spell that triggers the real one.
  3. A spell leaves something on the ground when it has an effect of one of three kinds:
       persistent_area  effect 27 (PERSISTENT_AREA_AURA): a dynamic object with the spell's aura, radius from SpellRadius. Only when the aura it applies is harmful
                        (periodic damage, damage percent, slow, stun, fear, confuse): a healing or buff area is left out.
       areatrigger      effect 164 (CREATE_AREATRIGGER): an area trigger of the world database's areatrigger_create_properties (EffectMiscValue is its id);
                        the radius is the shape's: sphere radius, box and cylinder the larger half-side. Harmful when the trigger's own spell does damage or a bad aura
                        (not decided here: every area trigger of a creature spell is listed, kind areatrigger).
       trap_object      effect 76/104..107/50 summons a gameobject of type 6 (trap): radius = Data2 yards.
  4. Effects come from three places, in this order of trust: serverside_spell_effect (world database), spell_effect (hotfixes database), the client's
     SpellEffect.db2 (its encrypted sections are skipped: see --report for how many rows that leaves).

Each line of the output (data/avoid_spells.txt, read by src/avoid_spells.h):
    <spell id>|<radius yards>|<kind>|<map id>|<creature entry>|<spell name> (<creature name>)
A spell on several maps or from several creatures has one line each.
"""
from __future__ import annotations

import argparse
import collections
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import db2_reader  # noqa: E402

DUNGEON_MAPS = [33, 34, 36, 43, 47, 48, 70, 90, 109, 129, 189, 209, 229, 230, 289, 329, 349, 389, 429]
EFFECT_PERSISTENT_AREA, EFFECT_AREATRIGGER = 27, 164
EFFECT_SUMMON_OBJECT = (50, 76, 104, 105, 106, 107)
HARMFUL_AURAS = {3: "periodic damage", 89: "periodic damage percent", 33: "slow", 12: "stun", 7: "fear", 5: "confuse"}   # (a dummy aura does what the spell script says: not known to be harmful)
TRIGGER_DEPTH = 3
CAST_ACTIONS = (11, 85, 86)

# SpellEffect.db2 record layout (the record id and SpellID are not in the records: SpellID is the relationship table's foreign id)
F_AURA, F_DIFF, F_INDEX, F_EFFECT, F_TRIGGER, F_MISC, F_RADIUS = 0, 1, 2, 3, 15, 25, 26


class Mysql:
    def __init__(self, command: str):
        self.command = command

    def query(self, sql: str) -> list[list[str]]:
        out = subprocess.run(self.command.split() + ["-N", "-B"], input=sql, capture_output=True, text=True)
        if out.returncode:
            sys.exit(f"mysql failed: {out.stderr.strip()[:300]}")
        return [line.split("\t") for line in out.stdout.splitlines() if line]


def load_effects(dbc: str, sql: Mysql, world: str, hotfixes: str):
    """{spell id: {effect index: dict(effect, aura, trigger, misc, radius_index)}} and the stats of where the rows came from."""
    stats = collections.Counter()
    effects: dict[int, dict[int, dict]] = collections.defaultdict(dict)
    table = db2_reader.read_table(os.path.join(dbc, "SpellEffect.db2"), skip_encrypted=True)
    parents = db2_reader.read_table.parents
    for rid, v in table.items():
        if v[F_DIFF] not in (0,):
            continue
        sid = parents.get(rid)
        if sid:
            effects[sid][v[F_INDEX]] = dict(effect=v[F_EFFECT], aura=v[F_AURA], trigger=db2_reader.signed(v[F_TRIGGER]), misc=db2_reader.signed(v[F_MISC][0]),
                                             radius_index=v[F_RADIUS][0], src="db2")
            stats["db2"] += 1
    for src, db, tbl in (("hotfix", hotfixes, "spell_effect"), ("serverside", world, "serverside_spell_effect")):
        rows = sql.query(f"select SpellID,EffectIndex,Effect,EffectAura,EffectTriggerSpell,EffectMiscValue1,EffectRadiusIndex1 from {db}.{tbl} where DifficultyID=0")
        for r in rows:
            sid, idx = int(r[0]), int(r[1])
            effects[sid][idx] = dict(effect=int(r[2]), aura=int(r[3]), trigger=int(r[4]), misc=int(r[5]), radius_index=int(r[6]), src=src)
            stats[src] += 1
    stats["db2 rows in encrypted sections (not readable)"] = db2_reader.read_table.skipped_records
    return effects, stats


def spell_names(dbc: str) -> dict[int, str]:
    try:
        table = db2_reader.read_table(os.path.join(dbc, "SpellName.db2"), strings=(0,), skip_encrypted=True)
    except Exception:  # noqa: BLE001  (a name is only a comment in the output)
        return {}
    return {k: v[0] for k, v in table.items() if isinstance(v[0], str)}


def creature_spells(sql: Mysql, world: str, entries: list[int]) -> dict[int, set[int]]:
    ids = ",".join(str(e) for e in entries) or "0"
    spells: dict[int, set[int]] = collections.defaultdict(set)
    for entry, spell in sql.query(f"select CreatureID,Spell from {world}.creature_template_spell where CreatureID in ({ids}) and Spell>0"):
        spells[int(entry)].add(int(spell))
    actions = ",".join(str(a) for a in CAST_ACTIONS)
    for entry, spell in sql.query(f"select entryorguid,action_param1 from {world}.smart_scripts where source_type=0 and entryorguid in ({ids}) and action_type in ({actions}) and action_param1>0"):
        spells[int(entry)].add(int(spell))
    # timed action lists called from those scripts (action 80: param1 is the list id, source_type 9 with entryorguid = list id)
    lists = sql.query(f"select entryorguid,action_param1 from {world}.smart_scripts where source_type=0 and entryorguid in ({ids}) and action_type=80")
    list_owner = {int(l): int(e) for e, l in lists}
    if list_owner:
        listids = ",".join(str(l) for l in list_owner)
        for lid, spell in sql.query(f"select entryorguid,action_param1 from {world}.smart_scripts where source_type=9 and entryorguid in ({listids}) and action_type in ({actions}) and action_param1>0"):
            spells[list_owner[int(lid)]].add(int(spell))
    return spells


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dbc", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--mysql", required=True)
    ap.add_argument("--world", default="forever_test_world")
    ap.add_argument("--hotfixes", default="forever_test_hotfixes")
    ap.add_argument("--map", type=int, action="append")
    ap.add_argument("--report", action="store_true", help="print the numbers (also written as a comment at the top of the file)")
    args = ap.parse_args()
    maps = args.map or DUNGEON_MAPS
    sql = Mysql(args.mysql)

    effects, stats = load_effects(args.dbc, sql, args.world, args.hotfixes)
    names = spell_names(args.dbc)
    radius = {k: v[0] for k, v in db2_reader.read_table(os.path.join(args.dbc, "SpellRadius.db2"), skip_encrypted=True).items()}
    radius = {k: db2_reader.as_float(v) if isinstance(v, int) else v for k, v in radius.items()}
    shapes = {}
    for pid, shape, d0, d1, d2 in sql.query(f"select Id,Shape,ShapeData0,ShapeData1,ShapeData2 from {args.world}.areatrigger_create_properties where IsCustom=0"):
        shapes[int(pid)] = (int(shape), float(d0), float(d1), float(d2))
    traps = {int(e): float(r) for e, r in sql.query(f"select entry,Data2 from {args.world}.gameobject_template where type=6")}

    def area_radius(props: int) -> float:
        shape = shapes.get(props)
        if not shape:
            return 0.0
        kind, d0, d1, d2 = shape
        return d0 if kind == 0 else max(d0, d1, d2) if kind in (1, 3) else d0    # 0 sphere, 1 box (half-sides), 3 cylinder (radius first)

    rows: list[tuple] = []
    counts = collections.Counter()
    for m in maps:
        creatures = {int(i): n for i, n in sql.query(
            f"select distinct c.id,replace(t.name,'\\t',' ') from {args.world}.creature c join {args.world}.creature_template t on t.entry=c.id where c.map={m}")}
        counts[f"map {m}: creatures"] = len(creatures)
        cspells = creature_spells(sql, args.world, sorted(creatures))
        for entry, spells in cspells.items():
            for root in sorted(spells):
                seen, frontier = {root}, {root}
                for _ in range(TRIGGER_DEPTH):
                    nxt = set()
                    for sid in frontier:
                        for e in effects.get(sid, {}).values():
                            if e["trigger"] > 0 and e["trigger"] not in seen:
                                nxt.add(e["trigger"])
                    seen |= nxt
                    frontier = nxt
                counts[f"map {m}: spells cast"] += 1
                for sid in sorted(seen):
                    for e in effects.get(sid, {}).values():
                        kind, rad = None, 0.0
                        if e["effect"] == EFFECT_PERSISTENT_AREA and e["aura"] in HARMFUL_AURAS:
                            kind, rad = "persistent_area", radius.get(e["radius_index"], 0.0)
                        elif e["effect"] == EFFECT_AREATRIGGER:
                            kind, rad = "areatrigger", area_radius(e["misc"])
                        elif e["effect"] in EFFECT_SUMMON_OBJECT and e["misc"] in traps:
                            kind, rad = "trap_object", traps[e["misc"]]
                        if kind:
                            nm = names.get(sid, "")
                            rows.append((sid, round(rad, 1), kind, m, entry, f"{nm} ({creatures[entry]})".strip()))
                            counts[f"map {m}: {kind}"] += 1
    rows = sorted(set(rows), key=lambda r: (r[3], r[4], r[0]))

    with open(args.out, "w", encoding="utf-8") as f:
        f.write("# Spells of the Classic dungeons' creatures that leave something harmful on the ground (src/avoid_spells.h). Made by tools/gen_avoid_spells.py from the\n")
        f.write("# server's world database and the client's DB2 tables; method and counts: docs/99-ground-effects.md.\n")
        f.write("# spell id|radius yards (0 = not known: the bot uses its default)|kind|map id|creature entry|spell name (creature name)\n")
        for r in rows:
            f.write("|".join(str(x) for x in r) + "\n")
    print(f"{len(rows)} lines -> {args.out}")
    if args.report:
        print(f"effect rows read: {dict(stats)}")
        for k in sorted(counts):
            print(f"  {k}: {counts[k]}")


if __name__ == "__main__":
    main()
