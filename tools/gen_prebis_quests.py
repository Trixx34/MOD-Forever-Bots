#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Level-60 quest lists for the pre-raid progression (doc/pre-raid-progression.md): the quests whose rewards are on a class's pre-raid gear list, and the
attunement chains, written in the format of data/world_quests_*_60.txt so the existing QuestFile path (QuestFileAlliance60, QuestFileHorde60) reads them.

Inputs
  --prebis DIR       the pre-raid gear lists (<classId>.txt from tools/gen_prebis.py): a comment line "# slot=.. ilvl=.. quality=.. source=<tag> name" followed by the item entry.
                     Only entries with source=quest are used. The generator never looks at an item entry's number: the list says which items, the database says which
                     quest rewards them.
  --attunements FILE rules for the attunement chains (data/attunements.txt): "attunement <name> <alliance|horde|both> <final quest id>". A chain is the final quest and
                     everything that leads to it, found by walking PrevQuestID and RewardNextQuest backwards in the world database.
  --pool FILE        (repeatable, per side) the quest blocks that already exist for the side, leveling list first (data/world_quests_<side>.txt, data/world_quests_<side>_60.txt).
                     A block is only copied from here, so every output quest has a giver, an ender, objectives and areas the planner understands.
  --mysql CMD        or --offline-json FILE: the world database tables as JSON (tests/data/prebis-quests-fixture.json), see load_from_db. Only SELECT statements run.
Output (per side) in --out: prebis_quests_<side>_60.txt (wishlist quests and attunement quests), and for both sides attunement_chains.txt (the ordered chains
with the placement status of every quest) and prebis_quests_report.txt.

