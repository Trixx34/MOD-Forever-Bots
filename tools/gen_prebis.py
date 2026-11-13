#!/usr/bin/env python3
"""Generate the pre-raid gear lists (data/prebis/<classId>.txt) from the world database.

The lists feed the module's ScenarioGearFile setting (doc/pre-raid-gear.md): every bot of a class is granted the listed items, then the core's equip
service wears the best of them. Selection is by item level, quality, source and class fit, never by item entry.

Reads the world database through a mysql command line (the SQL goes in on stdin, tab separated output comes back), for example
    tools/gen_prebis.py --mysql "ssh forever-test sudo -n mysql forever_test_world" --out data/prebis
or, without a database, from a JSON file with the same content (see tests/data/prebis-fixture.json):
    tools/gen_prebis.py --offline-json tests/data/prebis-fixture.json --out /tmp/prebis
It only reads: every statement is a SELECT. Python 3 standard library only.
"""
import argparse
import json
import os
import re
import shlex
import subprocess
import sys

# ---- game constants (the numbers of the 3.x/classic item tables the core and armory_item use) ----

ITEM_CLASS_WEAPON, ITEM_CLASS_ARMOR = 2, 4
CLASS_NAMES = {1: "warrior", 2: "paladin", 3: "hunter", 4: "rogue", 5: "priest", 7: "shaman", 8: "mage", 9: "warlock", 11: "druid"}

# ITEM_MOD_* numbers as they appear in armory_item.stats ("type:amount,...").
AGILITY, STRENGTH, INTELLECT, SPIRIT, STAMINA = 3, 4, 5, 6, 7
DEFENSE, DODGE, PARRY, BLOCK, CRIT, HASTE = 12, 13, 14, 15, 32, 36
PRIMARY = (AGILITY, STRENGTH, INTELLECT, SPIRIT)

# The core's gear profiles (ModuleBotActions.cpp PreferredStats): the stats a role wants. Profile 0 doubles as the class default.
PROFILES = {
    0: None,                                    # the class default, below
    1: (AGILITY, STAMINA),
    2: (INTELLECT, SPIRIT, STAMINA),
    3: (INTELLECT, CRIT, STAMINA),
    4: (INTELLECT, SPIRIT, HASTE, STAMINA),
    5: (STRENGTH, STAMINA, DODGE, PARRY, DEFENSE, BLOCK),   # shield tank
    6: (AGILITY, STAMINA, DODGE, DEFENSE),                  # feral tank
}
CLASS_DEFAULT_STATS = {
    1: (STRENGTH, STAMINA), 2: (STRENGTH, STAMINA), 3: (AGILITY, STAMINA), 4: (AGILITY, STRENGTH, STAMINA),
}
CASTER_STATS = (INTELLECT, SPIRIT, STAMINA)   # priest, shaman, mage, warlock, druid
# The one list per class that ScenarioGear<classId> can carry has no role: the roster profile of a class is picked here. The sim's raid and dungeon
# rosters send the warrior as the tank (kAutoGear spell_id 1), so the warrior list is the shield tank's. Override with --profile CLASS=N.
DEFAULT_ROLE_PROFILE = {1: 5}

# Armour: the heaviest subclass a class wears at level 60 (BotGearRules::MaxArmorSubclass). 1 cloth, 2 leather, 3 mail, 4 plate.
MAX_ARMOR = {1: 4, 2: 4, 3: 3, 4: 2, 5: 1, 7: 3, 8: 1, 9: 1, 11: 2}
SHIELD_CLASSES = (1, 2, 7)
# Weapon subclasses a class may wield (0 axe, 1 2h axe, 2 bow, 3 gun, 4 mace, 5 2h mace, 6 polearm, 7 sword, 8 2h sword, 10 staff, 13 fist, 15 dagger,
# 16 thrown, 18 crossbow, 19 wand), from the classic proficiency lists after the trainers. The core decides with skills at run time; this only keeps the lists sane.
WEAPONS = {
    1: {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 13, 15, 16, 18},
    2: {0, 1, 4, 5, 6, 7, 8},
    3: {0, 1, 2, 3, 6, 7, 8, 10, 13, 15, 18},
    4: {2, 3, 4, 7, 13, 15, 16, 18},
    5: {4, 10, 15, 19},
    7: {0, 1, 4, 5, 10, 13, 15},
    8: {7, 10, 15, 19},
    9: {7, 10, 15, 19},
    11: {4, 5, 6, 10, 13, 15},
}
DUAL_WIELD = (1, 3, 4, 7)
RELIC = {2: 7, 11: 8, 7: 9}          # armor subclass of the relic a class carries: libram, idol, totem
HOLDABLE_CLASSES = (2, 5, 7, 8, 9, 11)

