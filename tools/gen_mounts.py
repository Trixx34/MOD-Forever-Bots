#!/usr/bin/env python3
"""Generate data/mount_vendors.txt, data/riding_trainers.txt and data/riding_ranks.txt for the mount errands (doc/mounts.md, src/mount_plan.h).

Everything comes from the world and hotfix databases, nothing is picked by item entry:
  riding ranks     trainer_spell rows whose spell asks for the riding skill line (762) at the creature_trainer's trainer: the spell, level, copper. The skill the rank gives
                   (75, 150, ...) is the rank's position in the level order, as the Classic ranks go (Apprentice 75, Journeyman 150).
  riding trainers  creatures of those trainers that stand on a Classic continent (map 0 or 1), and the creatures with the subname "Riding Trainer" (the Undercity
                   one has its own trainer id whose spells are not on the riding skill line: see doc/mounts.md). The side comes from the nearest mount vendor (its race), else from
                   tools/faction_sides.tsv.
  mount vendors    creatures in npc_vendor that sell mounts (item class 15, subclass 5) for plain copper (no extended cost, no reputation gate) on a Classic continent. The
                   race a vendor serves comes from its subname ("Ram Breeder" -> Dwarf, ...), because the item's race mask is faction wide. A vendor whose subname names no
                   race is left out and reported.
Reads only (every statement is a SELECT). Python 3 standard library only.

    tools/gen_mounts.py --mysql "ssh forever-test sudo -n mysql" --out-dir data
    tools/gen_mounts.py --offline-json tests/data/mounts-fixture.json --out-dir /some/dir     (no database; the fixture is the query results)
"""
import argparse
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

RIDING_SKILL_LINE = 762
CLASSIC_MAPS = (0, 1)
# vendor subname keyword -> race bit (1 Human, 2 Orc, 4 Dwarf, 8 Night Elf, 16 Undead, 32 Tauren, 64 Gnome, 128 Troll); first match wins, so "Undead Horse" comes before "Horse".
SUBNAME_RACE = [("undead", 16), ("ram", 4), ("mechanostrider", 64), ("saber", 8), ("kennel", 2), ("wolf", 2), ("kodo", 32), ("raptor", 128), ("horse", 1)]
ALLIANCE_BITS = 1 | 4 | 8 | 64
RANK_NAMES = {75: "Apprentice Riding", 150: "Journeyman Riding", 225: "Expert Riding", 300: "Artisan Riding"}
# Capitals, to name a trip in the journal and spread bots: name, map, x, y.
CAPITALS = [("Stormwind", 0, -8913.0, 554.0), ("Ironforge", 0, -4981.0, -881.0), ("Darnassus", 1, 9947.0, 2482.0),
            ("Undercity", 0, 1633.0, 240.0), ("Orgrimmar", 1, 1629.0, -4373.0), ("ThunderBluff", 1, -1277.0, 124.0)]
# Only for the trainer's side when no vendor of a race stands within 150 yards.
HORDE_CITIES = {"Undercity", "Orgrimmar", "ThunderBluff"}

VENDOR_SQL = (
    "SELECT v.entry AS entry, t.name AS name, t.subname AS subname, c.guid AS guid, c.map AS map, c.position_x AS x, c.position_y AS y, c.position_z AS z, "
    "a.id AS item, a.name AS item_name, a.requiredLevel AS level, s.BuyPrice AS copper "
    "FROM {w}.npc_vendor v JOIN {w}.armory_item a ON a.id = v.item AND a.class = 15 AND a.subclass = 5 "
    "JOIN {h}.item_sparse s ON s.ID = a.id AND s.MinReputation = 0 JOIN {w}.creature_template t ON t.entry = v.entry "
    "JOIN {w}.creature c ON c.id = v.entry AND c.map IN (0, 1) WHERE v.ExtendedCost = 0 AND v.PlayerConditionID = 0 ORDER BY v.entry, a.id")
TRAINER_SQL = (
    "SELECT t.entry AS entry, t.name AS name, t.subname AS subname, t.faction AS faction, c.guid AS guid, c.map AS map, c.position_x AS x, c.position_y AS y, c.position_z AS z "
    "FROM {w}.creature_trainer ct JOIN {w}.creature_template t ON t.entry = ct.CreatureID JOIN {w}.creature c ON c.id = t.entry AND c.map IN (0, 1) "
    "WHERE ct.TrainerID IN (SELECT TrainerId FROM {w}.trainer_spell WHERE ReqSkillLine = " + str(RIDING_SKILL_LINE) + ") "
    "OR (t.subname LIKE '%Riding Trainer%' AND t.npcflag & 16 <> 0) ORDER BY t.entry")