Rules (all on the quest or the list, never on an item entry)
  - a quest is wanted when one of its reward or choice-reward items is on a class's list with source=quest; a quest of a raid chain or for a party of ten is never wanted
    (the gear generator already kept those items out of the lists as source=quest);
  - the chain of a wanted quest is wanted too (PrevQuestID walked back while the quest is in the pool);
  - the block's `req` loses its `level` (a level-60 bot is above the grey margin of a level-52 quest, the planner would skip it) and gets `classes` set to the classes
    whose list wants it (a mage does not walk a warrior's quest); an attunement quest keeps the classes it had;
  - a wanted quest that is not in the pool (no creature giver, ender on another map, an objective the planner cannot do) is reported with the reason it is missing.
Python 3 standard library only.
"""
import argparse
import json
import os
import re
import shlex
import subprocess
import sys

CLASS_NAMES = {1: "warrior", 2: "paladin", 3: "hunter", 4: "rogue", 5: "priest", 7: "shaman", 8: "mage", 9: "warlock", 11: "druid"}
REWARD_COLS = ["RewardItem%d" % i for i in range(1, 5)] + ["RewardChoiceItemID%d" % i for i in range(1, 7)]
RAID_SUGGESTED_GROUP = 10          # a quest for ten or more players is a raid quest
QUEST_FLAG_RAID = 0x40             # QUEST_FLAGS_RAID


# ---- the gear lists ----

def read_prebis(folder):
    """{class id: [(item entry, slot, source tag, name)]} from folder/<classId>.txt."""
    out = {}
    for name in sorted(os.listdir(folder)):
        m = re.fullmatch(r"(\d+)\.txt", name)
        if not m:
            continue
        entries, meta = [], None
        with open(os.path.join(folder, name), encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line.startswith("#"):
                    mm = re.match(r"#\s*slot=(\S+)\s+ilvl=\S+\s+quality=\S+\s+source=(\S+)\s*(.*)", line)
                    meta = (mm.group(1), mm.group(2), mm.group(3)) if mm else None
                elif line.isdigit():
                    slot, tag, text = meta if meta else ("?", "none", "")
                    entries.append((int(line), slot, tag, text))
                    meta = None
        out[int(m.group(1))] = entries
    return out


def quest_wishes(prebis, rewards_of):
    """{quest id: {class id: [item entry]}}: for each class the quests that reward one of its source=quest items. rewards_of: {item: [quest ids]}."""
    wanted = {}
    for class_id, entries in prebis.items():
        for item, _slot, tag, _name in entries:
            if tag != "quest":
                continue
            for quest in rewards_of.get(item, ()):
                wanted.setdefault(quest, {}).setdefault(class_id, []).append(item)
    return wanted


# ---- the database ----

class Db:
    def __init__(self, command):
        self.command = shlex.split(command) + ["--batch", "--raw"]

    def rows(self, sql):
        if not re.match(r"\s*select\b", sql, re.I):
            raise ValueError("only SELECT statements are allowed: " + sql[:60])
        done = subprocess.run(self.command, input=sql, capture_output=True, text=True)
        if done.returncode != 0:
            raise RuntimeError("mysql failed: " + done.stderr.strip()[:300])
        lines = done.stdout.splitlines()
        if not lines:
            return []
        header = lines[0].split("\t")
        return [dict(zip(header, (None if v == "NULL" else v for v in line.split("\t")))) for line in lines[1:]]


def load_from_db(command):
    """Every quest with its title, chain links and reward items: {"quests": [{id, title, sort, group, flags, prev, next, startItem, rewards: [..]}]}."""
    db = Db(command)
    rows = db.rows("SELECT q.ID, q.LogTitle, q.QuestSortID, q.SuggestedGroupNum, q.Flags, q.RewardNextQuest, q.StartItem, IFNULL(a.PrevQuestID, 0) AS Prev, IFNULL(a.NextQuestID, 0) AS AddonNext, " +
                   ", ".join("q." + c for c in REWARD_COLS) + " FROM quest_template q LEFT JOIN quest_template_addon a ON a.ID = q.ID")
    starters = {int(r["quest"]) for r in db.rows("SELECT DISTINCT quest FROM creature_queststarter")}
    enders = {int(r["quest"]) for r in db.rows("SELECT DISTINCT quest FROM creature_questender")}
    levels = {int(r["ID"]): int(r["QuestLevel"]) for r in db.rows("SELECT ID, QuestLevel FROM quest_classic_level")}
    quests = []
    for r in rows:
        quests.append({"id": int(r["ID"]), "title": (r["LogTitle"] or "").replace("\n", " "), "sort": int(r["QuestSortID"]), "group": int(r["SuggestedGroupNum"]),
                       "flags": int(r["Flags"]), "prev": int(r["Prev"]), "next": int(r["RewardNextQuest"]), "addonNext": int(r["AddonNext"]),
                       "classicLevel": levels.get(int(r["ID"]), 0), "creatureStart": int(r["ID"]) in starters, "creatureEnd": int(r["ID"]) in enders, "startItem": int(r["StartItem"]),
                       "rewards": sorted({int(r[c]) for c in REWARD_COLS if int(r[c]) > 0})})
    return {"quests": quests}


class QuestTable:
    def __init__(self, data):
        self.by_id = {q["id"]: q for q in data["quests"]}
        self.rewards_of = {}
        for q in data["quests"]:
            if q["group"] >= RAID_SUGGESTED_GROUP or q["flags"] & QUEST_FLAG_RAID:
                continue
            for item in q["rewards"]:
                self.rewards_of.setdefault(item, []).append(q["id"])
        self.pred = {}
        for q in data["quests"]:
            if q["prev"] > 0:
                self.pred.setdefault(q["id"], set()).add(q["prev"])
            if q["next"] > 0:
                self.pred.setdefault(q["next"], set()).add(q["id"])
            if q.get("addonNext", 0) > 0:        # NextQuestID of the addon: the quest that follows when this one is done (the Test of Skulls quests lead to Axtroz)
                self.pred.setdefault(q["addonNext"], set()).add(q["id"])

    def chain(self, final):
        """The final quest and everything that leads to it, in the order to do them (a quest after all of its predecessors; ties by quest id)."""
        depth, queue = {final: 0}, [final]
        while queue:
            q = queue.pop(0)
            for p in self.pred.get(q, ()):
                if p in self.by_id and p not in depth:
                    depth[p] = depth[q] + 1
                    queue.append(p)
        return sorted(depth, key=lambda q: (-depth[q], q))


# ---- quest files ----

class Block:
    """One quest of a quest file: the `quest` line and the lines that name it (req, hunt, area, reward, startfrom)."""

    def __init__(self, qid, lines):
        self.id, self.lines = qid, lines

    @property
    def quest_line(self):
        return self.lines[0]

    def giver(self):
        t = self.lines[0].split()
        return tuple(float(x) for x in t[3:6])

    def req(self):
        for line in self.lines:
            t = line.split()
            if t[0] == "req":
                return dict(zip(t[2::2], t[3::2]))
        return {}

    def with_req(self, change):
        """A copy with the req record rewritten by change(dict) -> dict (a pair with value None is dropped)."""
        out, done = [], False
        for line in self.lines:
            t = line.split()
            if t[0] == "req":
                pairs = change(dict(zip(t[2::2], t[3::2])))
                done = True
                text = " ".join("%s %s" % (k, v) for k, v in pairs.items() if v is not None)
                if text:
                    out.append("req %d %s" % (self.id, text))
            else:
                out.append(line)
        if not done:
            pairs = change({})
            text = " ".join("%s %s" % (k, v) for k, v in pairs.items() if v is not None)
            if text:
                out.insert(1, "req %d %s" % (self.id, text))
        return Block(self.id, out)


class QuestFile:
    def __init__(self):
        self.starts = []   # [(name, map, [Block])] in file order

    @staticmethod
    def parse(path):
        f = QuestFile()
        name, current, qlines, qid = None, None, None, None
        with open(path, encoding="utf-8") as handle:
            for raw in handle:
                line = raw.split("#", 1)[0].strip()
                if not line:
                    continue
                t = line.split()
                if t[0] == "start":
                    name, current, qlines = t[1], [], None
                    f.starts.append([name, 0, current])
                elif t[0] == "anchor" and current is not None:
                    f.starts[-1][1] = int(t[1])
                elif t[0] == "quest" and current is not None:
                    qid = int(t[1])
                    qlines = [line]
                    current.append(Block(qid, qlines))
                elif qlines is not None and len(t) > 1 and t[1].isdigit() and int(t[1]) == qid:
                    qlines.append(line)
        return f

    def blocks(self):
        for name, map_id, blocks in self.starts:
            for b in blocks:
                yield name, map_id, b


def load_pool(paths):
    """{quest id: (start name, map, Block)}: the first file that has a quest wins (the level-60 list before the leveling list is the caller's order)."""
    pool = {}
    for path in paths:
        for name, map_id, block in QuestFile.parse(path).blocks():
            pool.setdefault(block.id, (name, map_id, block))
    return pool


def write_quest_file(path, header, chosen):
    """chosen: [(start name, map, Block)] in order. Quests of the same start name are written together; the anchor is the average giver position."""
    order, groups = [], {}
    for name, map_id, block in chosen:
        key = (name, map_id)
        if key not in groups:
            groups[key] = []
            order.append(key)
        groups[key].append(block)
    out = list(header) + [""]
    for key in order:
        blocks = groups[key]
        gx = sum(b.giver()[0] for b in blocks) / len(blocks)
        gy = sum(b.giver()[1] for b in blocks) / len(blocks)
        gz = sum(b.giver()[2] for b in blocks) / len(blocks)
        out.append("start %s   # %d quests" % (key[0], len(blocks)))
        out.append("anchor %d %.1f %.1f %.1f" % (key[1], gx, gy, gz))
        for b in blocks:
            out.extend(b.lines)
        out.append("")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(out))


# ---- the selection ----

def class_mask(classes):
    return sum(1 << (c - 1) for c in classes)


def read_attunements(path):
    """[(name, side, final quest)] from "attunement <name> <alliance|horde|both> <final quest>" lines."""
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            t = line.split("#", 1)[0].split()
            if not t:
                continue
            if t[0] != "attunement" or len(t) < 4 or t[2] not in ("alliance", "horde", "both") or not t[3].isdigit():
                raise SystemExit("bad attunement line: " + line.strip())
            out.append((t[1], t[2], int(t[3])))
    return out


def missing_reason(table, qid):
    q = table.by_id.get(qid)
    if q is None:
        return "not in quest_template"
    why = []
    if not q.get("creatureStart", True):
        why.append("no creature giver" + (" (starts from an item)" if q["startItem"] else " (an object or a script gives it)"))
    if not q.get("creatureEnd", True):
        why.append("no creature ender")
    if q.get("classicLevel", 1) == 0:
        why.append("no row in quest_classic_level (the generator cannot place it)")
    if q["group"] > 1:
        why.append("for a party of %d" % q["group"])
    if q["flags"] & QUEST_FLAG_RAID:
        why.append("flagged raid-eligible")
    return ", ".join(why) or "ender on another map or an objective the planner cannot do"


def build_side(side, table, wanted, attunements, pool):
    """Returns (chosen [(start, map, Block)], notes). chosen is ordered by the pool's order; wanted: {quest: {class: [items]}}."""
    need = {}                    # quest id -> {"classes": set or None (any), "why": [..]}

    def want(qid, classes, why):
        e = need.setdefault(qid, {"classes": set(), "why": []})
        if classes is None or e["classes"] is None:
            e["classes"] = None
        else:
            e["classes"] |= classes
        e["why"].append(why)

    missing = {}                 # quest id -> reason
    chains = []
    for qid, by_class in wanted.items():
        if qid not in pool:
            missing[qid] = missing_reason(table, qid)
            continue
        # the quests that must be done first, while the pool has them
        stack, seen = [qid], set()
        while stack:
            q = stack.pop()
            if q in seen:
                continue
            seen.add(q)
            want(q, set(by_class), "gear of " + ",".join(CLASS_NAMES[c] for c in sorted(by_class)) if q == qid else "chain of %d" % qid)
            prev = table.by_id.get(q, {}).get("prev", 0)
            if prev > 0:
                if prev in pool:
                    stack.append(prev)
                else:
                    missing[prev] = missing_reason(table, prev) + " (needed first for %d)" % q
    for name, a_side, final in attunements:
        if a_side not in ("both", side):
            continue
        order = table.chain(final)
        status = []
        for q in order:
            if q in pool:
                want(q, None, "attunement " + name)
                status.append((q, "placed"))
            else:
                reason = missing_reason(table, q)
                missing[q] = reason
                status.append((q, reason))
        chains.append((name, order, status))
    chosen = []
    order = {q: i for i, q in enumerate(pool)}
    for qid in sorted(need, key=order.get):
        start, map_id, block = pool[qid]
        classes = need[qid]["classes"]

        def change(req, classes=classes):
            req = dict(req)
            req.pop("level", None)
            if classes is not None:
                had = int(req.get("classes", 0))
                mask = class_mask(classes)
                req["classes"] = str(mask & had if had else mask)
            return req

        chosen.append((start, map_id, block.with_req(change)))
    return chosen, need, missing, chains


def main(argv):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--mysql")
    src.add_argument("--offline-json")
    p.add_argument("--dump-json", help="write the database tables read to this JSON file (a fixture)")
    p.add_argument("--prebis", default="data/prebis")
    p.add_argument("--attunements", default="data/attunements.txt")
    p.add_argument("--pool-alliance", action="append", default=[], help="quest file of the Alliance, leveling list first, level 60 list last (repeat)")
    p.add_argument("--pool-horde", action="append", default=[], help="the same for the Horde")
    p.add_argument("--out", default="data")
    args = p.parse_args(argv)
    if args.offline_json:
        with open(args.offline_json, encoding="utf-8") as f:
            data = json.load(f)
    else:
        data = load_from_db(args.mysql)
    if args.dump_json:
        with open(args.dump_json, "w", encoding="utf-8") as f:
            json.dump(data, f)
    table = QuestTable(data)
    prebis = read_prebis(args.prebis)
    wanted = quest_wishes(prebis, table.rewards_of)
    attunements = read_attunements(args.attunements)
    os.makedirs(args.out, exist_ok=True)
    report = ["# Pre-raid quest report, made by tools/gen_prebis_quests.py; do not edit by hand."]
    by_tag = {}
    for class_id, entries in prebis.items():
        for _i, _s, tag, _n in entries:
            by_tag.setdefault(tag, {}).setdefault(class_id, 0)
            by_tag[tag][class_id] += 1
    report.append("# Wishlist items by source tag (all classes): " + ", ".join("%s %d" % (t, sum(c.values())) for t, c in sorted(by_tag.items())))
    report.append("# Wishlist items with source=quest but no quest found in the database: " + str(sum(
        1 for c, es in prebis.items() for i, _s, tag, _n in es if tag == "quest" and i not in table.rewards_of)))
    chain_lines = ["# Attunement chains, made by tools/gen_prebis_quests.py from data/attunements.txt; do not edit by hand.",
                   "# chain <name> <side> <quest> <quest> ...   (do in this order; the last is the attunement quest)",
                   "# status <name> <side> <quest> <placed | reason it is not in the quest list>"]
    for side, pools in (("alliance", args.pool_alliance), ("horde", args.pool_horde)):
        if not pools:
            continue
        pool = load_pool(pools)
        chosen, need, missing, chains = build_side(side, table, wanted, attunements, pool)
        header = ["# Level-60 pre-raid quests for the %s: rewards on the class gear lists (source=quest) and the attunement chains." % side,
                  "# Made by tools/gen_prebis_quests.py (doc/pre-raid-progression.md); do not edit by hand. Format: src/quest_data.h.",
                  "# Use as QuestFile%s60 (the `level` of a block is dropped, `classes` limits a quest to the classes that want its reward)." % side.capitalize()]
        write_quest_file(os.path.join(args.out, "prebis_quests_%s_60.txt" % side), header, chosen)
        gear = sum(1 for e in need.values() if any(w.startswith("gear") for w in e["why"]))
        report.append("# %s: %d quests in the list (%d for gear, %d attunement or chain), %d wanted quests missing from the pool" % (
            side, len(need), gear, len(need) - gear, len(missing)))
        for qid in sorted(missing):
            report.append("missing %s %d %s %s" % (side, qid, table.by_id.get(qid, {}).get("title", "?"), missing[qid]))
        for name, order, status in chains:
            chain_lines.append("chain %s %s %s" % (name, side, " ".join(str(q) for q in order)))
            for q, st in status:
                chain_lines.append("status %s %s %d %s   # %s" % (name, side, q, st, table.by_id.get(q, {}).get("title", "?")))
    with open(os.path.join(args.out, "attunement_chains.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(chain_lines) + "\n")
    with open(os.path.join(args.out, "prebis_quests_report.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(report) + "\n")
    print("\n".join(l for l in report if l.startswith("# ")))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
