#!/usr/bin/env python3
"""Generate the PvP gear wishlists (data/pvp/<classId>.txt, <classId>-alliance.txt, <classId>-horde.txt) from the world database.

Same file format as data/prebis/<classId>.txt (one item entry per line, a comment line above each entry), so the module can load a list the same way
(doc/pvp-track.md). Where gen_prebis.py picks the best pre-raid piece per slot, this picks the best PvP piece per slot and per honor-rank band, best rank first.

A PvP piece is an item that is any of
  - rank gated: the hotfix item_sparse.RequiredPVPRank (raw rank - 4 = honor rank R1..R14), or, for the classic items that carry no hotfix row, a rank name
    in the item name ("Knight-Captain's", "Blood Guard's", "High Warlord's", ... NAME_RANK);
  - sold by a battleground reputation officer (creature subtitle "<faction> Supply Officer": Alterac Valley, Warsong Gulch, Arathi Basin);
  - sold by an honor quartermaster (creature subtitle "... Quartermaster").
The side (Alliance or Horde) comes from the rank name or from the reputation officer's faction; everything else is usable by both. Selection is by rules only
(quality, required level, class usability, stat profile, required rank), never by item entry. Item stats weights: gen_prebis.PROFILES with Stamina raised
(PvP: survive first) and a weight for Resilience. Reads only (every statement is a SELECT). Python 3 standard library only.

    tools/gen_pvp.py --mysql "ssh forever-test sudo -n mysql forever_test_world" --out data/pvp
    tools/gen_pvp.py --offline-json tests/data/pvp-fixture.json --out /tmp/pvp
"""
import argparse
import json
import os
import re
import sys
from types import SimpleNamespace

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_prebis as g  # noqa: E402

RESILIENCE = 35
HONOR_RANK_OFFSET = 4   # item_sparse.RequiredPVPRank is the honor rank + 4 (Private = 5, Grand Marshal / High Warlord = 18)

# Rank name -> (honor rank R1..R14, side). The names are the item name prefixes of the classic honor sets ("Knight-Captain's Chain Hauberk").
NAME_RANK = {
    "Private": (1, "alliance"), "Scout": (1, "horde"), "Corporal": (2, "alliance"), "Sergeant": (3, "both"), "Master Sergeant": (4, "alliance"),
    "Sergeant Major": (5, "alliance"), "Knight": (6, "alliance"), "Knight-Lieutenant": (7, "alliance"), "Knight-Captain": (8, "alliance"),
    "Knight-Champion": (9, "alliance"), "Lieutenant Commander": (10, "alliance"), "Commander": (11, "alliance"), "Marshal": (12, "alliance"),
    "Field Marshal": (13, "alliance"), "Grand Marshal": (14, "alliance"),
    "Grunt": (2, "horde"), "Senior Sergeant": (4, "horde"), "First Sergeant": (5, "horde"),
    "Stone Guard": (6, "horde"), "Blood Guard": (7, "horde"), "Legionnaire": (8, "horde"), "Centurion": (9, "horde"), "Champion": (10, "horde"),
    "Lieutenant General": (11, "horde"), "General": (12, "horde"), "Warlord": (13, "horde"), "High Warlord": (14, "horde"),
}
NAME_RANK_RE = re.compile("^(" + "|".join(sorted((re.escape(n) for n in NAME_RANK), key=len, reverse=True)) + ").s ")
# Reputation officers of the battlegrounds: subtitle keyword -> side.
BG_FACTION_SIDE = (("Defilers", "horde"), ("Frostwolf", "horde"), ("Warsong", "horde"),
                   ("League of Arathor", "alliance"), ("Stormpike", "alliance"), ("Silverwing", "alliance"))
BG_VENDOR_RE = re.compile(r"Supply Officer", re.I)
HONOR_VENDOR_RE = re.compile(r"Quartermaster", re.I)

