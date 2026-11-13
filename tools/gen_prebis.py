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

# ITEM_MOD_* numbers as they appear in armory_item.stats ("type:amount,..."), checked against ItemModType in the core (ItemTemplate.h) and the stats strings of
# the 2,000-odd weapons and armour of ilvl 58-70 (doc/pre-raid-gear.md has the table).
AGILITY, STRENGTH, INTELLECT, SPIRIT, STAMINA = 3, 4, 5, 6, 7
DEFENSE, DODGE, PARRY, BLOCK = 12, 13, 14, 15
HIT_MELEE, HIT_RANGED, HIT_SPELL, CRIT_MELEE, CRIT_RANGED, CRIT_SPELL = 16, 17, 18, 19, 20, 21
HIT, CRIT, ATTACK_POWER, RANGED_ATTACK_POWER = 31, 32, 38, 39
SPELL_HEALING, SPELL_DAMAGE, MANA_REGEN, SPELL_POWER, BLOCK_VALUE, EXTRA_ARMOR = 41, 42, 43, 45, 48, 50
PRIMARY = (AGILITY, STRENGTH, INTELLECT, SPIRIT)

# A stat profile: a weight per point of each item stat, plus ARMOR (per point of the item's own armour value) and DPS (per point of weapon damage per second;
# a wand counts as a weapon). A stat a profile does not name counts nothing; a negative weight counts against it. Ranking inside a slot is the weighted sum first,
# then item level, quality (see select_for_class). The weights follow the classic stat values: a point of Agility or Strength is about one attack power (and
# Agility adds crit), +healing and +spell damage are worth one point each to the class that uses them, Intellect gives mana and spell crit.
ARMOR, DPS, RANGED_DPS = "armor", "dps", "ranged_dps"   # RANGED_DPS: weapon damage per second of a bow, gun, crossbow, thrown weapon or wand (default: DPS)
PHYS_JUNK = {INTELLECT: -1, SPIRIT: -1, SPELL_POWER: -1, SPELL_DAMAGE: -1, SPELL_HEALING: -1, MANA_REGEN: -0.5}   # spell stats mean nothing to a physical profile
SPELL_JUNK = {STRENGTH: -1, AGILITY: -1, ATTACK_POWER: -2, RANGED_ATTACK_POWER: -2}                                # attack stats mean nothing to a caster or healer
PROFILES = {
    # shield tank (the sim's warriors): stamina, armour, defence, avoidance and block first; strength for threat and block value
    "tank": {STAMINA: 2, DEFENSE: 4, DODGE: 3, PARRY: 3, BLOCK: 2, BLOCK_VALUE: 0.5, STRENGTH: 1.2, AGILITY: 0.8, ATTACK_POWER: 0.4, HIT: 1, HIT_MELEE: 1, CRIT: 0.5,
             EXTRA_ARMOR: 0.05, ARMOR: 0.05, DPS: 3, **PHYS_JUNK},
    "warrior-dps": {STRENGTH: 2, AGILITY: 1.5, ATTACK_POWER: 1, CRIT: 2, CRIT_MELEE: 2, HIT: 2, HIT_MELEE: 2, STAMINA: 0.4, DPS: 3, **PHYS_JUNK},
    "rogue": {AGILITY: 2.2, STRENGTH: 1, ATTACK_POWER: 1, CRIT: 2, CRIT_MELEE: 2, HIT: 2, HIT_MELEE: 2, STAMINA: 0.4, DPS: 3, **PHYS_JUNK},
    # hunter: Agility (attack power, ranged attack power and crit), Strength as attack power, ranged damage per second; Intellect, Spirit and spell power count nothing
    "hunter": {AGILITY: 2.2, ATTACK_POWER: 1, RANGED_ATTACK_POWER: 1, STRENGTH: 1, CRIT: 2, CRIT_RANGED: 2, CRIT_MELEE: 1, HIT: 2, HIT_RANGED: 2, HIT_MELEE: 1,
               STAMINA: 0.4, DPS: 1, RANGED_DPS: 3, INTELLECT: 0, SPIRIT: 0, SPELL_POWER: 0, SPELL_DAMAGE: 0, SPELL_HEALING: 0, MANA_REGEN: 0},
    "ret-paladin": {STRENGTH: 2, AGILITY: 0.8, ATTACK_POWER: 1, CRIT: 2, CRIT_MELEE: 2, HIT: 2, HIT_MELEE: 2, STAMINA: 0.4, INTELLECT: 0.3, DPS: 3},
    # healers (holy priest, holy paladin, restoration shaman and druid): +healing, Intellect, mana per five seconds and spell crit; Stamina last
    "healer": {SPELL_HEALING: 1.2, SPELL_POWER: 1.2, INTELLECT: 1.5, SPIRIT: 0.8, MANA_REGEN: 2.5, CRIT: 0.5, CRIT_SPELL: 0.5, STAMINA: 0.4, DPS: 0, RANGED_DPS: 0.5,
               SPELL_DAMAGE: 0, **SPELL_JUNK},
    # damage casters (mage, warlock): spell damage, hit, crit and Intellect; Stamina second-rate (the warlock's Life Tap is not scored)
    "caster": {SPELL_DAMAGE: 1.2, SPELL_POWER: 1.2, INTELLECT: 1, CRIT: 1, CRIT_SPELL: 1, HIT: 1, HIT_SPELL: 1, STAMINA: 0.4, SPIRIT: 0.1, MANA_REGEN: 0.3, DPS: 0, RANGED_DPS: 0.5,
               SPELL_HEALING: 0, **SPELL_JUNK},
}
# The stats that make a weapon fit a profile (a weapon with none of them loses to one that has them, whatever its damage or crit): the main stats of the role.
CORE = {
    "tank": {STRENGTH, AGILITY, ATTACK_POWER, STAMINA, DEFENSE, PARRY, DODGE, BLOCK},
    "warrior-dps": {STRENGTH, AGILITY, ATTACK_POWER},
    "rogue": {AGILITY, STRENGTH, ATTACK_POWER},
    "hunter": {AGILITY, STRENGTH, ATTACK_POWER, RANGED_ATTACK_POWER},
    "ret-paladin": {STRENGTH, AGILITY, ATTACK_POWER},
    "healer": {SPELL_HEALING, SPELL_POWER, INTELLECT, MANA_REGEN},
    "caster": {SPELL_DAMAGE, SPELL_POWER, INTELLECT},
}
# Trinket and other item effects: the database has no spell text (no spell description table; the hotfix spell_effect holds no row of these spells), only the effect name
# ("trigger:spellId:Name|..." in armory_item.effects). The names below are matched, never an item entry, and add a flat bonus to the stat score when the effect fits the
# profile. An effect not named here scores nothing, so a trinket with stats beats it.
EFFECT_BONUS = 40
PHYSICAL_EFFECTS = re.compile(r"Earthstrike|Heroism|Lightning Strike|Blood Fury", re.I)
CASTER_EFFECTS = re.compile(r"Ephemeral Power|Massive Destruction|Arcane Potency", re.I)
HEALER_EFFECTS = re.compile(r"Blue Dragon|Rapid Healing|Nimble Healing Touch", re.I)
EFFECT_FIT = {"tank": PHYSICAL_EFFECTS, "warrior-dps": PHYSICAL_EFFECTS, "rogue": PHYSICAL_EFFECTS, "hunter": PHYSICAL_EFFECTS, "ret-paladin": PHYSICAL_EFFECTS,
              "healer": re.compile(HEALER_EFFECTS.pattern + "|" + CASTER_EFFECTS.pattern, re.I), "caster": re.compile(CASTER_EFFECTS.pattern + "|Blue Dragon", re.I)}
