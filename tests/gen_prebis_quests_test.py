#!/usr/bin/env python3
"""Tests for tools/gen_prebis_quests.py on a small fixture (tests/data/prebis-quests). No database needed. Usage: gen_prebis_quests_test.py <module source dir>"""
import json
import os
import subprocess
import sys
import tempfile

root = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
fx = os.path.join(root, "tests", "data", "prebis-quests")
failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("FAIL:", what)


def run(out, fixture, prebis):
    done = subprocess.run([sys.executable, "-I", os.path.join(root, "tools", "gen_prebis_quests.py"), "--offline-json", fixture, "--prebis", prebis,
                           "--attunements", os.path.join(fx, "attunements.txt"),
                           "--pool-alliance", os.path.join(fx, "pool_leveling.txt"), "--pool-alliance", os.path.join(fx, "pool_60.txt"),
                           "--pool-horde", os.path.join(fx, "pool_leveling.txt"), "--out", out], capture_output=True, text=True)
    check(done.returncode == 0, "generator exits 0: " + done.stderr)
    return done.stdout


def quests(path):
    return [int(l.split()[1]) for l in open(path).read().splitlines() if l.startswith("quest ")]


def req(path, qid):
    for l in open(path).read().splitlines():
        if l.startswith("req %d " % qid):
            return dict(zip(l.split()[2::2], l.split()[3::2]))
    return None


with tempfile.TemporaryDirectory() as out:
    run(out, os.path.join(fx, "fixture.json"), os.path.join(fx, "prebis"))
    a = os.path.join(out, "prebis_quests_alliance_60.txt")
    h = os.path.join(out, "prebis_quests_horde_60.txt")
    qa = quests(a)
    check({100, 101, 102} <= set(qa), "gear quests and the chain step are in the list: %s" % qa)
    check(103 not in qa and 104 not in qa and 105 not in qa, "raid chain quest, unplaceable quest and a vendor item's quest are left out")
    check(set(qa) >= {200, 201, 202} and 205 not in qa, "attunement chain (RewardNextQuest and addon next walked back) is in, the final quest is not in the pool")
    check(req(a, 100) == {"classes": "129"}, "level dropped, classes = warrior (1) and mage (128): %s" % req(a, 100))
    check(req(a, 101) == {"prev": "100", "classes": "129"}, "prev kept: %s" % req(a, 101))
    check(req(a, 102) == {"classes": "128"}, "mage quest is for the mage only: %s" % req(a, 102))
    check(req(a, 202) == {"prev": "201", "classes": "5"}, "attunement keeps its own class mask, no class limit added: %s" % req(a, 202))
    check(req(a, 200) is None, "attunement quest has no class limit: %s" % req(a, 200))
    check(quests(h) == [100, 101, 102], "horde pool lists the gear quests and no alliance attunement: %s" % quests(h))
    text = open(os.path.join(out, "attunement_chains.txt")).read()
    check("chain test alliance 200 201 202 205" in text, "ordered chain: " + text)
    check("status test alliance 205 flagged raid-eligible" in text, "missing final quest is reported with a reason")
    check("chain other horde 210" in text and "other alliance" not in text, "a horde chain is not written for the alliance")
    report = open(os.path.join(out, "prebis_quests_report.txt")).read()
    check("missing alliance 205" in report, "report lists the missing attunement quest")
    check("missing alliance 104 Unplaceable no creature giver (starts from an item)" in report, "a wanted quest that is not placeable is reported with its reason")
    first = open(a).read()
    # selection never looks at the item number: renumber every item entry and the quests wanted stay the same
    shift = 7000000
    with open(os.path.join(fx, "fixture.json")) as f:
        data = json.load(f)
    for q in data["quests"]:
        q["rewards"] = [r + shift for r in q["rewards"]]
    fixture2 = os.path.join(out, "fixture2.json")
    json.dump(data, open(fixture2, "w"))
    prebis2 = os.path.join(out, "prebis2")
    os.mkdir(prebis2)
    for name in os.listdir(os.path.join(fx, "prebis")):
        lines = [l if l.startswith("#") else str(int(l) + shift) for l in open(os.path.join(fx, "prebis", name)).read().splitlines()]
        open(os.path.join(prebis2, name), "w").write("\n".join(lines) + "\n")
    out2 = os.path.join(out, "second")
    run(out2, fixture2, prebis2)
    check(open(os.path.join(out2, "prebis_quests_alliance_60.txt")).read() == first, "renumbered items give the same list (no item entry is special)")
    # blocks stay in the format of the quest data parser: start, anchor, quest, then records that name the quest
    check(first.count("\nstart ") == first.count("\nanchor "), "every start has an anchor")
    check("hunt 100 0 3 1111" in first and "area 100 20.0 20.0 1.0" in first, "hunt and area records are copied")
    check("reward 102" in first, "reward record is copied")

sys.exit(1 if failures else 0)
