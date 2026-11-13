#!/usr/bin/env python3
"""Tests for tools/gen_prebis.py on a small fixture (tests/data/prebis-fixture.json) and a fake mysql command. No database needed.
Usage: gen_prebis_test.py <module source dir>"""
import os
import subprocess
import sys
import tempfile

root = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(root, "tools"))
import gen_prebis as g  # noqa: E402

FIXTURE = os.path.join(root, "tests", "data", "prebis-fixture.json")
failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("FAIL:", what)


def ids_of(path):
    return [int(l) for l in open(path).read().splitlines() if l and not l.startswith("#")]


def run(out, *extra):
    done = subprocess.run([sys.executable, "-I", os.path.join(root, "tools", "gen_prebis.py"), "--offline-json", FIXTURE, "--out", out, *extra],
                          capture_output=True, text=True)
    check(done.returncode == 0, "generator exits 0: " + done.stderr)
    return done.stdout


out_dir = tempfile.mkdtemp()
run(out_dir)


def ids_text(text):
    return [l for l in text.splitlines() if l and not l.startswith("#")]


with tempfile.TemporaryDirectory() as out:
    run(out)
    rogue = ids_of(os.path.join(out, "4.txt"))
    mage = ids_of(os.path.join(out, "8.txt"))
    check(sorted(os.listdir(out)) == ["1.txt", "11.txt", "2.txt", "3.txt", "4.txt", "5.txt", "7.txt", "8.txt", "9.txt"], "one file for each of the nine classes")
    # armour: the better stat fit wins among equal item levels, leather beats a cloth robe of a higher level, plate is too heavy
    check(1002 in rogue and 1001 not in rogue, "rogue takes the leather chest with the better stats")
    check(1003 not in rogue and 1004 not in rogue, "no cloth or plate chest for the rogue while a leather one exists")
    # excluded by attribute
    check(1005 not in rogue, "test item (name starts with its item level) excluded")
    check(1006 not in rogue, "raid loot excluded")
    check(1007 not in rogue, "quest-bound excluded")
    check(1008 not in rogue, "placeholder name excluded")
    check(1011 not in rogue, "item with no source excluded")
    check(1012 not in rogue and 1013 not in rogue, "item level and required level limits")
    check(1014 not in rogue, "class mask of another class excluded")
    check(1021 not in rogue, "profession-gated item excluded")
    # epics stay in (owner decision) and rings come in twos
    check(1009 in rogue and 1010 in rogue and 1015 not in rogue, "two rings, epic included, third ring cut")
    check(1027 in rogue and 1025 in rogue and 1002 in rogue, "a PvP piece where it is the only fit, crafted and dungeon sources kept")
    check(1026 in rogue and 1016 not in rogue, "PvE first: the lower level dungeon gloves beat the PvP gloves")
    check(1019 in rogue and 1033 not in rogue and 1020 not in rogue and 1030 not in rogue and 1036 not in rogue, "rogue: one-handers that carry agility (the slow mace has no core stat), no two-hander")
    check(1022 not in rogue, "no shield for a rogue")
    # a cloth class: the robe with the intellect, not the agility one; a cloth class takes the caster stat robe
    check(1017 in mage and 1018 not in mage and 1003 not in mage, "mage robe picked by spell power and Intellect, not by item level or agility")
    check(1020 not in mage, "mage cannot use a two-hand mace")
    check(1030 in mage and 1019 not in mage and 1031 not in mage, "mage: the staff set beats dagger plus held item when its score is higher, never both")
    text = open(os.path.join(out, "4.txt")).read()
    check("# slot=chest ilvl=62 quality=3 source=dungeon Dungeon Leather Tunic Better Stats\n1002\n" in text, "comment line above the entry names slot, ilvl, quality, source")
    # no pvp / epics off
    run(out, "--no-pvp", "--max-quality", "3")
    rogue = ids_of(os.path.join(out, "4.txt"))
    check(1016 not in rogue and 1009 not in rogue, "--no-pvp and --max-quality 3 drop the pvp glove and the epic ring")
    run(out, "--keep-unsourced")
    check(1011 in ids_of(os.path.join(out, "4.txt")), "--keep-unsourced keeps the item without a source")
    # the same run twice writes the same files
    run(out)
    first = open(os.path.join(out, "4.txt")).read()
    run(out)
    check(first == open(os.path.join(out, "4.txt")).read(), "output is deterministic")

# profiles: the roster's classes get the role of the roster, and the weighted score ranks by stat fit
check(g.DEFAULT_PROFILE[1] == "tank" and g.DEFAULT_PROFILE[5] == "healer" and g.DEFAULT_PROFILE[8] == "caster" and g.DEFAULT_PROFILE[9] == "caster"
      and g.DEFAULT_PROFILE[4] == "rogue" and g.DEFAULT_PROFILE[3] == "hunter", "profile per class")
