#!/usr/bin/env python3
"""Generate data/reputation_sources.txt: where a bot earns the reputation of the factions in data/reputation_targets.txt (doc/reputation.md).

Three kinds of source, all read from the world database (SELECT only) and joined with the quest files the quest planner already knows (the quest level and the
quest planner's own coverage come from data/world_quests*.txt, so every quest listed is one a bot can walk):

  q|faction|quest|points|level|side|kind|cap     a quest that rewards reputation. points: the quest's reward (QuestFactionReward value index, or the override in
                                                   hundredths), kind 0 plain quest, 1 item hand-in (a quest objective of type item), 2 kills (objective type creature).
                                                   cap: the highest rank the quest still gives reputation up to (0 = no cap, from RewardFactionCapIn).
  k|faction|creature|points|cap|map|x|y|z|spawns  a creature that gives reputation when killed (creature_onkill_reputation): points, the highest rank (MaxStanding),
                                                   one spawn (the first by guid) and the number of spawns. Only positive rewards and creatures with a spawn.

Nothing is selected by item entry. Python 3 standard library only.

    tools/gen_reputation_sources.py --mysql "ssh forever-test sudo -n mysql" --out data/reputation_sources.txt
"""
import argparse
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_prebis as g  # noqa: E402

# QuestFactionReward.db2 (classic row): index 1..8 -> points, a negative index is a loss.
POINTS = [0, 10, 25, 75, 150, 250, 350, 500, 1000]
ALLIANCE_RACES, HORDE_RACES = 1101, 690

HEADER = """# Reputation sources (tools/gen_reputation_sources.py): where the factions of data/reputation_targets.txt give reputation. '|' separated:
#   q|faction|quest|points|level|side|kind|cap      a quest (side 0 both, 1 Alliance, 2 Horde; kind 0 plain, 1 item hand-in, 2 kills; cap: highest rank, 0 none)
#   k|faction|creature|points|cap|map|x|y|z|spawns  a creature kill (cap: highest rank it still gives reputation up to; one spawn and the number of spawns)
"""


def side_of(mask):
    if mask <= 0 or mask == 2 ** 32 - 1:
        return 0
    a, h = bool(mask & ALLIANCE_RACES), bool(mask & HORDE_RACES)
    return 1 if a and not h else 2 if h and not a else 0


def points(index, override):
    if override:
        return int(override) // 100
    index = int(index)
    return (POINTS[index] if index > 0 else -POINTS[-index]) if abs(index) < len(POINTS) else 0


def quest_levels(folder):
    """quest id -> its level (req lines of the world quest files)."""
    levels = {}
    for path in glob.glob(os.path.join(folder, "world_quests*.txt")):
        with open(path, encoding="utf-8") as fh:
            for line in fh:
                m = re.match(r"req (\d+)\b.*?\blevel (\d+)", line)
                if m:
                    levels[int(m.group(1))] = int(m.group(2))
    return levels


def targets(path):
    out = []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            if line.strip() and not line.startswith("#"):
                out.append(int(line.split("|")[0]))
    return out


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    data = os.path.join(here, "..", "data")
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--mysql", required=True)
    ap.add_argument("--world-db", default="forever_test_world")
    ap.add_argument("--targets", default=os.path.join(data, "reputation_targets.txt"))
    ap.add_argument("--quest-dir", default=data)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    db = g.Db(args.mysql)
    w = args.world_db
    ids = targets(args.targets)
    flist = ",".join(str(i) for i in ids)
    levels = quest_levels(args.quest_dir)
    out = [HEADER]
    kinds = {}
    for r in db.rows("SELECT o.QuestID AS q, MAX(o.Type = 1) AS item, MAX(o.Type = 0) AS kills FROM %s.quest_objectives o GROUP BY o.QuestID" % w):
        kinds[int(r["q"])] = 1 if r["item"] == "1" else 2 if r["kills"] == "1" else 0
    nq = 0
    for n in range(1, 6):
        for r in db.rows(
                "SELECT ID AS q, RewardFactionID%d AS f, RewardFactionValue%d AS v, RewardFactionOverride%d AS o, RewardFactionCapIn%d AS c, AllowableRaces AS races "
                "FROM %s.quest_template WHERE RewardFactionID%d IN (%s)" % (n, n, n, n, w, n, flist)):
            q = int(r["q"])
            pts = points(r["v"], r["o"])
            if pts <= 0 or q not in levels:
                continue
            out.append("q|%s|%d|%d|%d|%d|%d|%s\n" % (r["f"], q, pts, levels[q], side_of(int(r["races"])), kinds.get(q, 0), r["c"]))
            nq += 1
    nk = 0
    spawns = {}
    for r in db.rows("SELECT id, MIN(guid) AS g, COUNT(*) AS n FROM %s.creature WHERE id IN (SELECT creature_id FROM %s.creature_onkill_reputation WHERE "
                     "RewOnKillRepFaction1 IN (%s) OR RewOnKillRepFaction2 IN (%s)) GROUP BY id" % (w, w, flist, flist)):
        spawns[int(r["id"])] = (int(r["g"]), int(r["n"]))
    place = {}
    for r in db.rows("SELECT guid, map, ROUND(position_x, 1) AS x, ROUND(position_y, 1) AS y, ROUND(position_z, 1) AS z FROM %s.creature WHERE id IN "
                     "(SELECT creature_id FROM %s.creature_onkill_reputation WHERE RewOnKillRepFaction1 IN (%s) OR RewOnKillRepFaction2 IN (%s))" % (w, w, flist, flist)):
        place[int(r["guid"])] = (r["map"], r["x"], r["y"], r["z"])
    for n in (1, 2):
        for r in db.rows(
                "SELECT RewOnKillRepFaction%d AS f, creature_id AS c, RewOnKillRepValue%d AS v, MaxStanding%d AS m FROM %s.creature_onkill_reputation "
                "WHERE RewOnKillRepFaction%d IN (%s) AND RewOnKillRepValue%d > 0" % (n, n, n, w, n, flist, n)):
            sp = spawns.get(int(r["c"]))
            if not sp:
                continue
            m, x, y, z = place[sp[0]]
            out.append("k|%s|%s|%s|%s|%s|%s|%s|%s|%d\n" % (r["f"], r["c"], r["v"], r["m"], m, x, y, z, sp[1]))
            nk += 1
    with open(args.out, "w", encoding="utf-8") as fh:
        fh.write("".join(out))
    print("wrote %d quests and %d kill sources to %s" % (nq, nk, args.out))


if __name__ == "__main__":
    main()
