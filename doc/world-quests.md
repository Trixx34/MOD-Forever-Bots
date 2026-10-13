# Whole-world quests: leveling 1 to 60 by quests

Status: planner and data format built and unit-tested; the data file is made from the world database with `tools/make_world_quests.py` (not made yet).

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

`drops.tsv` (which creatures drop a quest item):

    SELECT o.ObjectID, lt.Entry FROM quest_objectives o JOIN creature_loot_template lt ON lt.Item = o.ObjectID WHERE o.Type = 1;

Then:

    python3 -I tools/make_world_quests.py exports/ > data/world_quests.txt

The tool prints how many quests it kept and why it skipped the others. Options: `--cell 500` (yards per start), `--maps 0,1`, `--areas 3` (hunting
areas per quest), `--min-level`, `--max-level`. Set `QuestFile = "data/world_quests.txt"` in the module's config (instead of the starter file; the two
formats are the same, so both can also be joined by hand).

## Known limits

- Loot-table drop chances are not looked at: a collect quest with a rare drop may take long and be abandoned by the 10-minute stuck rule.
- Objective order: an objective's position in the log is its rank by `Order` (verified for quest 376 only when the sim has run it).
- Quests that need an item to start (a quest item from a drop), reputation or a skill are not filtered: the core refuses them and the block escalates.
- Spawn points are used as hunting areas as they are; creatures behind walls or in water are tried and left after the usual give-up time.