# inventoryType -> slot name (None: not equipment the generator hands out).
SLOT_OF = {1: "head", 2: "neck", 3: "shoulder", 5: "chest", 20: "chest", 6: "waist", 7: "legs", 8: "feet", 9: "wrist", 10: "hands",
           11: "finger", 12: "trinket", 16: "cloak", 14: "shield", 23: "holdable",
           13: "weapon1h", 21: "weapon1h", 22: "offhand", 17: "weapon2h", 15: "ranged", 25: "ranged", 26: "ranged", 28: "relic"}
BODY_SLOTS = ("head", "shoulder", "chest", "waist", "legs", "feet", "wrist", "hands")
# How many items a slot keeps. Pairs (rings, trinkets) keep two; one-handers two for a dual wielder; everything else one.
PAIR_SLOTS = ("finger", "trinket")

PLACEHOLDER = re.compile(r"deprecated|unused|\[ph\]|^zz|^test|^monster -|^qa ", re.I)
# A test item carries its item level in its name ("90 Green Warrior Axe", "63 Epic Rogue Tunic"): a leading number that equals the item level.
LEADING_NUMBER = re.compile(r"^(\d+)\s")
PVP_NAME = re.compile(r"^(Private|Scout|Corporal|Sergeant|Master Sergeant|Sergeant Major|Knight|Knight-Lieutenant|Knight-Captain|Knight-Champion|"
                      r"Lieutenant Commander|Commander|Marshal|Field Marshal|Grand Marshal|Stone Guard|Blood Guard|Legionnaire|Centurion|Champion|"
                      r"Lieutenant General|General|Warlord|High Warlord)'s ", re.I)

SOURCE_ORDER = ("pvp", "crafted", "dungeon", "quest", "world", "vendor")


# ---- the database side ----

class Db:
    """Runs SELECTs through the mysql command line; the statement goes in on stdin."""

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


def idlist(ids):
    return ",".join(str(i) for i in sorted(ids)) or "0"


def resolve_loot(rows, references):
    """Items reachable from loot rows (Entry, ItemType, Item): ItemType 1 is a reference to reference_loot_template, followed recursively."""
    items, seen, queue = set(), set(), []
    for row in rows:
        queue.append((int(row["ItemType"]), int(row["Item"])))
    while queue:
        kind, value = queue.pop()
        if kind == 0:
            items.add(value)
        elif kind == 1 and value not in seen:
            seen.add(value)
            queue.extend(references.get(value, ()))
    return items


