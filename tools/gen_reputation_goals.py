#!/usr/bin/env python3
"""Generate data/reputation_goals.txt: the factions whose reputation gates equipment, for the bots' reputation goals (doc/reputation.md).

A goal line is a faction that gates at least one piece of equipment (item_sparse.MinFactionID / MinReputation of the hotfix database; the rank is the core's
ReputationRank 0 Hated .. 7 Exalted), with what a bot can plan with: the level span of that gear, the lowest rank it needs, how many vendors in npc_vendor sell any
of it (0 = no vendor in the world database yet), how many quests reward reputation with the faction, and the side. Nothing is selected by item entry: the rows
are made from the item columns, the vendor table and the quest table, so gear the Forever world adds later is picked up by running this again.

Left out: PvP honor and battleground factions (the items carry RequiredPVPRank, or are sold by a "Supply Officer" or an honor "Quartermaster"; the PvP track
has its own lists), and gear with no class usable by a bot (class 2 weapons and class 4 armour with an equipment slot only).

The side: the faction's parent chain in Faction.db2 reaches the Alliance (469) or the Horde (67); anything else is 0 (both). Faction names come from Faction.db2
(--faction-db2), the gate from the hotfix database. Reads only (every statement is a SELECT). Python 3 standard library only.

    tools/gen_reputation_goals.py --mysql "ssh forever-test sudo -n mysql" --faction-db2 /srv/forever/data/dbc/enUS/Faction.db2 --out data/reputation_goals.txt
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_prebis as g  # noqa: E402
import db2_reader  # noqa: E402

ALLIANCE, HORDE = 469, 67
HEADER = """# Reputation goals (tools/gen_reputation_goals.py): factions whose reputation gates equipment. One faction per line, '|' separated:
#   faction | name | side (0 both, 1 Alliance, 2 Horde) | min level | max level | min rank | items | vendors | quests
# min level, max level: the required levels of the faction-gated gear. min rank: the lowest reputation rank (0 Hated, 3 Neutral, 4 Friendly, 5 Honored,
# 6 Revered, 7 Exalted) any of it asks for. items: gated pieces of gear. vendors: creatures in npc_vendor that sell any of them (0: nobody sells them yet, so a bot
# has nothing to buy and the faction is no goal). quests: quests that reward reputation with it. PvP factions are left out (see the tool).
"""


def faction_table(path):
    if not path:
        return {}, {}
    rows = db2_reader.read_table(path, strings=(0,))
    names = {i: r[0] for i, r in rows.items()}
    parents = {i: r[3] for i, r in rows.items()}   # ParentFactionID
    return names, parents


def side_of(faction, parents):
    seen = set()
    while faction and faction not in seen:
        seen.add(faction)
        if faction == ALLIANCE:
            return 1
        if faction == HORDE:
            return 2
        faction = parents.get(faction, 0)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--mysql", required=True, help='mysql command; the databases are named in the statements, e.g. "ssh forever-test sudo -n mysql"')
    ap.add_argument("--world-db", default="forever_test_world")
    ap.add_argument("--hotfix-db", default="forever_test_hotfixes")
    ap.add_argument("--faction-db2", help="Faction.db2 of the client data, for names and sides")
    ap.add_argument("--out", required=True)
    ap.add_argument("--max-level", type=int, default=60, help="gear with a higher required level is left out")
    args = ap.parse_args()
    db = g.Db(args.mysql)
    w, h = args.world_db, args.hotfix_db
    gear = db.rows(
        "SELECT s.MinFactionID AS f, s.MinReputation AS r, a.id AS item, a.requiredLevel AS rl FROM %s.item_sparse s JOIN %s.armory_item a ON a.id = s.ID "
        "WHERE s.MinFactionID > 0 AND a.class IN (2, 4) AND a.inventoryType <> 0 AND a.quality >= 2 AND a.requiredLevel <= %d AND s.RequiredPVPRank = 0 AND s.RequiredPVPMedal = 0"
        % (h, w, args.max_level))
    pvp_vendor = {int(r["entry"]) for r in db.rows(
        "SELECT entry FROM %s.creature_template WHERE subname REGEXP 'Supply Officer|Quartermaster'" % w)}
    sold = {}
    for r in db.rows("SELECT entry, item FROM %s.npc_vendor WHERE item IN (SELECT ID FROM %s.item_sparse WHERE MinFactionID > 0)" % (w, h)):
        sold.setdefault(int(r["item"]), set()).add(int(r["entry"]))
    quests = {}
    for n in range(1, 6):
        for r in db.rows("SELECT RewardFactionID%d AS f, COUNT(*) AS n FROM %s.quest_template WHERE RewardFactionID%d > 0 GROUP BY f" % (n, w, n)):
            quests[int(r["f"])] = quests.get(int(r["f"]), 0) + int(r["n"])
    names, parents = faction_table(args.faction_db2)

    goals = {}
    for r in gear:
        f, rank, item, rl = int(r["f"]), int(r["r"]), int(r["item"]), int(r["rl"])
        vendors = {v for v in sold.get(item, set()) if v not in pvp_vendor}
        if sold.get(item) and not vendors:
            continue   # sold only by a PvP quartermaster
        e = goals.setdefault(f, {"min_rl": rl, "max_rl": rl, "rank": rank, "items": 0, "vendors": set()})
        e["min_rl"], e["max_rl"], e["rank"] = min(e["min_rl"], rl), max(e["max_rl"], rl), min(e["rank"], rank)
        e["items"] += 1
        e["vendors"] |= vendors
    out = [HEADER]
    for f in sorted(goals):
        e = goals[f]
        name = re.sub(r"[|\n]", " ", names.get(f, "faction %d" % f))
        out.append("%d|%s|%d|%d|%d|%d|%d|%d|%d\n" % (f, name, side_of(f, parents), e["min_rl"], e["max_rl"], e["rank"], e["items"], len(e["vendors"]), quests.get(f, 0)))
    with open(args.out, "w", encoding="utf-8") as fh:
        fh.write("".join(out))
    print("wrote %d factions to %s" % (len(goals), args.out))


if __name__ == "__main__":
    main()