RANK_SQL = (
    "SELECT SpellId AS spell, ReqLevel AS level, MIN(MoneyCost) AS copper, COUNT(DISTINCT TrainerId) AS trainers FROM {w}.trainer_spell "
    "WHERE ReqSkillLine = " + str(RIDING_SKILL_LINE) + " OR SpellId IN (SELECT ReqAbility1 FROM {w}.trainer_spell WHERE ReqSkillLine = " + str(RIDING_SKILL_LINE) + ") "
    "GROUP BY SpellId, ReqLevel ORDER BY ReqLevel, SpellId")
SPELL_NAME_SQL = "SELECT ID AS id, Name AS name FROM {h}.spell_name WHERE ID IN ({ids})"


def fetch(args):
    import gen_prebis as g
    db = g.Db(args.mysql)
    fmt = {"w": args.world_db, "h": args.hotfix_db}
    data = {"vendors": db.rows(VENDOR_SQL.format(**fmt)), "trainers": db.rows(TRAINER_SQL.format(**fmt)), "ranks": db.rows(RANK_SQL.format(**fmt)), "names": []}
    if data["ranks"]:
        ids = ",".join(str(int(r["spell"])) for r in data["ranks"])
        data["names"] = db.rows(SPELL_NAME_SQL.format(ids=ids, **fmt))
    return data


def race_of(subname):
    low = (subname or "").lower()
    for key, bit in SUBNAME_RACE:
        if key in low:
            return bit
    return 0


def clean(text):
    return (text or "").replace("|", " ").replace("\n", " ").strip()


def pos(r):
    return "%.1f %.1f %.1f" % (float(r["x"]), float(r["y"]), float(r["z"]))


def load_sides():
    sides = {}
    try:
        with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "faction_sides.tsv"), encoding="utf-8") as fh:
            for line in fh:
                if line.startswith("#") or not line.strip() or line.startswith("faction"):
                    continue
                f, s = line.split()
                sides[int(f)] = 1 if s == "alliance" else 2
    except OSError:
        pass
    return sides