# The profile each class's list is built for. ScenarioGear<classId> is keyed by class, so one profile per class. The roster of the sim's ZG, MC and Onyxia runs
# (tools/sim-overnight ZG_ROSTER, and the dungeon file roster 2:1,5:5,8:8,2:9,5:4) is warrior, priest, mage, warlock and rogue, four of each, the warrior as tank, the
# priest as healer, mage, warlock and rogue as damage. Hunter, paladin, shaman and druid are in no sim roster yet: hunter is damage, paladin, shaman and druid are
# healers (the raid planner's default for them). --profile CLASS=NAME changes any of these.
DEFAULT_PROFILE = {1: "tank", 2: "healer", 3: "hunter", 4: "rogue", 5: "healer", 7: "healer", 8: "caster", 9: "caster", 11: "healer"}
# How a class's weapons are chosen: "shield" one-hander and shield, "dual" two one-handers, "twohand" a two-hander, "melee" the better of a two-hander and two one-handers, "auto" the better of a two-hander and a
# one-hander with a held item. The equip service (EquipBagUpgrades) wears bag items best gear score first into the first slot that takes them and a shield or off-hand
# cannot follow a two-hander, so a list carries ONE consistent set, never both a two-hander and a one-hander.
WEAPON_MODE = {1: "shield", 2: "shield", 3: "melee", 4: "dual", 5: "auto", 7: "shield", 8: "auto", 9: "auto", 11: "auto"}

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
WEAPON_SLOTS = ("weapon1h", "weapon2h", "offhand", "shield", "holdable", "ranged", "relic")
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