def load_from_db(args):
    db = Db(args.mysql)
    quality_list = idlist(range(args.min_quality, args.max_quality + 1))
    items = db.rows(
        "SELECT id, name, quality, itemLevel, requiredLevel, class, subclass, inventoryType, bonding, stats, allowableClass, requiredSkill, requiredSkillRank "
        "FROM armory_item WHERE class IN (2, 4) AND quality IN (%s) AND itemLevel BETWEEN %d AND %d AND requiredLevel <= %d AND inventoryType <> 0"
        % (quality_list, args.min_ilvl, args.max_ilvl, args.level))

    # The loot tables of a set of maps: creature loot ids (creature_template_difficulty.LootID of every creature spawned there) and chest loot ids.
    def loot_roots(maps):
        m = idlist(maps)
        creature = {int(r["LootID"]) for r in db.rows(
            "SELECT DISTINCT d.LootID FROM creature c JOIN creature_template_difficulty d ON d.Entry = c.id WHERE c.map IN (%s) AND d.LootID > 0" % m)}
        chest = {int(r["Data1"]) for r in db.rows(
            "SELECT DISTINCT t.Data1 FROM gameobject g JOIN gameobject_template t ON t.entry = g.id WHERE t.type = 3 AND t.Data1 > 0 AND g.map IN (%s)" % m)}
        return creature, chest

    references = {}
    for r in db.rows("SELECT Entry, ItemType, Item FROM reference_loot_template"):
        references.setdefault(int(r["Entry"]), []).append((int(r["ItemType"]), int(r["Item"])))

    def items_dropped_on(maps):
        creature, chest = loot_roots(maps)
        rows = []
        if creature:
            rows += db.rows("SELECT Entry, ItemType, Item FROM creature_loot_template WHERE Entry IN (%s)" % idlist(creature))
        if chest:
            rows += db.rows("SELECT Entry, ItemType, Item FROM gameobject_loot_template WHERE Entry IN (%s)" % idlist(chest))
        return resolve_loot(rows, references)

    raid = items_dropped_on(args.raid_maps)
    dungeon = items_dropped_on(args.dungeon_maps)
    # Any creature or reference drop at all: what is neither raid nor dungeon is a world drop.
    anywhere = {int(r["Item"]) for r in db.rows("SELECT DISTINCT Item FROM creature_loot_template WHERE ItemType = 0")}
    for entries in references.values():
        anywhere |= {v for k, v in entries if k == 0}
    # Quest rewards, except those of raid quests: a quest of a raid zone (QuestSortID is an AreaTable id), of the raid quest kinds (QuestInfoID) or
    # suggested for ten or more players is raid-tier content (attunements, Zandalar and Ahn'Qiraj chains).
    quest = set()
    not_raid = ("QuestSortID NOT IN (%s) AND QuestInfoID NOT IN (%s) AND SuggestedGroupNum < 10" % (idlist(args.raid_quest_sorts), idlist(args.raid_quest_infos)))
    for column in ["RewardItem%d" % i for i in range(1, 5)] + ["RewardChoiceItemID%d" % i for i in range(1, 7)]:
        quest |= {int(r["i"]) for r in db.rows("SELECT DISTINCT %s AS i FROM quest_template WHERE %s > 0 AND %s" % (column, column, not_raid))}
    vendor = {int(r["item"]) for r in db.rows("SELECT DISTINCT item FROM npc_vendor WHERE item > 0")}
    crafted = {int(r["i"]) for r in db.rows(
        "SELECT DISTINCT EffectItemType AS i FROM %s.spell_effect WHERE Effect = 24 AND EffectItemType > 0" % args.hotfix_db)}
    pvp = {int(r["i"]) for r in db.rows(
        "SELECT ID AS i FROM %s.item_sparse WHERE RequiredPVPRank > 0 OR RequiredPVPMedal > 0" % args.hotfix_db)}
    return {
        "items": items,
        "raid_loot": sorted(raid),
        "sources": {"dungeon": sorted(dungeon), "world": sorted(anywhere - dungeon - raid), "quest": sorted(quest), "vendor": sorted(vendor),
                    "crafted": sorted(crafted), "pvp": sorted(pvp)},
    }


# ---- the selection ----

def parse_stats(text):
    out = []
    for part in (text or "").split(","):
        if ":" in part:
            kind, amount = part.split(":", 1)
            try:
                out.append((int(kind), int(amount)))
            except ValueError:
                pass
    return out


def preferred_stats(class_id, profile):
    if profile:
        return PROFILES[profile]
    return CLASS_DEFAULT_STATS.get(class_id, CASTER_STATS)


def stat_score(item, wanted):
    """The core's GearScore: item level * 10 plus 3x a wanted primary, 1x another wanted stat, minus 3x an unwanted primary (ModuleBotSupplyRules)."""
    total = 0
    for kind, amount in parse_stats(item["stats"]):
        if amount <= 0:
            continue
        primary = kind in PRIMARY
        if kind in wanted:
            total += 3 * amount if primary else amount
        elif primary:
            total -= 3 * amount
    return int(item["itemLevel"]) * 10 + total


def is_test_item(item):
    """Custom test items, found by attributes and never by entry: a placeholder name (the core's own rule, ModuleBotSupplyRules::IsPlaceholderItemName),
    or a name that starts with its own item level ("90 Green Warrior Axe"). Items with no source at all are dropped separately (see sourced())."""
    name = item["name"]
    if not name or PLACEHOLDER.search(name):
        return True
    match = LEADING_NUMBER.match(name)
    return bool(match and int(match.group(1)) == int(item["itemLevel"]))


def tag_of(item_id, sources, name):
    tags = [t for t in SOURCE_ORDER if item_id in sources[t]]
    if PVP_NAME.match(name) and "pvp" not in tags:
        tags.insert(0, "pvp")
    return tags[0] if tags else None


