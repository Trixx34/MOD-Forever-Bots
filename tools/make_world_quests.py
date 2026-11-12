#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Builds a whole-world quest file (src/quest_data.h format, with 'req' records) from exports of the world database. See doc/world-quests.md.

Inputs (tab-separated, one record per line, a header line is skipped; see the doc for the queries that make them):
  quests.tsv      id  questLevel  minLevel  allowableRaces  allowableClasses  prevQuestId  [suggestedGroup]
  objectives.tsv  quest  order  type  objectId  amount          type 0 = kill a creature, 1 = bring an item; other types make the quest unsupported
  npcs.tsv        quest  role  creature  map  x  y  z           role start or end (every spawn of the giver or ender; the first on the quest's map is used)
  targets.tsv     quest  creature  map  x  y  z                 spawn points of the creatures a quest asks for (the hunting areas)
  drops.tsv       item  creature                               which creatures drop a quest item (for 'bring an item' objectives)
  npc_factions.tsv  creature  faction                    optional: creature_template.faction of every quest giver and ender creature (tools/export_giver_factions.sql, export_world_quests.py);
                                                              with --side a giver or ender spawn that belongs to the other side is not used, and a quest left with no usable giver or
                                                              ender spawn is skipped (the bot could not talk to a hostile NPC)
  givers.tsv      quest  faction                              optional: the creature_template.faction of each creature that gives the quest (tools/export_giver_factions.sql);
                                                              with --side a quest whose givers all belong to the other side (tools/faction_sides.tsv) is left out

Quests are grouped into starts: the givers are cut into square cells (--cell yards, default 500) per map, and each cell with quests becomes a start
whose anchor is the average position of its givers. Inside a start the quests are ordered by quest level, then by the order of their chains.
With --side and givers.tsv, quests whose creature givers all belong to the other side are skipped too (the race masks are 0 on many quests).
Skipped: quests with a type other than kill or item (exploration, talk, use object, escort ...), quests for a group (suggestedGroup > 1; kept with --groups), quests on
maps outside --maps, quests with no giver or ender on one map, and quests whose objectives name no creature with a known spawn.
"""
import argparse
import math
import os
import sys
from collections import defaultdict


def rows(path):
    try:
        handle = open(path, encoding="utf-8")
    except OSError as error:
        sys.exit(f"cannot read {path}: {error}")
    with handle:
        for number, line in enumerate(handle, 1):
            parts = line.rstrip("\n").split("\t")
            if number == 1 and not parts[0].lstrip("-").isdigit():
                continue
            yield number, parts


def num(text, default=0):
    # Exact integers first: a mask such as AllowableRaces = -1 stored as unsigned 64 bits (18446744073709551615) must not go through float.
    try:
        return int(text)
    except ValueError:
        pass
    try:
        return int(float(text))
    except ValueError:
        return default


# Standard race ids as mask bits (bit id-1): Human 1, Dwarf 3, Night Elf 4, Gnome 7 are Alliance; Orc 2, Undead 5, Tauren 6, Troll 8 are Horde. A mask that also
# names a race above 8 (a custom race) is kept for both sides.
SIDE_RACES = {"alliance": 0b01001101, "horde": 0b10110010}


def load_faction_sides():
    # tools/faction_sides.tsv: creature faction id -> alliance / horde; an id that is not listed is neutral.
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "faction_sides.tsv")
    sides = {}
    for _, p in rows(path):
        if len(p) >= 2 and p[1] in ("alliance", "horde"):
            sides[num(p[0])] = p[1]
    return sides


def mask_or_any(value):
    # AllowableRaces / AllowableClasses: 0 or negative mean any; so does a value with every bit set (-1 stored unsigned). A wider mask is cut to 32 bits
    # (the module has race and class ids up to 32); a mask with no bit in those 32 means any.
    if value <= 0 or value >= (1 << 63):
        return 0
    return (value & 0xFFFFFFFF) or 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", help="folder with quests.tsv, objectives.tsv, npcs.tsv, targets.tsv, drops.tsv")
    parser.add_argument("--cell", type=float, default=500.0)
    parser.add_argument("--maps", default="0,1")
    parser.add_argument("--areas", type=int, default=3, help="hunting areas per quest")
    parser.add_argument("--min-level", type=int, default=1)
    parser.add_argument("--max-level", type=int, default=60)
    parser.add_argument("--side", choices=["alliance", "horde"], help="keep only quests the side's races may take (a quest open to every race stays in both lists)")
    parser.add_argument("--groups", action="store_true", help="keep quests for a group (suggestedGroup > 1) and mark them 'group <n>' in their req record; the planner takes them "
                        "only with the config key QuestGroups on (use it for the level 60 lists)")
    parser.add_argument("--explain", help="write one line per skipped quest (quest, level, reason, detail) to this file, and print a breakdown of the reasons")
    args = parser.parse_args()
    maps = {int(m) for m in args.maps.split(",") if m}
    d = args.directory.rstrip("/\\")

    quests = {}
    for _, p in rows(f"{d}/quests.tsv"):
        if len(p) < 6:
            continue
        group = num(p[6]) if len(p) > 6 else 0
        quests[num(p[0])] = dict(level=num(p[1]), minlevel=num(p[2]), races=mask_or_any(num(p[3])), classes=mask_or_any(num(p[4])),
                                 prev=max(0, num(p[5])), group=group)

    objectives = defaultdict(list)
    for _, p in rows(f"{d}/objectives.tsv"):
        if len(p) >= 5:
            objectives[num(p[0])].append((num(p[1]), num(p[2]), num(p[3]), num(p[4])))

    # Creature -> side from its faction template, for the giver and ender spawns (no npc_factions.tsv or no --side: every spawn is usable).
    npc_side = {}
    other = {"alliance": "horde", "horde": "alliance"}.get(args.side)
    if other and os.path.exists(f"{d}/npc_factions.tsv"):
        faction_sides_npc = load_faction_sides()
        for _, p in rows(f"{d}/npc_factions.tsv"):
            if len(p) >= 2:
                npc_side[num(p[0])] = faction_sides_npc.get(num(p[1]), "neutral")

    npcs = defaultdict(lambda: {"start": [], "end": []})
    hostile_npcs = defaultdict(lambda: {"start": 0, "end": 0})   # spawns left out because the creature belongs to the other side
    for _, p in rows(f"{d}/npcs.tsv"):
        if len(p) >= 7 and p[1] in ("start", "end"):
            if other and npc_side.get(num(p[2])) == other:
                hostile_npcs[num(p[0])][p[1]] += 1
                continue
            npcs[num(p[0])][p[1]].append((num(p[3]), float(p[4]), float(p[5]), float(p[6])))

    targets = defaultdict(lambda: defaultdict(list))
    for _, p in rows(f"{d}/targets.tsv"):
        if len(p) >= 6:
            targets[num(p[0])][num(p[1])].append((num(p[2]), float(p[3]), float(p[4]), float(p[5])))

    drops = defaultdict(set)
    for _, p in rows(f"{d}/drops.tsv"):
        if len(p) >= 2:
            drops[num(p[0])].add(num(p[1]))

    # Giver sides: a quest is kept out of a side's list when all its creature givers belong to the other side. A neutral giver (a faction that is in
    # neither side's list, or one the table does not know) or a giver of each side keeps it in both lists. No givers.tsv means no filter.
    giver_sides = defaultdict(set)
    if args.side and os.path.exists(f"{d}/givers.tsv"):
        faction_sides = load_faction_sides()
        for _, p in rows(f"{d}/givers.tsv"):
            if len(p) >= 2:
                giver_sides[num(p[0])].add(faction_sides.get(num(p[1]), "neutral"))

    built = {}
    skipped = defaultdict(int)
    explained = []   # (quest, level, reason, detail)

    def skip(qid, reason, detail=""):
        skipped[reason] += 1
        explained.append((qid, quests[qid]["level"], reason, detail))
    for qid, info in quests.items():
        # A class quest (restricted to some classes) is completed with its reward, not by its objectives (the planner's reward step), so its
        # objectives do not decide whether it is kept, only its giver and ender do.
        reward = info["classes"] != 0
        if info["group"] > 1 and not reward and not args.groups:
            skip(qid, "group", f"suggestedGroup={info['group']}")
            continue
        if not (args.min_level <= info["level"] <= args.max_level):
            skip(qid, "level")
            continue
        if args.side and info["races"] and not (info["races"] & SIDE_RACES[args.side]) and info["races"] >> 8 == 0:
            skip(qid, "other faction", f"races mask {info['races']}")
            continue
        if args.side and giver_sides[qid] == {"horde" if args.side == "alliance" else "alliance"}:
            skip(qid, "giver of the other side", "givers: " + ",".join(sorted(giver_sides[qid])))
            continue
        giver = next(((m, x, y, z) for m, x, y, z in npcs[qid]["start"] if m in maps), None)
        if not giver and hostile_npcs[qid]["start"] and not npcs[qid]["start"]:
            skip(qid, "giver hostile to the side", f"{hostile_npcs[qid]['start']} giver spawn(s), all of the {other} side")
            continue
        if not giver:
            starts_elsewhere = [m for m, *_ in npcs[qid]["start"]]
            skip(qid, "no giver", "no creature giver spawn" if not starts_elsewhere else f"giver only on maps {sorted(set(starts_elsewhere))}")
            continue
        ender = next(((m, x, y, z) for m, x, y, z in npcs[qid]["end"] if m == giver[0]), None)
        if not ender and hostile_npcs[qid]["end"] and not [e for e in npcs[qid]["end"]]:
            skip(qid, "ender hostile to the side", f"{hostile_npcs[qid]['end']} ender spawn(s), all of the {other} side")
            continue
        if not ender:
            skip(qid, "no ender on the giver's map", f"enders on maps {sorted({m for m, *_ in npcs[qid]['end']})}")
            continue
        if reward:
            built[qid] = dict(info=info, giver=giver, ender=ender, hunts=[], areas=[], reward=True)
            continue
        objs = sorted(objectives.get(qid, []))
        if any(kind not in (0, 1) for _, kind, _, _ in objs):
            skip(qid, "unsupported objective", "types " + ",".join(sorted({str(kind) for _, kind, _, _ in objs if kind not in (0, 1)})))
            continue
        hunts = []
        areas = []
        usable = True
        for position, (_order, kind, ident, amount) in enumerate(objs):
            if position >= 4:
                break
            creatures = {ident} if kind == 0 else set(drops.get(ident, ()))
            spawns = [(x, y, z) for c in creatures for (m, x, y, z) in targets[qid].get(c, []) if m == giver[0]]
            if not creatures or not spawns:
                usable = False
                if kind == 1 and not creatures:
                    why = f"item {ident}: no creature drops it (gameobject, vendor, reference or other loot)"
                elif not spawns:
                    why = f"{'creature' if kind == 0 else 'item ' + str(ident)}: {len(creatures)} creature(s), none spawned on map {giver[0]}"
                else:
                    why = "unknown"
                break
            hunts.append((position, amount, sorted(creatures)))
            areas.extend(spawns)
        if not usable:
            skip(qid, "no known creature spawns", why)
            continue
        # Hunting areas: spawns spread out, nearest to the giver first.
        areas.sort(key=lambda a: (a[0] - giver[1]) ** 2 + (a[1] - giver[2]) ** 2)
        picked = []
        for a in areas:
            if all(math.hypot(a[0] - b[0], a[1] - b[1]) >= 60 for b in picked):
                picked.append(a)
            if len(picked) >= args.areas:
                break
        built[qid] = dict(info=info, giver=giver, ender=ender, hunts=hunts, areas=picked, reward=False)

    # Starts: cells of givers per map.
    cells = defaultdict(list)
    for qid, q in built.items():
        m, x, y, z = q["giver"]
        cells[(m, math.floor(x / args.cell), math.floor(y / args.cell))].append(qid)

    def chain_depth(qid, seen=()):
        prev = built[qid]["info"]["prev"]
        return 0 if prev == 0 or prev not in built or prev in seen else 1 + chain_depth(prev, seen + (qid,))

    out = ["# Whole-world quests. Made by tools/make_world_quests.py from world database exports (doc/world-quests.md). Format: src/quest_data.h"]
    for (m, cx, cy), ids in sorted(cells.items()):
        gx = sum(built[i]["giver"][1] for i in ids) / len(ids)
        gy = sum(built[i]["giver"][2] for i in ids) / len(ids)
        gz = sum(built[i]["giver"][3] for i in ids) / len(ids)
        levels = [built[i]["info"]["level"] for i in ids]
        out.append("")
        out.append(f"start W{m}_{cx}_{cy}   # {len(ids)} quests, levels {min(levels)}-{max(levels)}")
        out.append(f"anchor {m} {gx:.1f} {gy:.1f} {gz:.1f}")
        for qid in sorted(ids, key=lambda i: (built[i]["info"]["level"], chain_depth(i), i)):
            q = built[qid]
            _, gx2, gy2, gz2 = q["giver"]
            _, ex, ey, ez = q["ender"]
            out.append(f"quest {qid} giver {gx2:.1f} {gy2:.1f} {gz2:.1f} ender {ex:.1f} {ey:.1f} {ez:.1f}")
            info = q["info"]
            req = f"req {qid} level {info['level']}"
            if info["minlevel"]:
                req += f" minlevel {info['minlevel']}"
            if info["prev"]:
                req += f" prev {info['prev']}"
            if info["races"]:
                req += f" races {info['races']}"
            if info["classes"]:
                req += f" classes {info['classes']}"
            if info["group"] > 1 and args.groups and not q["reward"]:
                req += f" group {info['group']}"
            out.append(req)
            if q["reward"]:
                out.append(f"reward {qid}")
            for position, amount, creatures in q["hunts"]:
                out.append(f"hunt {qid} {position} {amount} {','.join(str(c) for c in creatures)}")
            for ax, ay, az in q["areas"]:
                out.append(f"area {qid} {ax:.1f} {ay:.1f} {az:.1f}")
    if args.explain:
        with open(args.explain, "w", encoding="utf-8") as handle:
            handle.write("quest\tlevel\treason\tdetail\n")
            for row in sorted(explained):
                handle.write("\t".join(str(v) for v in row) + "\n")
        breakdown = defaultdict(int)
        for _q, _l, reason, detail in explained:
            key = reason if reason != "no known creature spawns" else reason + ": " + ("item without a creature dropper" if "no creature drops it" in detail else "no spawn on the giver's map")
            breakdown[key] += 1
        for key, n in sorted(breakdown.items(), key=lambda kv: -kv[1]):
            print(f"  {n:5d}  {key}", file=sys.stderr)
    print("\n".join(out))
    print(f"{len(built)} quests in {len(cells)} starts; skipped: " + ", ".join(f"{n} {why}" for why, n in sorted(skipped.items())), file=sys.stderr)


if __name__ == "__main__":
    main()
