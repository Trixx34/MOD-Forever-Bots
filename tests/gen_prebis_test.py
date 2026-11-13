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
    check(1016 in rogue and 1025 in rogue and 1002 in rogue, "pvp, crafted and dungeon sources kept")
    check(1019 in rogue and 1023 in rogue and 1024 not in rogue and 1020 not in rogue, "rogue: two one-handers, no two-hander")
    check(1022 not in rogue, "no shield for a rogue")
    # a cloth class: the robe with the intellect, not the agility one; a cloth class takes the caster stat robe
    check(1017 in mage and 1018 not in mage and 1003 not in mage, "mage robe picked by stat fit (1017 intellect+spirit over agility)")
    check(1020 not in mage, "mage cannot use a two-hand mace")
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

# the warrior is a shield tank by default
check(g.preferred_stats(1, g.DEFAULT_ROLE_PROFILE[1]) == g.PROFILES[5], "warrior list uses the shield tank profile")
check(g.preferred_stats(4, 0) == (g.AGILITY, g.STRENGTH, g.STAMINA) and g.preferred_stats(8, 0) == g.CASTER_STATS, "class default stats")
# the score is the core's: item level * 10 + 3x a wanted primary, minus 3x an unwanted primary
item = {"itemLevel": "60", "stats": "3:10,5:4,7:5"}
check(g.stat_score(item, (g.AGILITY, g.STAMINA)) == 600 + 30 + 5 - 12, "stat score follows ModuleBotSupplyRules::StatScore")
# loot references are followed, nested and cyclic
refs = {10: [(0, 100), (1, 11)], 11: [(0, 101), (1, 10)]}
check(g.resolve_loot([{"ItemType": "1", "Item": "10"}, {"ItemType": "0", "Item": "5"}], refs) == {100, 101, 5}, "reference loot resolved recursively")
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
