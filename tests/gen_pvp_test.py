#!/usr/bin/env python3
"""Tests for tools/gen_pvp.py on a small fixture (tests/data/pvp-fixture.json). No database needed.
Usage: gen_pvp_test.py <module source dir>"""
import os
import subprocess
import sys
import tempfile

root = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(root, "tools"))
import gen_pvp as p  # noqa: E402

FIXTURE = os.path.join(root, "tests", "data", "pvp-fixture.json")
failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("FAIL:", what)


def ids_of(path):
    return [int(l) for l in open(path).read().splitlines() if l and not l.startswith("#")]


def run(out, *extra):
    done = subprocess.run([sys.executable, "-I", os.path.join(root, "tools", "gen_pvp.py"), "--offline-json", FIXTURE, "--out", out, *extra], capture_output=True, text=True)
    check(done.returncode == 0, "generator exits 0: " + done.stderr)
    return done.stdout


# classification: rank from the hotfix or the name, side from the name or the vendor
item = {"name": "Knight-Captain's Chain Hauberk"}
check(p.classify(item, 12, []) and item["_rank"] == 8 and item["_side"] == "alliance" and item["_src"] == "rank", "hotfix rank 12 is honor rank 8, the name gives the side")
item = {"name": "Blood Guard's Chain Greaves"}
check(p.classify(item, 0, []) and item["_rank"] == 7 and item["_side"] == "horde", "a classic item without hotfix row: rank and side from the name")
item = {"name": "Defiler's Epaulets"}
check(p.classify(item, 0, ["Defilers Supply Officer"]) and item["_rank"] == 0 and item["_side"] == "horde" and item["_src"] == "bg-rep", "reputation officer: faction gives the side")
item = {"name": "Silverwing Pendant"}
check(p.classify(item, 0, ["Silverwing Supply Officer"]) and item["_side"] == "alliance", "Silverwing is Alliance")
item = {"name": "Warsong Pendant"}
check(p.classify(item, 0, ["Warsong Supply Officer"]) and item["_side"] == "horde", "Warsong is Horde")
item = {"name": "Some Cloak"}
check(p.classify(item, 0, ["Accessories Quartermaster"]) and item["_side"] == "both" and item["_src"] == "honor-vendor", "quartermaster item is for both sides")
check(not p.classify({"name": "Dungeon Boots"}, 0, []), "an item with no PvP trait is not a PvP piece")

with tempfile.TemporaryDirectory() as out:
    run(out)
    names = sorted(os.listdir(out))
    check(len(names) == 27 and "4.txt" in names and "4-alliance.txt" in names and "8-horde.txt" in names, "three files for each of the nine classes: " + str(names))
    ra, rh, rb = ids_of(os.path.join(out, "4-alliance.txt")), ids_of(os.path.join(out, "4-horde.txt")), ids_of(os.path.join(out, "4.txt"))
    check(ra.index(2002) < ra.index(2001) < ra.index(2005), "alliance rogue chests: best rank first (R12, R8, then the reputation piece)")
    check(rh.index(2004) < rh.index(2003) < rh.index(2013), "horde rogue chests: best rank first")
    check(not (set(ra) & {2003, 2004, 2013}) and not (set(rh) & {2001, 2002, 2005}), "a side list holds no piece of the other side")
    check(set(rb) >= set(ra[:1]) | set(rh[:1]) and 2002 in rb and 2004 in rb, "the combined list holds both sides")
    check(2006 not in rb, "an item without PvP source is never listed")
    check(2007 not in rb, "plate is too heavy for a rogue")
    check(2010 not in rb, "item of another class mask excluded")
    check(2011 not in rb, "required level 70 excluded")
    check(2017 not in rb, "item level under the floor excluded")
    check(2012 in ra and 2012 in rh, "a both-sides quartermaster piece is in both side lists")
    check(2015 in rh and 2016 in ra and 2015 not in ra, "dagger per side")
    text = open(os.path.join(out, "4-alliance.txt")).read()
    check("# slot=chest ilvl=71 quality=4 source=rank rank=R12 side=alliance Marshal's Leather Tunic\n2002\n" in text, "comment line above the entry names slot, ilvl, quality, source, rank, side")
    check("source=bg-rep rank=R0 side=alliance Highlander's Leather Tunic\n2005\n" in text, "reputation vendor piece has rank R0")
    mage_a, mage_h = ids_of(os.path.join(out, "8-alliance.txt")), ids_of(os.path.join(out, "8-horde.txt"))
    check(2009 in mage_a and 2008 not in mage_a and 2008 in mage_h and 2009 not in mage_h, "mage: the cloth set of each side")
    warrior = ids_of(os.path.join(out, "1-alliance.txt"))
    check(2014 in warrior, "warrior takes the two-hander (PvP weapon mode)")
    first = {n: open(os.path.join(out, n)).read() for n in names}
    run(out)
    check(all(first[n] == open(os.path.join(out, n)).read() for n in names), "the same run twice writes the same files")

# a band cap of 0 leaves only the pieces that need no honor rank
with tempfile.TemporaryDirectory() as out:
    run(out, "--bands", "0")
    ra = ids_of(os.path.join(out, "4-alliance.txt"))
    check(2005 in ra and 2001 not in ra and 2002 not in ra, "--bands 0: reputation piece only, no rank piece")

if failures:
    print(failures, "failure(s)")
    sys.exit(1)
print("gen_pvp_test: ok")