def class_usable(item, class_id):
    allowed = int(item["allowableClass"]) if item["allowableClass"] not in (None, "") else -1
    if allowed != -1 and allowed != 0 and not (allowed & (1 << (class_id - 1))):
        return False
    if item["requiredSkill"] not in (None, "", "0"):
        return False        # a profession-gated item: the bot may not have the profession
    cls, sub, inv = int(item["class"]), int(item["subclass"]), int(item["inventoryType"])
    slot = SLOT_OF.get(inv)
    if slot is None:
        return False
    if cls == ITEM_CLASS_WEAPON:
        return sub in WEAPONS[class_id] and (slot != "offhand" or class_id in DUAL_WIELD)
    if slot in BODY_SLOTS:
        return 1 <= sub <= MAX_ARMOR[class_id]
    if slot == "shield":
        return class_id in SHIELD_CLASSES and sub == 6
    if slot == "holdable":
        return class_id in HOLDABLE_CLASSES
    if slot == "relic":
        return RELIC.get(class_id) == sub
    return True             # neck, finger, trinket, cloak


def select_for_class(class_id, items, sources, raid_loot, args, profile):
    wanted = preferred_stats(class_id, profile)
    by_slot = {}
    for item in items:
        name = item["name"]
        if not (args.min_quality <= int(item["quality"]) <= args.max_quality and args.min_ilvl <= int(item["itemLevel"]) <= args.max_ilvl
                and int(item["requiredLevel"]) <= args.level and int(item["class"]) in (ITEM_CLASS_WEAPON, ITEM_CLASS_ARMOR)):
            continue
        if int(item["bonding"] or 0) == 4:
            continue                                              # quest-bound
        if is_test_item(item) or int(item["id"]) in raid_loot:
            continue
        tag = tag_of(int(item["id"]), sources, name)
        if tag is None and not args.keep_unsourced:
            continue                                              # no drop, quest, vendor, craft or PvP source: not obtainable in the game
        if tag == "pvp" and args.no_pvp:
            continue
        if not class_usable(item, class_id):
            continue
        slot = SLOT_OF[int(item["inventoryType"])]
        by_slot.setdefault(slot, []).append((stat_score(item, wanted), int(item["quality"]), item, tag))

    chosen = []
    for slot, cands in by_slot.items():
        if slot in BODY_SLOTS:
            # The core drops armour lighter than the class maximum when a maximum-type piece exists for the slot (BotGearRules::ChooseForSlot).
            top = [c for c in cands if int(c[2]["subclass"]) == MAX_ARMOR[class_id]]
            cands = top or cands
        # Best first: score (item level first, then stat fit), quality, then name and entry only to make the order stable.
        cands.sort(key=lambda c: (-c[0], -c[1], c[2]["name"], int(c[2]["id"])))
        keep = 2 if slot in PAIR_SLOTS or (slot in ("weapon1h",) and class_id in DUAL_WIELD) else args.per_slot
        for c in cands[:keep]:
            chosen.append((slot, c))
    order = {s: i for i, s in enumerate(("head", "neck", "shoulder", "cloak", "chest", "wrist", "hands", "waist", "legs", "feet", "finger", "trinket",
                                         "weapon2h", "weapon1h", "offhand", "shield", "holdable", "ranged", "relic"))}
    chosen.sort(key=lambda sc: (order.get(sc[0], 99), -sc[1][0]))
    return chosen


