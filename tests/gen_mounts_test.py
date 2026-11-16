#!/usr/bin/env python3
"""Tests for tools/gen_mounts.py on a small fixture (tests/data/mounts-fixture.json). No database needed.
Usage: gen_mounts_test.py <module source dir>"""
import os
import subprocess
import sys
import tempfile

root = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIXTURE = os.path.join(root, "tests", "data", "mounts-fixture.json")
failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("FAIL:", what)


def lines(path):
    with open(path, encoding="utf-8") as fh:
        return [l.rstrip("\n") for l in fh if l.strip() and not l.startswith("#")]


with tempfile.TemporaryDirectory() as out:
    done = subprocess.run([sys.executable, "-I", os.path.join(root, "tools", "gen_mounts.py"), "--offline-json", FIXTURE, "--out-dir", out], capture_output=True, text=True)
    check(done.returncode == 0, "generator exits 0: " + done.stderr)
    check("Pet Merchant" in done.stdout, "a vendor with no race in its subname is reported")
    v = lines(os.path.join(out, "mount_vendors.txt"))
    check("vendor|4|0|1261|20004152|-5537.0 -1323.7 399.2|Veron Amberstill|Ram Breeder" in v, "ram vendor is the Dwarf one: %s" % v)
    check("vendor|16|0|4731|20028472|2250.4 328.1 36.9|Zachariah Post|Undead Horse Merchant" in v, "undead horse merchant is Undead, not Human")
    check("item|1261|5864|40|75|200000|Gray Ram" in v and "item|1261|13328|60|150|2000000|Black Ram" in v, "skill follows the level")
    check(not any("Odd Seller" in l for l in v), "unknown vendor left out")
    t = lines(os.path.join(out, "riding_trainers.txt"))
    check(len(t) == 2 and t[0].startswith("1|Ironforge|0|4772|762|") and t[1].startswith("2|Undercity|0|4773|762|"), "trainer side and city: %s" % t)
    r = lines(os.path.join(out, "riding_ranks.txt"))
    check(r == ["33388|40|75|900000|Apprentice Riding", "33391|60|150|9000000|Journeyman Riding"], "ranks sorted by level with skill 75 and 150: %s" % r)

sys.exit(1 if failures else 0)
