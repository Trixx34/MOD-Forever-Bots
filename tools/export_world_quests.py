#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Writes the five exports that tools/make_world_quests.py reads (doc/world-quests.md) from a SQLite file made by tools/dump_to_sqlite.py, so no
MySQL server is needed. The tables needed: quest_template, quest_template_addon, quest_template_classic_level, quest_objectives, creature,
creature_queststarter, creature_questender, creature_loot_template, reference_loot_template.

    python3 -I tools/dump_to_sqlite.py world.sql.gz world.sqlite quest_template quest_template_addon quest_template_classic_level quest_objectives \
        creature creature_queststarter creature_questender creature_loot_template reference_loot_template
    python3 -I tools/export_world_quests.py world.sqlite exports/
    python3 -I tools/make_world_quests.py exports/ > data/world_quests.txt

This world database keeps no MinLevel on the quest (it comes from content tuning, which is in another database), so minLevel is written as 0 and the
planner's own rule applies (a bot more than 4 levels below the quest's level leaves it for later).
Creature loot: a row of creature_loot_template with ItemType 1 is a reference to reference_loot_template (one level, nested references are not followed).
"""
import os
import sqlite3
import sys


def write(db, path, query, header=None):
    count = 0
    with open(path, "w", encoding="utf-8") as out:
        for row in db.execute(query):
            out.write("\t".join("" if value is None else str(value) for value in row) + "\n")
            count += 1
    print(f"{os.path.basename(path)}: {count} rows")


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    db = sqlite3.connect(argv[1])
    out = argv[2]
    os.makedirs(out, exist_ok=True)
    for name, columns in [("creature", "id"), ("creature", "map"), ("creature_loot_template", "Item"), ("creature_loot_template", "Entry"),
                          ("reference_loot_template", "Item"), ("reference_loot_template", "Entry"), ("quest_objectives", "QuestID"),
                          ("quest_objectives", "ObjectID"), ("creature_queststarter", "quest"), ("creature_questender", "quest")]:
        db.execute(f"CREATE INDEX IF NOT EXISTS idx_{name}_{columns} ON `{name}`(`{columns}`)")

    write(db, f"{out}/quests.tsv", """
        SELECT q.ID, c.QuestLevel, 0, q.AllowableRaces, a.AllowableClasses, a.PrevQuestID, q.SuggestedGroupNum
        FROM quest_template q
        LEFT JOIN quest_template_addon a ON a.ID = q.ID
        JOIN quest_template_classic_level c ON c.ID = q.ID
        WHERE c.QuestLevel BETWEEN 1 AND 60 AND (q.QuestSortID >= 0 OR a.AllowableClasses <> 0)""")
    write(db, f"{out}/objectives.tsv", """
        SELECT o.QuestID, o.`Order`, o.Type, o.ObjectID, o.Amount
        FROM quest_objectives o
        JOIN quest_template_classic_level c ON c.ID = o.QuestID AND c.QuestLevel BETWEEN 1 AND 60""")
    write(db, f"{out}/npcs.tsv", """
        SELECT s.quest, 'start', s.id, cr.map, cr.position_x, cr.position_y, cr.position_z
        FROM creature_queststarter s JOIN creature cr ON cr.id = s.id
        UNION ALL
        SELECT e.quest, 'end', e.id, cr.map, cr.position_x, cr.position_y, cr.position_z
        FROM creature_questender e JOIN creature cr ON cr.id = e.id""")

    # Which creatures drop each quest item: directly, or through one reference table.
    db.execute("DROP TABLE IF EXISTS temp_drops")
    db.execute("""CREATE TEMP TABLE temp_drops AS
        SELECT o.ObjectID AS item, lt.Entry AS creature FROM quest_objectives o JOIN creature_loot_template lt ON lt.Item = o.ObjectID AND lt.ItemType = 0
        WHERE o.Type = 1
        UNION
        SELECT o.ObjectID, lt.Entry FROM quest_objectives o
        JOIN reference_loot_template r ON r.Item = o.ObjectID AND r.ItemType = 0
        JOIN creature_loot_template lt ON lt.Item = r.Entry AND lt.ItemType = 1
        WHERE o.Type = 1""")
    write(db, f"{out}/drops.tsv", "SELECT item, creature FROM temp_drops")
    write(db, f"{out}/targets.tsv", """
        SELECT o.QuestID, cr.id, cr.map, cr.position_x, cr.position_y, cr.position_z
        FROM quest_objectives o JOIN creature cr ON cr.id = o.ObjectID WHERE o.Type = 0
        UNION ALL
        SELECT o.QuestID, cr.id, cr.map, cr.position_x, cr.position_y, cr.position_z
        FROM quest_objectives o
        JOIN temp_drops d ON d.item = o.ObjectID
        JOIN creature cr ON cr.id = d.creature
        WHERE o.Type = 1""")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