def write_lists(data, args):
    items = data["items"]
    sources = {k: set(v) for k, v in data["sources"].items()}
    for k in SOURCE_ORDER:
        sources.setdefault(k, set())
    raid_loot = set(data["raid_loot"])
    os.makedirs(args.out, exist_ok=True)
    report = {}
    for class_id, class_name in sorted(CLASS_NAMES.items()):
        profile = args.profiles.get(class_id, DEFAULT_ROLE_PROFILE.get(class_id, 0))
        chosen = select_for_class(class_id, items, sources, raid_loot, args, profile)
        lines = ["# Pre-raid gear for %s (class %d), generated by tools/gen_prebis.py; do not edit by hand." % (class_name, class_id),
                 "# Rules: quality %d-%d, item level %d-%d, required level <= %d, no quest-bound items, no test items, none dropped by raid creatures or chests (maps %s),"
                 % (args.min_quality, args.max_quality, args.min_ilvl, args.max_ilvl, args.level, ",".join(str(m) for m in args.raid_maps)),
                 "# usable by the class (armour type, weapon skill, class mask), best by item level then stat fit (stat profile %d)." % profile,
                 "# One item entry per line; the comment line above an entry gives: slot, item level, quality, source, name."]
        for slot, (score, quality, item, tag) in chosen:
            lines.append("# slot=%s ilvl=%s quality=%s source=%s %s" % (slot, item["itemLevel"], quality, tag or "none", item["name"]))
            lines.append(str(int(item["id"])))
        with open(os.path.join(args.out, "%d.txt" % class_id), "w") as f:
            f.write("\n".join(lines) + "\n")
        counts = {}
        for slot, c in chosen:
            counts[slot] = counts.get(slot, 0) + 1
        report[class_id] = {"items": len(chosen), "slots": counts,
                            "pvp": sum(1 for _, c in chosen if c[3] == "pvp"), "epic": sum(1 for _, c in chosen if c[1] == 4)}
    return report


def parse_args(argv):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--mysql", help='mysql command that reads the world database, e.g. "ssh forever-test sudo -n mysql forever_test_world"')
    src.add_argument("--offline-json", help="read the item and source tables from a JSON file instead of a database")
    p.add_argument("--out", default="data/prebis", help="folder for <classId>.txt (default data/prebis)")
    p.add_argument("--hotfix-db", default="forever_test_hotfixes", help="hotfixes database name (spell_effect, item_sparse)")
    p.add_argument("--level", type=int, default=60, help="bot level: RequiredLevel limit (default 60)")
    p.add_argument("--min-ilvl", type=int, default=58)
    p.add_argument("--max-ilvl", type=int, default=70)
    p.add_argument("--min-quality", type=int, default=3, help="3 rare (default)")
    p.add_argument("--max-quality", type=int, default=4, help="4 epic (default: epics are allowed, owner decision)")
    p.add_argument("--raid-maps", default="409,469,509,531,533,249,309", help="maps whose creature and chest loot is excluded")
    p.add_argument("--dungeon-maps", default="33,34,36,43,47,48,70,90,109,129,189,229,289,329,349,389,429,230", help="maps tagged as dungeon sources")
    p.add_argument("--raid-quest-sorts", default="19,2717,2677,2159,3429,3428,3456,1977",
                   help="QuestSortID (AreaTable id) of raid zones: Zul'Gurub, Molten Core, Blackwing Lair, Onyxia, AQ20, AQ40, Naxxramas; their quest rewards are excluded")
    p.add_argument("--raid-quest-infos", default="62,83", help="QuestInfoID of raid quests; their rewards are excluded")
    p.add_argument("--per-slot", type=int, default=1, help="items kept per slot (rings, trinkets and a dual wielder's one-handers keep 2)")
    p.add_argument("--profile", action="append", default=[], metavar="CLASS=N", help="stat profile 0-6 of a class list (default: warrior 5, shield tank)")
    p.add_argument("--no-pvp", action="store_true", help="leave out PvP rank items")
    p.add_argument("--keep-unsourced", action="store_true", help="keep items with no known source (default: dropped, they cannot be obtained)")
    args = p.parse_args(argv)
    args.raid_maps = [int(x) for x in args.raid_maps.split(",") if x]
    args.dungeon_maps = [int(x) for x in args.dungeon_maps.split(",") if x]
    args.raid_quest_sorts = [int(x) for x in args.raid_quest_sorts.split(",") if x]
    args.raid_quest_infos = [int(x) for x in args.raid_quest_infos.split(",") if x]
    args.profiles = {}
    for spec in args.profile:
        c, n = spec.split("=")
        args.profiles[int(c)] = int(n)
    return args


def main(argv):
    args = parse_args(argv)
    if args.offline_json:
        with open(args.offline_json) as f:
            data = json.load(f)
    else:
        data = load_from_db(args)
    report = write_lists(data, args)
    for class_id, r in sorted(report.items()):
        print("%-8s %3d items  pvp=%d epic=%d  %s" % (CLASS_NAMES[class_id], r["items"], r["pvp"], r["epic"],
                                                      " ".join("%s=%d" % kv for kv in sorted(r["slots"].items()))))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
