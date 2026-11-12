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

## Quest lists per faction and band, and the faction of the NPCs (2026-11-12)

`make_world_quests.py --side alliance|horde` keeps a quest out of a side's list in three ways: the race mask (when it names only the other side), the
giver creatures (all of them belong to the other side, `givers.tsv`), and, since this change, per NPC: a giver or ender spawn whose creature belongs to the
other side is not used (the generator takes the next spawn of the quest on the map), and a quest with no usable giver or ender spawn is left out
("giver hostile to the side", "ender hostile to the side"). The faction of a creature comes from `npc_factions.tsv` (creature entry, `creature_template.faction`;
`tools/export_world_quests.py` writes it with `givers.tsv` when the dump has `creature_template`; the MySQL queries are in `tools/export_giver_factions.sql`) and
`tools/faction_sides.tsv`. Without `npc_factions.tsv` the NPC filter is off. The check is in `tests/world_data_test.py` (a hand-made export with Horde,
Alliance and neutral NPCs). Regenerated 2026-11-12 from the sim world database: Alliance 944 quests (was 953), Horde 895 (894), 60 lists 343 and 327.

Map 2991 (start map of races 95 and 96, levels 1 to 13) was missing because `--maps` defaulted to `0,1`. Regenerated 2026-11-12 from the sim world database with
`--maps 0,1,2991` (the same four lists and the whole-world file): Alliance 1012 quests (was 945), Horde 968 (895), 60 lists 344 and 328, 14 and 15 new `W2991` starts.
Commands: leveling `--side <s> --maps 0,1,2991 --max-level 59`, level 60 `--side <s> --maps 0,1,2991 --min-level 60`, whole world `--maps 0,1,2991`. The sim and prod
have mmaps for map 2991 (34 files on the sim). Not yet seen on the sim: a bot from race 95 or 96 taking a quest at 4098/1849 (`quest_state` step other than `none`).

## Quest completion on the sim (analysis 2026-11-12)

Read-only look at the sim journal (3 days) and the bots telemetry files:

- Quests do complete: `forever-bots-telemetry.csv*` holds 5,660 `quest_done` lines (hand-ins and quest-leaves of a complete quest) over four of its five rotated
  files, none in the newest. The newest file and the last 6 hours of the journal come from the dungeon scenarios (maps 389, 36, 48, 43 ...): there every
  quest line says `allowed=0 party=1`, so the planner has nothing to do. "No `quest_done` in 2 hours" is a run with scenarios, not a broken quest path.