def quest_reward_items(quests, raid_sorts, raid_infos, reward_cols):
    """The items rewarded by quests that are not raid-tier (see load_from_db). `quests` are rows with ID, QuestSortID, QuestInfoID, SuggestedGroupNum, Flags, Prev,
    Grp and the reward columns."""
    by_id = {int(q["ID"]): q for q in quests}
    raid = {i for i, q in by_id.items() if int(q["QuestSortID"]) in raid_sorts or int(q["QuestInfoID"]) in raid_infos
            or int(q["SuggestedGroupNum"]) >= 10 or (int(q["Flags"]) & 64)}
    groups = {}
    for i, q in by_id.items():
        if int(q["Grp"]) > 0:
            groups.setdefault(int(q["Grp"]), set()).add(i)
    queue = list(raid)
    while queue:
        i = queue.pop()
        q = by_id[i]
        linked = {abs(int(q["Prev"]))} | (groups.get(int(q["Grp"]), set()) if int(q["Grp"]) > 0 else set())
        for j in linked:
            if j in by_id and j not in raid:
                raid.add(j)
                queue.append(j)
    items = set()
    for i, q in by_id.items():
        if i not in raid:
            items |= {int(q[c]) for c in reward_cols if int(q[c]) > 0}
    return items


def load_from_db(args):
    db = Db(args.mysql)
    quality_list = idlist(range(args.min_quality, args.max_quality + 1))
    items = db.rows(
        "SELECT id, name, quality, itemLevel, requiredLevel, class, subclass, inventoryType, bonding, armor, dmgMin, dmgMax, delay, stats, effects, allowableClass, requiredSkill, requiredSkillRank "
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
    # Quest rewards, except those of raid quests. A raid quest is one in a raid zone (QuestSortID is an AreaTable id, --raid-quest-sorts), of a raid kind (QuestInfoID,
    # --raid-quest-infos), suggested for ten or more players, or with the raid-eligible flag (QuestFlags 0x40, QUEST_FLAGS_RAID: the Ahn'Qiraj gate event, the Scarab
    # Lord line and the other world-event chains carry it). The chain counts as a whole: a quest that leads on to a raid quest (PrevQuestID, walked backward) or shares its
    # positive ExclusiveGroup with one is raid-tier too, so the first steps of the AQ chains, which carry no flag, go with it. A rule on the quest, never on an item.
    reward_cols = ["RewardItem%d" % i for i in range(1, 5)] + ["RewardChoiceItemID%d" % i for i in range(1, 7)]
    quests = db.rows("SELECT q.ID, q.QuestSortID, q.QuestInfoID, q.SuggestedGroupNum, q.Flags, IFNULL(a.PrevQuestID, 0) AS Prev, IFNULL(a.ExclusiveGroup, 0) AS Grp, " +
                     ", ".join("q." + c for c in reward_cols) + " FROM quest_template q LEFT JOIN quest_template_addon a ON a.ID = q.ID")
    quest = quest_reward_items(quests, args.raid_quest_sorts, args.raid_quest_infos, reward_cols)
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


def weapon_dps(item):
    try:
        low, high, delay = float(item.get("dmgMin") or 0), float(item.get("dmgMax") or 0), float(item.get("delay") or 0)
    except ValueError:
        return 0.0
    return (low + high) / 2.0 / (delay / 1000.0) if delay > 0 else 0.0


def effect_names(item):
    """The names of an item's effects (armory_item.effects is 'trigger:spellId:Name|...')."""
    return [part.split(":", 2)[2] for part in (item.get("effects") or "").split("|") if part.count(":") >= 2]


def fit_of(item, profile):
    """(fit, core): fit is the sum of the profile's positive weights times the item's stats plus the bonus of a fitting effect, so an item whose stats the profile does
    not want has fit 0; core is the amount of the profile's core stats (CORE), for the weapon rule."""
    weights = PROFILES[profile]
    fit = core = 0
    for kind, amount in parse_stats(item["stats"]):
        if weights.get(kind, 0) > 0 and amount > 0:
            fit += weights[kind] * amount
        if kind in CORE[profile]:
            core += amount
    if any(EFFECT_FIT[profile].search(n) for n in effect_names(item)):
        fit += EFFECT_BONUS
    return fit, core


def stat_score(item, weights, profile=None):
    """The profile score of an item: the sum of weight * amount over its stats, plus the profile's weight of the item's armour value and weapon damage per second,
    plus the effect bonus (when a profile name is given)."""
    total = 0.0
    for kind, amount in parse_stats(item["stats"]):
        total += weights.get(kind, 0) * amount
    total += weights.get(ARMOR, 0) * int(float(item.get("armor") or 0))
    ranged = str(item.get("inventoryType")) in ("15", "25", "26")
    total += weights.get(RANGED_DPS if ranged else DPS, weights.get(DPS, 0)) * weapon_dps(item)
    if profile and any(EFFECT_FIT[profile].search(n) for n in effect_names(item)):
        total += EFFECT_BONUS
    return round(total, 3)


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
    weights = PROFILES[profile]
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
        inv = int(item["inventoryType"])
        slot = SLOT_OF[inv]
        fit, core = fit_of(item, profile)
        by_slot.setdefault(slot, []).append((stat_score(item, weights, profile), int(item["quality"]), item, tag, inv, fit, core))

    def ranked(cands, weapon=False):
        # Best first. Tier 0 is a PvE piece that fits the profile: a positive score and positive fit (useful stats; an item of stats the profile does not want, or one the
        # profile's junk weights pull below zero, only fills a slot with no fitting piece). PvP rank pieces come after (owner decision: PvE first). A weapon must also
        # carry a core stat of the profile (CORE) to be in tier 0. Inside a tier: the profile score, then item level, then quality; the name and entry only make the
        # order stable.
        def key(c):
            tier = 0 if (c[3] != "pvp" and c[0] > 0 and c[5] > 0 and (not weapon or c[6] > 0)) else 1
            return (tier, -c[0], -int(c[2]["itemLevel"]), -c[1], c[2]["name"], int(c[2]["id"]))
        return sorted(cands, key=key)

    chosen = []

    def add(slot, c):
        chosen.append((slot, c[:4]))
        weak = c[0] <= 0 or c[5] <= 0 or (slot in ("weapon1h", "weapon2h", "mainhand", "offhand") and c[6] <= 0)
        if weak and getattr(args, "audit", False):
            print("weak pick: %s %s slot=%s %s score=%s fit=%s core=%s (nothing better fits)" % (CLASS_NAMES[class_id], profile, slot, c[2]["name"], c[0], c[5], c[6]))

    for slot, cands in by_slot.items():
        if slot in WEAPON_SLOTS:
            continue
        if slot in BODY_SLOTS:
            # The core drops armour lighter than the class maximum when a maximum-type piece exists for the slot (BotGearRules::ChooseForSlot).
            top = [c for c in cands if int(c[2]["subclass"]) == MAX_ARMOR[class_id]]
            cands = top or cands
        for c in ranked(cands)[:2 if slot in PAIR_SLOTS else args.per_slot]:
            add(slot, c)

    # One consistent weapon set (see WEAPON_MODE): a main hand and what goes with it, a ranged weapon or relic.
    def best(slot, invs=None, exclude=None):
        pool = [c for c in by_slot.get(slot, []) if (invs is None or c[4] in invs) and (exclude is None or c[2]["id"] != exclude)]
        r = ranked(pool, weapon=slot in WEAPON_SLOTS and slot != "ranged")
        return r[0] if r else None

    mode = WEAPON_MODE[class_id]
    main = off = None
    two = best("weapon2h")
    one = best("weapon1h", (13, 21))

    def pair_off():
        o = best("weapon1h", (13,), one[2]["id"] if one else None) or best("offhand")
        if o and best("offhand") and best("offhand")[0] > o[0]:
            o = best("offhand")
        return o

    def set_key(*cs):
        # a set of weapons: every piece carrying a core stat first, then the higher total score
        cs = [c for c in cs if c]
        return (0 if cs and all(c[6] > 0 and c[0] > 0 for c in cs) else 1, -sum(c[0] for c in cs))

    if mode == "shield":
        main, off = one, best("shield") or best("holdable")
    elif mode == "dual":
        main, off = one, pair_off()
    elif mode == "twohand":
        main = two or one
    elif mode == "melee":    # two-hander or two one-handers, whichever has the better set
        o = pair_off() if one else None
        if two and (not one or set_key(two) <= set_key(one, o)):
            main = two
        else:
            main, off = one, o
    else:   # auto: the better of a two-hander and a one-hander with a held item
        held = best("holdable")
        if two and (not one or set_key(two) <= set_key(one, held)):
            main = two
        else:
            main, off = one, (held if one else None)
    for slot, c in ((("weapon2h" if main and main[4] == 17 else "mainhand"), main), (("shield" if off and off[4] == 14 else "holdable" if off and off[4] == 23 else "offhand"), off),
                    ("ranged", best("ranged")), ("relic", best("relic"))):
        if c:
            add(slot, c)
    order = {s: i for i, s in enumerate(("head", "neck", "shoulder", "cloak", "chest", "wrist", "hands", "waist", "legs", "feet", "finger", "trinket",
                                         "weapon2h", "mainhand", "offhand", "shield", "holdable", "ranged", "relic"))}
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
        profile = args.profiles.get(class_id, DEFAULT_PROFILE[class_id])
        chosen = select_for_class(class_id, items, sources, raid_loot, args, profile)
        lines = ["# Pre-raid gear for %s (class %d), generated by tools/gen_prebis.py; do not edit by hand." % (class_name, class_id),
                 "# Rules: quality %d-%d, item level %d-%d, required level <= %d, no quest-bound items, no test items, none dropped by raid creatures or chests (maps %s),"
                 % (args.min_quality, args.max_quality, args.min_ilvl, args.max_ilvl, args.level, ",".join(str(m) for m in args.raid_maps)) +
                 (" no PvP rank items," if args.no_pvp else ""),
                 "# usable by the class (armour type, weapon skill, class mask), best by stat profile '%s' score, then item level (tools/gen_prebis.py PROFILES)." % profile,
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
    p.add_argument("--audit", action="store_true", help="print every pick whose stats do not fit the class profile (no better candidate existed)")
    p.add_argument("--profile", action="append", default=[], metavar="CLASS=NAME", help="stat profile of a class list, one of %s (defaults: DEFAULT_PROFILE)" % ", ".join(sorted(PROFILES)))
    p.add_argument("--no-pvp", action="store_true", help="leave out PvP rank items")
    p.add_argument("--also-nopvp", metavar="DIR", help="after the main run, write a second set of lists without PvP rank items to DIR (data/prebis-nopvp)")
    p.add_argument("--keep-unsourced", action="store_true", help="keep items with no known source (default: dropped, they cannot be obtained)")
    args = p.parse_args(argv)
    args.raid_maps = [int(x) for x in args.raid_maps.split(",") if x]
    args.dungeon_maps = [int(x) for x in args.dungeon_maps.split(",") if x]
    args.raid_quest_sorts = [int(x) for x in args.raid_quest_sorts.split(",") if x]
    args.raid_quest_infos = [int(x) for x in args.raid_quest_infos.split(",") if x]
    args.profiles = {}
    for spec in args.profile:
        c, n = spec.split("=")
        if n not in PROFILES:
            raise SystemExit("unknown profile " + n)
        args.profiles[int(c)] = n
    return args


def main(argv):
    args = parse_args(argv)
    if args.offline_json:
        with open(args.offline_json) as f:
            data = json.load(f)
    else:
        data = load_from_db(args)
    report = write_lists(data, args)
    if args.also_nopvp:
        print("-- without PvP rank items: " + args.also_nopvp)
        args.out, args.no_pvp = args.also_nopvp, True
        report_nopvp = write_lists(data, args)
        print_report(report_nopvp)
        print("-- with PvP rank items")
    print_report(report)
    return 0


def print_report(report):
    for class_id, r in sorted(report.items()):
        print("%-8s %3d items  pvp=%d epic=%d  %s" % (CLASS_NAMES[class_id], r["items"], r["pvp"], r["epic"],
                                                      " ".join("%s=%d" % kv for kv in sorted(r["slots"].items()))))


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
