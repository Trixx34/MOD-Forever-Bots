# Whole-world quests: leveling 1 to 60 by quests

Status: planner and data format built and unit-tested. The data file `data/world_quests.txt` is made (1917 quests in 288 starts, levels 1 to 60, from the
test world database) with `tools/make_world_quests.py`. Of those, 684 are class quests (`reward` records, since 0.25.0): the bot completes them with
their reward (see README, Class quests). **No bot has levelled by it on the sim yet.**

## Making the data without a MySQL server

The five exports can be made from a `mysqldump` file (a backup of the world database) with two tools, which need no database server and no packages:

    python3 -I tools/dump_to_sqlite.py forever_test_world.sql.gz world.sqlite quest_template quest_template_addon quest_template_classic_level \
        quest_objectives creature creature_queststarter creature_questender creature_loot_template reference_loot_template
    python3 -I tools/export_world_quests.py world.sqlite exports/
    python3 -I tools/make_world_quests.py exports/ --explain skipped.tsv > data/world_quests.txt

This is how the committed file was made (about 15 seconds). `export_world_quests.py` runs the queries below for this world's tables, with two differences:
this world has no `MinLevel` on the quest (it comes from content tuning in the hotfix database), so `minLevel` is written as 0 and the planner's own
rule applies; and quest items dropped through a loot reference are found with the reference path described under the queries. Result on the test world:
3636 quests have a classic level 1 to 60; 1256 are kept. The rest are skipped: 929 have no creature giver (they start from an item or an object), 849 want an item
no creature drops (an object, a vendor, a pickpocket), 359 have no target on the giver's map, 212 have no ender on the giver's map, 31 are not kill or
collect quests (the counts overlap, a quest is counted by its first reason). About 116 of the kept quests are for levels 1 to 9, 180 for 10 to 19, 170 for 20
to 29, 210 for 30 to 39, 170 for 40 to 49, 230 for 50 to 59 and 170 for 60.

## What changed in the planner

The M5 planner worked one starter zone: the bot's start was the nearest anchor, and it did that start's quests in order. For the whole world:

- **Many starts.** Each area of quest givers is a start (anchor = the average position of its givers). A bot takes quests from the nearest start on its
  map that has a quest it can take now; when that start is used up it walks on to the next one, however far (a start made for the whole world may be
  walked to from anywhere on the map; the older starter-zone starts keep their 1500-yard radius).
- **Who may take a quest** (`req` records, see `src/quest_data.h`): the least level, the quest's own level, a predecessor quest, race and class masks.
  - A bot below the least level, or more than 4 levels below the quest's level, leaves it for later.
  - A bot more than 7 levels above the quest's level skips it (grey: little experience).
  - A quest whose race or class mask excludes the bot is skipped. The module knows each bot's race and class.
  - A quest whose predecessor is not known to be done is tried after all the others, not never: a bot that restarts forgets what it has done, and the core
    refuses what is already complete (the refusal blocks that quest for a minute, then ten minutes, then an hour).
- **Quests from another start in the log** are still worked: the bot hands in and hunts them wherever they came from.
- **The log has room for 5 accepted quests** at most; the bot hands in and finishes before it takes more.
- Group quests, elite quests, exploration, escort, talk-to and use-object quests are left out of the data (the planner can only kill and collect).
  Class quests are the exception (since 0.25.0): a quest restricted to some classes is kept with its giver and ender and marked `reward`, so the bot
  completes it with its reward and does no objectives. The export keeps them although their quest sort is negative (the class sort IDs are -61 to -344).

Travel is by walking on one map. A bot never changes continent by itself (boats, zeppelins and portals are not part of this), and the server's pathing
decides whether a long walk works.

## Making the data

The input is five tab-separated exports from the sim world database (the column names below are the ones confirmed for doc 17; adapt a name that
differs, and start with `SHOW COLUMNS FROM` for the tables you are unsure of). A header line is fine, it is skipped.