# The class profiles for PvP: the damage or healing role a bot of that class plays in a battleground (warrior arms/fury, paladin retribution, hunter, rogue,
# priest, shaman and druid healers, mage and warlock casters). --profile CLASS=NAME changes any of them.
DEFAULT_PVP_PROFILE = {1: "warrior-dps", 2: "ret-paladin", 3: "hunter", 4: "rogue", 5: "healer", 7: "healer", 8: "caster", 9: "caster", 11: "healer"}
# Weapons: a PvP warrior fights with a two-hander or two one-handers (arms, fury), a retribution paladin with a two-hander, not with the shield of the tank
# and healer sets gen_prebis hands out.
g.WEAPON_MODE = {**g.WEAPON_MODE, 1: "melee", 2: "twohand"}
PVP_STAMINA = 1.0       # the lowest Stamina weight of a PvP profile: Forever's PvP variants carry more Stamina, and a battleground is a damage race
PVP_RESILIENCE = 1.0
# Rank bands: the wishlist holds, per slot, the best piece usable at each band's rank cap. 0 = no rank needed (reputation and honor vendors).
DEFAULT_BANDS = (0, 8, 10, 12, 14)
BAND_WEAPON_SLOTS = ("weapon2h", "mainhand", "offhand", "shield", "holdable")
SLOT_ORDER = ("head", "neck", "shoulder", "cloak", "chest", "wrist", "hands", "waist", "legs", "feet", "finger", "trinket",
              "weapon2h", "mainhand", "offhand", "shield", "holdable", "ranged", "relic")
SIDES = ("alliance", "horde")


def register_profiles():
    """Add 'pvp-<name>' copies of the gen_prebis profiles with the PvP weights."""
    for name in list(g.PROFILES):
        if name.startswith("pvp-"):
            continue
        w = dict(g.PROFILES[name])
        w[g.STAMINA] = max(w.get(g.STAMINA, 0), PVP_STAMINA)
        w[RESILIENCE] = PVP_RESILIENCE
        g.PROFILES["pvp-" + name] = w
        g.CORE["pvp-" + name] = g.CORE[name]
        g.EFFECT_FIT["pvp-" + name] = g.EFFECT_FIT[name]


register_profiles()


# ---- classification (rules on the data, never on an item entry) ----

def classify(item, hotfix_rank, vendors):
    """Annotate an item with _rank (honor rank, 0 = none), _side and _src. `hotfix_rank` is item_sparse.RequiredPVPRank or 0; `vendors` the subtitles of the
    vendors selling the item. Returns False when the item is no PvP piece."""
    rank, side, src = 0, "both", None
    if hotfix_rank > 0:
        rank, src = max(hotfix_rank - HONOR_RANK_OFFSET, 1), "rank"
    named = NAME_RANK_RE.match(item["name"])
    if named:
        n_rank, n_side = NAME_RANK[named.group(1)]
        side = n_side
        if not rank:
            rank, src = n_rank, "rank"
    for subtitle in vendors:
        if BG_VENDOR_RE.search(subtitle):
            for key, vendor_side in BG_FACTION_SIDE:
                if key.lower() in subtitle.lower():
                    side = vendor_side if side == "both" else side
            src = src or "bg-rep"
        elif HONOR_VENDOR_RE.search(subtitle):
            src = src or "honor-vendor"
    if src is None:
        return False
    item["_rank"], item["_side"], item["_src"] = rank, side, src
    return True


def build_items(data):
    """The candidate items of the JSON data (items, pvp_rank {id: raw rank}, vendors {id: [subtitle, ...]}), classified; non PvP items drop out."""
    ranks, vendors = data.get("pvp_rank", {}), data.get("vendors", {})
    out = []
    for item in data["items"]:
        item = dict(item)
        if classify(item, int(ranks.get(str(item["id"]), 0)), vendors.get(str(item["id"]), [])):
            out.append(item)
    return out


# ---- the database side ----

NAME_RE_SQL = "^(" + "|".join(sorted(NAME_RANK, key=len, reverse=True)) + ").s "