- The accepts the core refuses: 14,700 `quest_refused kind=accept_or_abandon` in 3 days (the core's own log says `bot cannot take the quest` for the 497 that
  it logs), 976 hand-in refusals (`bag_room=no` in 654). The ten most refused quests are class quests (792, 1463, 1516, 5648 ...) and level-1 starter quests.
  The refusing code is `Player::CanTakeQuest` (status, exclusive group, class, race, level, skill, reputation, previous quest, conditions). The module cannot see
  which test failed (the core's reason string is in the core log only) and its `Done` memory is lost at every restart, so a quest handed in on an earlier run is
  tried again. Change: a refused accept is now blocked 10 minutes the first time, one hour the second time and six hours from the third.
- The no-route flood: 280,000 `no_route ... cause=to_point` events in 3 days. Only a start's giver walk was handled (`NoteNoRoute` blocked the start for 30
  minutes); the walk to the ender of a quest in the log or to a hunting area was not, and a hunting quest ignored the block that `NoteWalkStuck` set
  (the planner's step 2 never looked at it). Changes: the hunt step honours the block; a no-route answer on a hunt moves to the quest's next area and three in a
  row block the quest (`kNoRouteWalksBlock`, 15 minutes), the same for the walk to an ender; a giver or ender within 9 yards that the core has no route to is
  tried from where the bot stands (the core measures the reach from the NPC's edge, and some NPCs stand behind a counter or on an object).
- Not fixable in the module: a core that answers no route for a target that stands on no navmesh tile; the 0,0,0 destinations (1,075 events) come from another
  feature, not from quests.

## Group quests (Stage B, config QuestGroups, off by default)

A quest for a party (`SuggestedGroupNum` 2 or more) is kept by `make_world_quests.py --groups` with `group <n>` in its `req` record (class quests keep no marker: they
are completed with their reward). Without `QuestGroups` the planner ignores such quests, so lists made with the flag are safe to load. With `QuestGroups = 1`:

1. The bot accepts the quest like any other. In the log, with its party smaller than `n`, the planner's step is **WaitGroup** (the bot stands still; the wait does
   not count as "stuck"). After `QuestGroupWaitMinutes` (10) without a party the step is Abandon: the quest is left alone for an hour and the bot goes on.
2. `quest_group.h` picks the leader: of the bots in WaitGroup for the same quest, the lowest guid that is not in another bot's party. It invites, until the party has `n`
   members, the free bots within `QuestGroupRangeYards` (150) on its map that have the same quest in their log (nearest first), then its guild mates
   (`QuestGroupGuildHelpers`). The invite is the group action; an invitee accepts an invite from a registered leader at once (not in a fight). Events:
   `quest_group_invite`, `quest_group_disband`.
3. The party follows its leader the way a tank-led party does (the policy puts it in the leader/member tables). Members do no quest work of their own; the
   leader hunts and hands in.
4. When the quest is out of the leader's log (handed in, abandoned) every member leaves the party; a member that completed the quest with the party then hands it
   in at the ender on its own, as a free bot.

**Data gap.** The sim world database has 502 quests with `SuggestedGroupNum` > 1, and none of them has a row in `quest_template_classic_level`, which the generator
joins for the quest level. So the 60 lists made today hold no group quest, and the feature is covered by unit tests only. To get group quests into the lists a level
source for them is needed (the content tuning of the hotfix database, or a hand-kept level file).

## Quests that start from an item or a game object (protocol 27)

The core's kQuest operation 5 (SDK `QuestStartFromItemOrObject`) starts a quest from the quest-starting item the bot carries or, with no such item, from the game
object whose entry is `target_guid` within interaction range. The module sends it for a quest with a `startfrom` row in the quest data (see `quest_data.h`):

    startfrom <quest> item <entry>      # no walk: tried where the bot stands; the core refuses (reason no giver) when the bot has no such item
    startfrom <quest> object <entry>    # the bot walks to the quest's `giver` point (the object's place), then operation 5 with the object entry

A refusal is read by reason like any accept (`ActionReason`): no giver is blocked ten minutes, then an hour, then six hours. **No data row exists yet**: the item and
object entries of 4903, 7761 and 7781 are not in the module's data and were not invented. They need the `quest_template.StartItem` and the game object that
`gameobject_queststarter` names, from the world database; once known, add `startfrom` rows after the `quest` lines of the three quests (and the object's position as
the giver point). Until then the chain is not started by the bots.

## Onyxia attunement chain: what is missing (2026-11-12)

The chain (quests 4903, 7761, 7781, 4241, 4941, 6568, 6570, 6502, 6602, 6569, 6601) does not fit the planner for these reasons,
from `--explain`: 4903, 7761, 7781 have no creature giver; 4241, 4941, 6568, 6570 end on another map (230, 1, 0); 6502, 6602, 6569, 6601 need items from creatures on
another map. What the module could do itself, and what stops it, checked against the SDK (`tier2_layout.h`, layout 25) and the core (`ModuleBotActions.cpp`):

| Need | Module side | Missing |
| --- | --- | --- |
| Accept a quest that starts from an item or a game object | the planner can walk to the object (data format has no object giver yet) | the core's `QuestAction` accepts only from a creature (`NearestQuestNpc` looks at `hasQuest` of creatures). Needs a kQuest operation 5 (accept from the game object or item named in `target_guid`) or a `kUseItem` that triggers `ItemTemplate::StartQuest`. `kUseObject` exists but is gated by `Bot.Actions.UseObject` and only clicks. |
| Know that the bot holds a quest item (the items from the other map) | - | a snapshot field with the bag's count of a given entry (the snapshot has ammo only). Without it a collect objective on another map cannot be told from "not yet looted"; the quest log counts do show it once the quest is in the log, so only the item-started quests need it. |
| Hop to another map for the ender (230, 1, 0) | `kTeleport` (map, x, y, z) with `Bot.Actions.Teleport`; a `hop` step plus an `ender map` field in the data format | nothing in the SDK; the sim must run with `Bot.Actions.Teleport = 1`, and a dungeon map needs the party/instance entry that `level_dungeon.cpp` does (a bare teleport into an instance map is refused unless the bot is in the party's instance) |
| Boss-kill step (Drakkisath, Rend and the like) | the dungeon runner knows boss deaths (`dungeon_events`) | a link from a quest objective to a dungeon run: the quest's creature kill objective in the log is progress enough, but the bot has to be in the dungeon party for it |

Nothing of this was invented in code. The first change to ask of the core is the kQuest operation 5 (object or item start); the second the bag-count snapshot field.