`quests.tsv` (one line per quest; `q.MinLevel` may be named differently):

    SELECT q.ID, c.QuestLevel, q.MinLevel, q.AllowableRaces, a.AllowableClasses, a.PrevQuestID, q.SuggestedGroupNum
    FROM quest_template q
    LEFT JOIN quest_template_addon a ON a.ID = q.ID
    JOIN quest_template_classic_level c ON c.ID = q.ID
    WHERE c.QuestLevel BETWEEN 1 AND 60 AND q.QuestSortID >= 0;

`objectives.tsv`:

    SELECT o.QuestID, o.`Order`, o.Type, o.ObjectID, o.Amount
    FROM quest_objectives o
    JOIN quest_template_classic_level c ON c.ID = o.QuestID AND c.QuestLevel BETWEEN 1 AND 60;

`npcs.tsv` (every spawn of every giver and ender; the tool picks the first on the quest's map):

    SELECT s.quest, 'start', s.id, cr.map, cr.position_x, cr.position_y, cr.position_z
    FROM creature_queststarter s JOIN creature cr ON cr.id = s.id
    UNION ALL
    SELECT e.quest, 'end', e.id, cr.map, cr.position_x, cr.position_y, cr.position_z
    FROM creature_questender e JOIN creature cr ON cr.id = e.id;

`targets.tsv` (spawn points of the creatures a quest asks for, kill objectives and the droppers of collect objectives):

    SELECT o.QuestID, cr.id, cr.map, cr.position_x, cr.position_y, cr.position_z
    FROM quest_objectives o JOIN creature cr ON cr.id = o.ObjectID WHERE o.Type = 0
    UNION ALL
    SELECT o.QuestID, cr.id, cr.map, cr.position_x, cr.position_y, cr.position_z
    FROM quest_objectives o
    JOIN creature_loot_template lt ON lt.Item = o.ObjectID
    JOIN creature cr ON cr.id = lt.Entry
    WHERE o.Type = 1;

Items that creatures drop only through a loot **reference** table are missed by the plain join. The query depends on how the fork stores references. On the sim world
`creature_loot_template` has **no `Reference` column**: a reference is a row with `ItemType = 1` whose `Item` is the reference entry, and `reference_loot_template`
rows with `ItemType = 0` hold the actual items. So the path is: `reference_loot_template` (`ItemType = 0`, `Item` = the quest item) -> its `Entry` ->
`creature_loot_template` (`ItemType = 1`, `Item` = that entry) -> `creature`. A direct join of the three tables can run for many minutes; use temporary tables for
the reference entries first. Nested references (a reference row that is itself `ItemType = 1`) are not followed. On the sim this added 465 rows to `drops.tsv` and 863
to `targets.tsv`, but the recovered quests mostly failed the next check (their droppers do not spawn on the giver's map), so the kept count did not change.

`drops.tsv` (which creatures drop a quest item):

    SELECT o.ObjectID, lt.Entry FROM quest_objectives o JOIN creature_loot_template lt ON lt.Item = o.ObjectID WHERE o.Type = 1;

Then:

    python3 -I tools/make_world_quests.py exports/ > data/world_quests.txt

The tool prints how many quests it kept and why it skipped the others. `--explain skipped.tsv` also writes one line per skipped quest (quest, level, reason, detail) and prints a breakdown of the reasons; use it to see where a high skip count comes from. Options: `--cell 500` (yards per start), `--maps 0,1`, `--areas 3` (hunting
areas per quest), `--min-level`, `--max-level`. Set `QuestFile = "data/world_quests.txt"` in the module's config (instead of the starter file; the two
formats are the same, so both can also be joined by hand).

## Known limits

- Loot-table drop chances are not looked at: a collect quest with a rare drop may take long and be abandoned by the 10-minute stuck rule.
- Objective order: an objective's position in the log is its rank by `Order` (verified for quest 376 only when the sim has run it).
- Quests that need an item to start (a quest item from a drop), reputation or a skill are not filtered: the core refuses them and the block escalates.
- Spawn points are used as hunting areas as they are; creatures behind walls or in water are tried and left after the usual give-up time.