def load_from_db(args):
    db = g.Db(args.mysql)
    vendors = {}
    for r in db.rows("SELECT DISTINCT v.item, t.subname FROM npc_vendor v JOIN creature_template t ON t.entry = v.entry "
                     "WHERE v.item > 0 AND (t.subname LIKE '%Supply Officer%' OR t.subname LIKE '%Quartermaster%')"):
        vendors.setdefault(r["item"], []).append(r["subname"])
    ranks = {r["id"]: int(r["pvp_rank"]) for r in db.rows("SELECT ID AS id, RequiredPVPRank AS pvp_rank FROM %s.item_sparse WHERE RequiredPVPRank > 0" % args.hotfix_db)}
    rows = db.rows(
        "SELECT id, name, quality, itemLevel, requiredLevel, class, subclass, inventoryType, bonding, armor, dmgMin, dmgMax, delay, stats, effects, allowableClass, requiredSkill, requiredSkillRank "
        "FROM armory_item WHERE class IN (2, 4) AND quality IN (%s) AND itemLevel BETWEEN %d AND %d AND requiredLevel <= %d AND inventoryType <> 0 "
        "AND (name REGEXP '%s' OR id IN (%s) OR id IN (%s))"
        % (g.idlist(range(args.min_quality, args.max_quality + 1)), args.min_ilvl, args.max_ilvl, args.level, NAME_RE_SQL.replace("'", "''"),
           g.idlist(int(i) for i in vendors), g.idlist(int(i) for i in ranks)))
    return {"items": rows, "pvp_rank": ranks, "vendors": vendors}


# ---- the selection ----

def select_band(class_id, items, args, profile, side, cap):
    """The best pieces per slot (gen_prebis.select_for_class) among the PvP items of `side` (or both sides) that need at most honor rank `cap`."""
    pool = [i for i in items if i["_side"] in (side, "both") and i["_rank"] <= cap]
    sources = {k: set() for k in g.SOURCE_ORDER}
    sources["pvp"] = {int(i["id"]) for i in pool}
    return g.select_for_class(class_id, pool, sources, set(), args, profile)


def side_list(class_id, items, args, profile, side, bands):
    """[(slot, score, band, item)], best band first inside a slot. A lower band keeps its weapons only when they are the same kind (two-hander or one-hander
    set) as the top band's, because the equip service wears a consistent set (gen_prebis WEAPON_MODE)."""
    per_band = [(b, select_band(class_id, items, args, profile, side, b)) for b in bands]

    def kind(chosen):
        return "2h" if any(s == "weapon2h" for s, _ in chosen) else "1h"
    top_kind = kind(per_band[-1][1])
    seen, out = set(), []
    for band, chosen in reversed(per_band):
        for slot, (score, quality, item, tag) in chosen:
            if slot in BAND_WEAPON_SLOTS and kind(chosen) != top_kind:
                continue
            if item["id"] in seen:
                continue
            seen.add(item["id"])
            out.append((slot, score, band, item))
    out.sort(key=lambda e: (SLOT_ORDER.index(e[0]) if e[0] in SLOT_ORDER else 99, -e[2], -e[1]))
    return out


def merge_sides(per_side):
    """Both factions in one list: every non-weapon entry of both sides, and the weapon entries of the side whose weapon set scores higher."""
    def weapon_score(entries):
        return sum(s for slot, s, b, _ in entries if slot in BAND_WEAPON_SLOTS and b == max((x[2] for x in entries), default=0))
    best = max(SIDES, key=lambda s: weapon_score(per_side[s]))
    seen, out = set(), []
    for side in SIDES:
        for e in per_side[side]:
            if e[0] in BAND_WEAPON_SLOTS and side != best:
                continue
            if e[3]["id"] not in seen:
                seen.add(e[3]["id"])
                out.append(e)
    out.sort(key=lambda e: (SLOT_ORDER.index(e[0]) if e[0] in SLOT_ORDER else 99, -e[2], -e[1]))
    return out