def build(data, sides=None):
    """Returns (vendors_text, trainers_text, ranks_text, report)."""
    sides = load_sides() if sides is None else sides
    report = {"vendors_skipped": [], "vendors": 0, "items": 0, "trainers": 0, "ranks": 0}
    by_vendor = {}
    for r in data["vendors"]:
        by_vendor.setdefault(int(r["entry"]), []).append(r)
    vlines, ilines, vend_pos = [], [], []
    for entry in sorted(by_vendor):
        rows = by_vendor[entry]
        first = rows[0]
        race = race_of(first["subname"])
        if not race:
            report["vendors_skipped"].append("%s (%s)" % (clean(first["name"]), clean(first["subname"])))
            continue
        items = {}
        for r in rows:
            level = int(r["level"])
            if 0 < level <= 60 and int(r["copper"]) > 0:
                items[int(r["item"])] = (level, int(r["copper"]), clean(r["item_name"]))
        if not items:
            continue
        vlines.append("vendor|%d|%d|%d|%s|%s|%s|%s\n" % (race, int(first["map"]), entry, first["guid"], pos(first), clean(first["name"]), clean(first["subname"])))
        vend_pos.append((int(first["map"]), float(first["x"]), float(first["y"]), race))
        report["vendors"] += 1
        for item in sorted(items):
            level, copper, name = items[item]
            ilines.append("item|%d|%d|%d|%d|%d|%s\n" % (entry, item, level, 150 if level >= 60 else 75, copper, name))
            report["items"] += 1
    names = {int(r["id"]): clean(r["name"]) for r in data.get("names", []) if r.get("name")}
    ranks = []
    seen_levels = []
    for r in sorted(data["ranks"], key=lambda r: (int(r["level"]), int(r["spell"]))):
        # one rank per spell: the riding trainers' spells (the other rows are earlier requirements, listed first with their own level)
        ranks.append(r)
        seen_levels.append(int(r["level"]))
    rank_lines = []
    for i, r in enumerate(ranks):
        skill = 75 * (i + 1)
        rank_lines.append("%d|%d|%d|%d|%s\n" % (int(r["spell"]), int(r["level"]), skill, int(r["copper"]), names.get(int(r["spell"]), RANK_NAMES.get(skill, "Riding rank %d" % (i + 1)))))
    report["ranks"] = len(rank_lines)
    tlines = []
    for r in data["trainers"]:
        m, x, y = int(r["map"]), float(r["x"]), float(r["y"])
        near = sorted((math.hypot(vx - x, vy - y), race) for vm, vx, vy, race in vend_pos if vm == m)
        if near and near[0][0] <= 150.0:
            side = 1 if near[0][1] & ALLIANCE_BITS else 2
        else:
            side = sides.get(int(r.get("faction") or 0), 0)
        city = min(((math.hypot(cx - x, cy - y) if cm == m else 1e9), cn) for cn, cm, cx, cy in CAPITALS)[1]
        if side == 0:
            side = 2 if city in HORDE_CITIES else 1
        tlines.append("%d|%s|%d|%d|%d|%s|%s|%s|Riding|%s\n" % (side, city, m, int(r["entry"]), RIDING_SKILL_LINE, pos(r), clean(r["name"]), clean(r["subname"]), r["guid"]))
    report["trainers"] = len(tlines)
    vendors_text = (
        "# Mount vendors for MountBuy (src/mount_plan.h), made by tools/gen_mounts.py from the world database. Two kinds of line:\n"
        "#   vendor|raceMask|map|entry|guid|x y z|name|subname\n"
        "#   item|vendorEntry|itemEntry|requiredLevel|ridingSkill|copper|name\n"
        "# raceMask: the race the vendor's mounts are for (1 Human, 2 Orc, 4 Dwarf, 8 Night Elf, 16 Undead, 32 Tauren, 64 Gnome, 128 Troll); a bot buys from a vendor of its own race.\n"
        "# ridingSkill: the riding skill the mount needs (75 Apprentice for a level 40 mount, 150 Journeyman for a level 60 mount).\n"
        + "".join(vlines) + "".join(ilines))
    trainers_text = (
        "# Riding trainers for the riding errand (RidingTraining = 1 with Errands = 1), made by tools/gen_mounts.py. Same format as errand_trainers.txt:\n"
        "# side|city|map|entry|skill|x y z|name|subname|profession|guid\n"
        "# skill 762 is the riding skill line. city is the nearest capital (only used to spread the bots and to name the trip in the journal): the trainers stand in\n"
        "# the race villages next to their mount vendors, not inside the capitals.\n" + "".join(tlines))
    ranks_text = (
        "# spell|minLevel|skill|copper|name\n"
        "# Riding ranks for RidingTraining (src/riding_training.h), cheapest first, made by tools/gen_mounts.py from the trainer_spell rows of the riding trainers.\n"
        + "".join(rank_lines))
    return vendors_text, trainers_text, ranks_text, report


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--mysql", help='mysql command, e.g. "ssh forever-test sudo -n mysql"')
    ap.add_argument("--offline-json", help="query results (keys vendors, trainers, ranks, names) instead of a database")
    ap.add_argument("--world-db", default="forever_test_world")
    ap.add_argument("--hotfix-db", default="forever_test_hotfixes")
    ap.add_argument("--out-dir", required=True)
    args = ap.parse_args(argv)
    if args.offline_json:
        with open(args.offline_json, encoding="utf-8") as fh:
            data = json.load(fh)
    elif args.mysql:
        data = fetch(args)
    else:
        ap.error("give --mysql or --offline-json")
    v, t, r, report = build(data)
    os.makedirs(args.out_dir, exist_ok=True)
    for name, text in (("mount_vendors.txt", v), ("riding_trainers.txt", t), ("riding_ranks.txt", r)):
        with open(os.path.join(args.out_dir, name), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
    print("wrote %d vendors, %d mounts, %d trainers, %d ranks to %s" % (report["vendors"], report["items"], report["trainers"], report["ranks"], args.out_dir))
    for s in report["vendors_skipped"]:
        print("left out (no race in the subname): " + s)


if __name__ == "__main__":
    main()
