#!/usr/bin/env python3
"""Tests for tools/dump_to_sqlite.py and tools/export_world_quests.py on a tiny made-up mysqldump (no real database needed)."""
import gzip
import os
import sqlite3
import subprocess
import sys
import tempfile

ROOT = sys.argv[1]
TOOLS = os.path.join(ROOT, "tools")

DUMP = r"""-- MySQL dump
CREATE TABLE `quest_template` (
  `ID` int unsigned NOT NULL,
  `QuestSortID` int NOT NULL DEFAULT '0',
  `AllowableRaces` bigint NOT NULL,
  `SuggestedGroupNum` tinyint unsigned NOT NULL,
  `LogTitle` text,
  PRIMARY KEY (`ID`)
) ENGINE=InnoDB;
INSERT INTO `quest_template` VALUES (7,12,-1,0,'Kobold Camp Cleanup'),(8,12,1101,0,'It\'s a "quoted" \\ title');
CREATE TABLE `quest_template_addon` (
  `ID` int unsigned NOT NULL,
  `AllowableClasses` int NOT NULL,
  `PrevQuestID` int NOT NULL
) ENGINE=InnoDB;
INSERT INTO `quest_template_addon` VALUES (7,0,0),(8,0,7);
CREATE TABLE `quest_template_classic_level` (
  `ID` int unsigned NOT NULL,
  `LevelType` int,
  `QuestLevel` int
) ENGINE=InnoDB;
INSERT INTO `quest_template_classic_level` VALUES (7,0,1),(8,0,2);
CREATE TABLE `quest_objectives` (
  `ID` int unsigned NOT NULL,
  `QuestID` int unsigned NOT NULL,
  `Type` tinyint unsigned NOT NULL,
  `Order` tinyint NOT NULL,
  `ObjectID` int NOT NULL,
  `Amount` int NOT NULL
) ENGINE=InnoDB;
INSERT INTO `quest_objectives` VALUES (1,7,0,0,6,10),(2,8,1,0,500,3);
CREATE TABLE `creature` (
  `guid` bigint unsigned NOT NULL,
  `id` int unsigned NOT NULL,
  `map` smallint unsigned NOT NULL,
  `position_x` float NOT NULL,
  `position_y` float NOT NULL,
  `position_z` float NOT NULL
) ENGINE=InnoDB;
INSERT INTO `creature` VALUES (1,100,0,-8900.5,-160.25,82),(2,6,0,-8800,-170,81),(3,7,0,-8795,-171,81.5),(4,200,0,-8901,-161,82);
CREATE TABLE `creature_queststarter` (
  `id` int unsigned NOT NULL,
  `quest` int unsigned NOT NULL
) ENGINE=InnoDB;
INSERT INTO `creature_queststarter` VALUES (100,7),(100,8);
CREATE TABLE `creature_questender` (
  `id` int unsigned NOT NULL,
  `quest` int unsigned NOT NULL
) ENGINE=InnoDB;
INSERT INTO `creature_questender` VALUES (100,7),(100,8);
CREATE TABLE `creature_loot_template` (
  `Entry` int unsigned NOT NULL,
  `ItemType` tinyint NOT NULL,
  `Item` int NOT NULL,
  `Chance` float
) ENGINE=InnoDB;
INSERT INTO `creature_loot_template` VALUES (7,0,500,50),(900,1,7000,100);
CREATE TABLE `reference_loot_template` (
  `Entry` int unsigned NOT NULL,
  `ItemType` tinyint NOT NULL,
  `Item` int NOT NULL,
  `Chance` float
) ENGINE=InnoDB;
INSERT INTO `reference_loot_template` VALUES (7000,0,500,0);
CREATE TABLE `unrelated` (
  `a` int
) ENGINE=InnoDB;
INSERT INTO `unrelated` VALUES (1);
"""

failures = 0


def check(condition, message):
    global failures
    if not condition:
        failures += 1
        print("FAIL:", message)


def run(*args):
    return subprocess.run([sys.executable, "-I", *args], capture_output=True, text=True)


with tempfile.TemporaryDirectory() as tmp:
    dump = os.path.join(tmp, "world.sql.gz")
    with gzip.open(dump, "wt", encoding="utf-8") as handle:
        handle.write(DUMP)
    db = os.path.join(tmp, "world.sqlite")
    tables = ["quest_template", "quest_template_addon", "quest_template_classic_level", "quest_objectives", "creature", "creature_queststarter",
              "creature_questender", "creature_loot_template", "reference_loot_template"]
    result = run(os.path.join(TOOLS, "dump_to_sqlite.py"), dump, db, *tables)
    check(result.returncode == 0, "dump_to_sqlite failed: " + result.stderr)
    conn = sqlite3.connect(db)
    names = {row[0] for row in conn.execute("SELECT name FROM sqlite_master WHERE type = 'table'")}
    check("unrelated" not in names, "a table that was not asked for was read")
    check(conn.execute("SELECT COUNT(*) FROM creature").fetchone()[0] == 4, "creature rows")
    title = conn.execute("SELECT LogTitle FROM quest_template WHERE ID = 8").fetchone()[0]
    check(title == 'It\'s a "quoted" \\ title', "string escapes were not undone: " + repr(title))
    check(conn.execute("SELECT AllowableRaces FROM quest_template WHERE ID = 7").fetchone()[0] == -1, "negative numbers")
    conn.close()

    out = os.path.join(tmp, "exports")
    result = run(os.path.join(TOOLS, "export_world_quests.py"), db, out)
    check(result.returncode == 0, "export_world_quests failed: " + result.stderr)
    def lines(name):
        with open(os.path.join(out, name), encoding="utf-8") as handle:
            return [line.rstrip("\n").split("\t") for line in handle]
    quests = lines("quests.tsv")
    check(sorted(row[0] for row in quests) == ["7", "8"], "quests: " + repr(quests))
    check([row for row in quests if row[0] == "8"][0][5] == "7", "the predecessor of quest 8 is 7")
    check(len(lines("npcs.tsv")) == 4 and ["7", "start", "100", "0", "-8900.5", "-160.25", "82"] in lines("npcs.tsv"), "npcs: " + repr(lines("npcs.tsv")))
    # Quest 8 asks for item 500, which creature 7 drops directly and creature 900 through reference 7000 (creature 900 has no spawn, creature 7 has).
    drops = sorted(lines("drops.tsv"))
    check(["500", "7"] in drops, "direct drop missing: " + repr(drops))
    targets = lines("targets.tsv")
    check(any(row[0] == "7" and row[1] == "6" for row in targets), "kill target of quest 7: " + repr(targets))
    check(any(row[0] == "8" and row[1] == "7" for row in targets), "drop target of quest 8: " + repr(targets))

    # The whole chain: the exports feed make_world_quests.py.
    result = run(os.path.join(TOOLS, "make_world_quests.py"), out)
    check(result.returncode == 0 and "quest 7 " in result.stdout, "make_world_quests did not keep quest 7: " + result.stdout[:300] + result.stderr[:300])

if failures == 0:
    print("world_data_test: all passed")
sys.exit(1 if failures else 0)