def write_file(path, header, entries):
    lines = list(header)
    for slot, score, band, item in entries:
        lines.append("# slot=%s ilvl=%s quality=%s source=%s rank=R%d side=%s %s" % (slot, item["itemLevel"], item["quality"], item["_src"], item["_rank"], item["_side"], item["name"]))
        lines.append(str(int(item["id"])))
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def write_lists(data, args):
    items = build_items(data)
    os.makedirs(args.out, exist_ok=True)
    report = {}
    for class_id, class_name in sorted(g.CLASS_NAMES.items()):
        profile = "pvp-" + args.profiles.get(class_id, DEFAULT_PVP_PROFILE[class_id])
        per_side = {s: side_list(class_id, items, args, profile, s, args.bands) for s in SIDES}
        header = ["# PvP gear wishlist for %s (class %d), generated by tools/gen_pvp.py; do not edit by hand." % (class_name, class_id),
                  "# Rules: PvP items (honor rank gated, battleground reputation vendors, honor quartermasters), quality %d-%d, item level %d-%d, required level <= %d, no test items,"
                  % (args.min_quality, args.max_quality, args.min_ilvl, args.max_ilvl, args.level),
                  "# usable by the class (armour type, weapon skill, class mask), best by PvP stat profile '%s' score per slot and per honor-rank band %s, best rank first." % (profile, ",".join(str(b) for b in args.bands)),
                  "# One item entry per line; the comment line above an entry gives: slot, item level, quality, source, honor rank needed (R0 none), side, name."]
        merged = merge_sides(per_side)
        write_file(os.path.join(args.out, "%d.txt" % class_id), header + ["# Both factions (weapons of the higher scoring side only); a faction bot uses %d-alliance.txt or %d-horde.txt." % (class_id, class_id)], merged)
        for side in SIDES:
            write_file(os.path.join(args.out, "%d-%s.txt" % (class_id, side)), header + ["# %s side." % side.capitalize()], per_side[side])
        report[class_id] = {"profile": profile, "all": len(merged), **{s: len(per_side[s]) for s in SIDES},
                            "top_rank": {s: max((e[3]["_rank"] for e in per_side[s]), default=0) for s in SIDES}}
    return report


def parse_args(argv):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--mysql", help='mysql command that reads the world database, e.g. "ssh forever-test sudo -n mysql forever_test_world"')
    src.add_argument("--offline-json", help="read the item, rank and vendor tables from a JSON file instead of a database")
    p.add_argument("--out", default="data/pvp", help="folder for the lists (default data/pvp)")
    p.add_argument("--dump-json", metavar="FILE", help="with --mysql: also write the data read from the database to FILE (a fixture)")
    p.add_argument("--hotfix-db", default="forever_test_hotfixes")
    p.add_argument("--level", type=int, default=60)
    p.add_argument("--min-ilvl", type=int, default=58)
    p.add_argument("--max-ilvl", type=int, default=90)
    p.add_argument("--min-quality", type=int, default=3)
    p.add_argument("--max-quality", type=int, default=4)
    p.add_argument("--bands", default=",".join(str(b) for b in DEFAULT_BANDS), help="honor rank caps of the bands, ascending (default %s)" % ",".join(str(b) for b in DEFAULT_BANDS))
    p.add_argument("--profile", action="append", default=[], metavar="CLASS=NAME", help="stat profile of a class (a gen_prebis profile name; default DEFAULT_PVP_PROFILE)")
    args = p.parse_args(argv)
    args.bands = sorted({int(x) for x in args.bands.split(",") if x != ""})
    args.profiles = {}
    for spec in args.profile:
        c, n = spec.split("=")
        if n not in g.PROFILES or n.startswith("pvp-"):
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
        if args.dump_json:
            with open(args.dump_json, "w") as f:
                json.dump(data, f)
    # the selection reads these like gen_prebis does
    sel = SimpleNamespace(min_quality=args.min_quality, max_quality=args.max_quality, min_ilvl=args.min_ilvl, max_ilvl=args.max_ilvl, level=args.level,
                          keep_unsourced=False, no_pvp=False, per_slot=1, audit=False, out=args.out, bands=args.bands, profiles=args.profiles)
    report = write_lists(data, sel)
    for class_id, r in sorted(report.items()):
        print("%-8s %-18s all=%3d alliance=%3d (top R%d) horde=%3d (top R%d)" % (g.CLASS_NAMES[class_id], r["profile"], r["all"], r["alliance"],
                                                                         r["top_rank"]["alliance"], r["horde"], r["top_rank"]["horde"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