item = {"itemLevel": "60", "stats": "3:10,5:4,7:5,38:6", "armor": "100", "dmgMin": "10", "dmgMax": "30", "delay": "2000", "inventoryType": "15"}
check(abs(g.stat_score(item, g.PROFILES["hunter"]) - (2.2 * 10 + 0 * 4 + 0.4 * 5 + 6 + 3 * 10)) < 0.01, "stat score is the weighted sum plus weapon dps")
check(g.stat_score({"stats": "7:10", "armor": "200", "itemLevel": "60"}, g.PROFILES["tank"]) == 20 + 10, "tank counts stamina and armour")
# hunter takes the attack power helm over the stamina helm of a higher level; the tank warrior takes the stamina one
text_h = open(os.path.join(out_dir, "3.txt")).read()
check("1029" in ids_text(text_h) and "1028" not in ids_text(text_h), "hunter profile: agility, attack power and hit over stamina")
check("1028" in ids_text(open(os.path.join(out_dir, "1.txt")).read()), "tank profile: stamina first")
check(1032 in ids_of(os.path.join(out_dir, "3.txt")) and 1033 not in ids_of(os.path.join(out_dir, "3.txt")), "hunter: one two-hander, no one-hander next to it")
check(1033 in ids_of(os.path.join(out_dir, "1.txt")) and 1022 in ids_of(os.path.join(out_dir, "1.txt")) and 1020 not in ids_of(os.path.join(out_dir, "1.txt")),
      "tank warrior: one-hander and shield, no two-hander")
# fit rules: a hunter two-hander with no agility or attack power loses to one with agility, whatever its damage or crit; AP/Agi are junk on casters; effects are scored by name
check(1032 in ids_of(os.path.join(out_dir, "3.txt")) and 1036 not in ids_of(os.path.join(out_dir, "3.txt")), "hunter weapon carries agility (core stat rule)")
check(g.fit_of({"stats": "6:10,7:19,32:28"}, "hunter")[1] == 0 and g.fit_of({"stats": "3:5"}, "hunter")[1] == 5, "core amount counts agility for the hunter, not intellect")
check(g.stat_score({"stats": "38:16,32:14,31:10,7:11"}, g.PROFILES["caster"]) < 0 < g.stat_score({"stats": "38:16,32:14,31:10,7:11"}, g.PROFILES["rogue"]),
      "attack power on a ring: negative for the caster, positive for the rogue")
check(g.stat_score({"stats": "5:10,45:20"}, g.PROFILES["hunter"]) <= 0 and g.stat_score({"stats": "5:10,6:5,45:20"}, g.PROFILES["tank"]) < 0, "spell stats count nothing for hunter and tank")
mage_out = ids_of(os.path.join(out_dir, "8.txt"))
check(1038 in mage_out and (1037 not in mage_out or mage_out.index(1038) < mage_out.index(1037)), "mage ranks the spell ring over the attack power ring")
check(1034 in ids_of(os.path.join(out_dir, "4.txt")) and 1034 not in mage_out and 1039 in mage_out and 1039 not in ids_of(os.path.join(out_dir, "4.txt")),
      "trinkets by effect name: Earthstrike for the rogue, Ephemeral Power for the mage")
check(1035 in ids_of(os.path.join(out_dir, "4.txt")), "a trinket with stats fills the second trinket slot")
# loot references are followed, nested and cyclic
refs = {10: [(0, 100), (1, 11)], 11: [(0, 101), (1, 10)]}
check(g.resolve_loot([{"ItemType": "1", "Item": "10"}, {"ItemType": "0", "Item": "5"}], refs) == {100, 101, 5}, "reference loot resolved recursively")
# quest chains: a raid quest (flag 0x40, raid zone, raid kind, group size) takes the quests that lead to it and its exclusive-group siblings with it
def quest(i, **kw):
    q = {"ID": str(i), "QuestSortID": "1", "QuestInfoID": "0", "SuggestedGroupNum": "0", "Flags": "0", "Prev": "0", "Grp": "0"}
    q.update({k: str(v) for k, v in kw.items()})
    for c in ("R1", "R2"):
        q.setdefault(c, "0")
    return q


chain = [quest(1, R1=101), quest(2, Prev=1, R1=102), quest(3, Prev=2, Flags=64, R1=103),          # 3 is flagged, 2 and 1 lead to it
         quest(4, Grp=9, R1=104), quest(5, Grp=9, Flags=64, R1=105),                              # 4 shares the group of the flagged 5
         quest(6, R1=106), quest(7, QuestSortID=19, R1=107), quest(8, SuggestedGroupNum=10, R1=108), quest(9, QuestInfoID=62, R1=109),
         quest(10, Prev=-6, R1=110)]                                                              # a quest after a plain one: both stay
check(g.quest_reward_items(chain, [19], [62], ["R1", "R2"]) == {106, 110}, "raid quest chains are not a source of items")
with tempfile.TemporaryDirectory() as both:
    run(both, "--also-nopvp", os.path.join(both, "np"))
    check(os.path.isfile(os.path.join(both, "np", "4.txt")) and 1027 not in ids_of(os.path.join(both, "np", "4.txt")) and 1027 in ids_of(os.path.join(both, "4.txt")),
          "--also-nopvp writes the second variant without PvP pieces")

# the mysql side: only SELECT, the statement goes in on stdin
with tempfile.TemporaryDirectory() as tmp:
    fake = os.path.join(tmp, "fakesql.py")
    with open(fake, "w") as f:
        f.write("import sys\nsql = sys.stdin.read()\nprint('a\\tb')\nprint('1\\tNULL')\nprint('x y\\t' + str(len(sql)))\n")
    db = g.Db("%s %s" % (sys.executable, fake))
    rows = db.rows("SELECT 1")
    check(rows == [{"a": "1", "b": None}, {"a": "x y", "b": "8"}], "tab separated rows, NULL is None")
    try:
        db.rows("DELETE FROM armory_item")
        check(False, "a non-SELECT statement is refused")
    except ValueError:
        pass

if failures:
    print("%d failure(s)" % failures)
    sys.exit(1)
print("gen_prebis: ok")
