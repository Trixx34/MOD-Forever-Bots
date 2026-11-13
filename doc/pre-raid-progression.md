# Pre-raid progression: level 60 bots work towards their best pre-raid gear and the attunements

Status: design (2026-11-13) and the first slice (quest lists and attunement chains from the gear lists, `tools/gen_prebis_quests.py`). No planner change yet.

Owner's point (2026-11-13): "level 60 bots work towards their pre-raid best in slot gear and attunement quests". A level 60 bot in the live world (not only in a
scenario) follows a path: gear wishlist (per class, PvE first) -> pursues the sources of the items it lacks (quests, dungeon drops, vendors and reputation, crafting) ->
completes the attunement quests -> then raids. This keeps every raid a first-time raid: the bots arrive in pre-raid gear, not in the raid tier of the one before
([[bot-progression-path]] in the memory, `doc/pre-raid-gear.md` on `feature/prebis-gear`). Rule throughout: nothing is selected by item entry. Items come from the lists,
quests from the database by the rewards of the quest, attunements from quest ids of a short rules file and the chain walk (PrevQuestID, RewardNextQuest, NextQuestID).

## What exists today

| Piece | State |
| --- | --- |
| Gear wishlist | `data/prebis/<classId>.txt` (`tools/gen_prebis.py`, branch `feature/prebis-gear`): per slot an item, with a tag `source=quest|vendor|dungeon|world|crafted|pvp`. Used today only by scenarios (`ScenarioGear<class>`), which grant the items. |
| Quest planner | `quest_planner.cpp`, data `quest_data.h`; lists per side and band (`QuestFileAlliance`, `QuestFileHorde`, `QuestFileAlliance60`, `QuestFileHorde60`; level 60 falls back to the leveling list, then `QuestFile`). `Eligible` honours `req` `level`, `minlevel`, `races`, `classes`, `prev`, `group`. |
| Grind spots | `GrindFile`, `GrindSpotsForBot` in `bot_policy_turn_town.cpp`; idle bots walk to spots that suit their level. |
| Dungeons | `level_dungeon.*` schedules parties at the high end of a dungeon's level range; `dungeon_files.*`, scenario runs with the roster and `ScenarioGear`. |
| Errands | `errand_policy.*`: training and auction visits to capitals, with a daily budget. |
| Raids | `raid_rules.*`, `ScenarioRaid`, guild raids (`GuildsRaid*`). |

The level 60 lists (`data/world_quests_<side>_60.txt`) hold only quests of level 60 (`--min-level 60`). The rewards of quests of level 50 to 59 (many pre-raid pieces) are in
the leveling lists, which a level 60 bot does not use. That is the first gap, and the first slice closes it.

## Stages

A bot at level 60 sits in one stage. The stage is a pure function of the bot's gear, its done quests and its attunements, so a restart does not lose it
(see the core gaps for how "done" is known).

1. **Wishlist.** The class list gives the target item per slot with its source tag. Missing items = the list entries the bot does not wear and does not carry. A
   slot the bot fills with an item of equal or higher item level and quality counts as filled (the core's equip service keeps wearing the best; the wishlist never
   makes a bot take off a better item). PvP items are not pursued (`source=pvp`, owner: PvE first).
2. **Quest sourcing** (`source=quest`). Level-60 quest list per side built from the rewards: first slice, below. The planner walks them like any quest list; `classes`
   limits each quest to the classes whose list holds one of its rewards. A reward choice is picked by the core's hand-in logic (not changed here).
3. **Dungeon sourcing** (`source=dungeon`). A table item -> dungeon -> boss from the loot tables (creature loot of the dungeon maps; the generator already tags the maps).
   The planner raises the priority of a dungeon whose bosses drop an item the party's members miss (several members per dungeon beat one). It reuses
   `level_dungeon`'s party building and the dungeon runner; the change is in the choice (which dungeon, for how many runs), not in the run. Loot rules: a bot rolls need
   on a wishlist item, greed on the rest (core gap).
4. **Vendor and reputation** (`source=vendor`). Items bought with gold are an errand (like training): walk to the vendor, `BuyItemForBot`. Items that need reputation
   or a profession recipe wait for the reputation (a grind spot or quests of the faction; phase 4). Items bought for honor or badges are `pvp`.
5. **Crafting** (`source=crafted`). The bot's profession and `kCraft` (SDK 0.18) make the item when the bot has the recipe and the materials; a bot without the profession
   buys it on the auction house (existing `auction.*`), at most at a price bound.
6. **Attunement.** The attunement chains of `data/attunements.txt` (generated ordered quest lists in `data/attunement_chains.txt`, quests in the side's pre-raid quest file
   where the planner can do them). The bot works the chain once its wishlist is at the "ready" threshold, or in parallel (the chain is a quest list, so the planner walks
   it with the rest).
7. **Ready for raid.** The predicate below; the raid scheduler (`raid_rules`, guild raids, scenario raid rosters) takes only ready bots.

Order of work for an idle level-60 bot (the planner's choice, highest first): a party that is forming for a dungeon that fits the missing items (3); a quest of the pre-raid list
in the log or startable here (2, 6); a due errand (4, 5); a grind spot (existing). It is one more input to the existing decision order in `bot_policy_turn_town.cpp`, not
a new loop.

## Data files

| File | Made by | Content |
| --- | --- | --- |
| `data/prebis/<classId>.txt` | `tools/gen_prebis.py` | the wishlist (exists on `feature/prebis-gear`) |
| `data/attunements.txt` | by hand (rules) | `attunement <name> <alliance\|horde\|both> <final quest id>`; the rest is derived |
| `data/prebis_quests_alliance_60.txt`, `data/prebis_quests_horde_60.txt` | `tools/gen_prebis_quests.py` | quest lists in the `world_quests_*_60.txt` format (first slice) |
| `data/attunement_chains.txt` | `tools/gen_prebis_quests.py` | `chain <name> <side> <quest ...>` in order, and `status` of each quest (placed, or why not) |
| `data/prebis_quests_report.txt` | `tools/gen_prebis_quests.py` | counts and the wanted quests that are not in a list, with the reason |
| `data/raid_ready.txt` (phase 2) | by hand | the thresholds and the gates below |
| `data/prebis_sources.txt` (phase 3) | generator | item -> source: dungeon and boss, vendor and price, recipe, quest |

## Settings (proposed; the first slice adds none)

The first slice is used with the existing keys: `QuestFileAlliance60 = data/prebis_quests_alliance_60.txt` and `QuestFileHorde60 = data/prebis_quests_horde_60.txt`.
Later phases:

| Key | Default | Meaning |
| --- | --- | --- |
| `PreRaidProgress` | 0 | the stages above for level 60 bots (off: today's behaviour) |
| `PreRaidGearDir` | `data/prebis` | the wishlists |
| `RaidReadyFile` | `data/raid_ready.txt` | thresholds and gates |
| `PreRaidDungeonShare` | 50 | percent of a level 60 bot's idle time that may go to missing-item dungeons |
| `PreRaidVendorBudgetCopper` | 0 (no bound) | most a bot spends on one vendor errand |

## The generator (first slice)

`tools/gen_prebis_quests.py --mysql "ssh forever-test sudo -n mysql forever_test_world" --prebis data/prebis --attunements data/attunements.txt --pool-alliance data/world_quests_alliance.txt --pool-alliance data/world_quests_alliance_60.txt --pool-horde ... --out data`
(or `--offline-json` with a file written by `--dump-json`; the tests use the fixture in `tests/data/prebis-quests`). It only reads (SELECT through a mysql command).

Tables and columns used (checked on `forever_test_world`, 2026-11-13): `quest_template` (`ID`, `LogTitle`, `QuestSortID`, `SuggestedGroupNum`, `Flags`, `StartItem`,
`RewardNextQuest`, `RewardItem1..4`, `RewardChoiceItemID1..6`), `quest_template_addon` (`PrevQuestID`, `NextQuestID`), `quest_classic_level` (`QuestLevel`, the level source of
the world-quest generator), `creature_queststarter` and `creature_questender` (`id`, `quest`).

Rules, none of them on an item entry:

1. Read the lists; keep entries with `source=quest`. A quest is wanted for a class when one of its reward or choice-reward items is such an entry. Quests with
   `SuggestedGroupNum >= 10` or the raid flag (0x40) never count (the gear generator already left the raid chains out).
2. Blocks are copied from the side's existing quest files (leveling list, then level 60 list), so each has a giver, an ender, objectives and areas the planner
   reads. A wanted quest that is not in them is reported with the reason (no creature giver, no creature ender, no `quest_classic_level` row, party quest, raid flag, or
   an ender on another map or an objective the planner cannot do; `make_world_quests.py --explain` gives the exact one).
3. The chain: `PrevQuestID` is walked back while the quests are in the lists, so `prev` quests come along.
4. `req` loses `level` (a level 60 bot is more than 7 levels above a level 52 quest and `Eligible` would skip it as grey) and gets `classes` = the classes that want a reward
   (intersected with a class mask the quest already has). Attunement quests get no class limit.
5. Attunement chain: the final quest of `data/attunements.txt`, then everything that leads to it: `PrevQuestID`, `RewardNextQuest` and `NextQuestID` of other quests, ordered by distance
   from the final quest.
6. Output: one file per side in the `world_quests_*_60.txt` format (starts regrouped by their cell, anchor = average of the givers), the chains file and the report. The parser
   of `quest_data.cpp` reads both files with no warnings (checked 2026-11-13).

### Result of the first run (sim world database, 2026-11-13; lists from `feature/prebis-gear`: 9 classes, 148 entries, 73 with `source=quest`)

See `data/prebis_quests_report.txt` for the current numbers. All 73 quest-tagged items have a quest in the database. Of the quests that reward them, 120 `missing` lines (both sides, chain quests included) cannot be placed with the
current planner (ender on another map, objective types the planner skips, no `quest_classic_level` row, a party quest).

### Attunement quest ids (checked against `quest_template` and the addon table of `forever_test_world`)

| Attunement | Side | Chain (in order) | Verified |
| --- | --- | --- | --- |
| Onyxia (Drakefire Amulet) | Alliance | 4182, 4183, 4184, 4185, 4186, 4223, 4224, 4241, 4242, 4264, 4282, 4322, 6402, 6403, 6501, 6502 | titles and links in the DB; 4182, 4185, 4223, 4224 are in the Alliance list; 4264 starts from an item (11446); 6402, 6403, 6502 carry the raid flag |
| Onyxia | Horde | 4903, 4941, 4974, 6566, 6567, 6568, 6569, 6570, 6582, 6583, 6584, 6585, 6601, 6602 (4975, the Orb quest, hangs on 4963 and is not in the chain) | the three Tests of Skulls lead to 6585 through `NextQuestID`; 6566, 6567, 6582, 6584 are in the Horde list |
| Molten Core | both | 7848 Attunement to the Core (Lothos Riftwaker, creature 14387; 7487 is a race-masked copy) | raid flag; not in the lists |
| Blackwing Lair | both | 7761 Blackhand's Command (no creature giver or ender; a game object, 179880, ends it; the quest text names the Mark of Drakkisath and the Orb of Command) | raid flag, no creature; not in the lists |
| Upper Blackrock Spire key | both | 4742, 4743 Seal of Ascension (4743 starts from item 12339) | 4743 from an item |
| Zul'Gurub, AQ20, AQ40, Naxxramas | - | none in the data | `access_requirement` has no quest or item for maps 229, 249, 309, 409, 469, 509, 531, 533 |

Not verified: that the 4182 line reaches 4241 in play as the chain says (it comes from `PrevQuestID` and `RewardNextQuest` only; no script was read); which quests of
the Alliance chain a Horde-only world list drops (the lists are per side and the chain is per side, so no cross-use); that 7781 (The Lord of Blackrock, the head of Nefarian) is not an
attunement (it is a turn-in from a quest item, so it was left out; the owner named it with 4903 and 7761, which the data does not support as an attunement). **The server does not enforce
any attunement:** `access_requirement` has no quest or item for Onyxia, Molten Core or Blackwing Lair. The attunements here are a rule of the bots' progression, enforced by
the readiness check, not a lock in the game.

## Raid ready (spec)

`ready(bot, raid)` is true when all of these hold. The thresholds live in `data/raid_ready.txt` (per raid), so they change without a build.

1. Level >= the raid's minimum level (60).
2. Gear: average item level of the equipped slots (no shirt, no tabard) >= `min_avg_ilvl` of the raid, and no equipped main slot (head, shoulder, chest, hands, legs, feet, weapon)
   below `min_slot_ilvl`. Starting values to be calibrated on the sim from the measured average of the pre-raid roster: ZG 58, Onyxia 60, Molten Core 60, Blackwing Lair 64;
   `min_slot_ilvl` 45. A bot with fewer than `min_wishlist_pct` (60) of the wishlist slots filled by a wishlist item is not ready either (the list says which items count; the pursuit of the
   rest is stages 2 to 5).
3. Attunement: every attunement the raid's gate names is done: `gate <raid> <attunement name>`: `gate onyxia onyxia`, `gate mc mc`, `gate bwl bwl`; ZG has no gate. The side picks the chain
   (Onyxia differs per side). "Done" = the final quest of the chain is rewarded.
4. Alive, durability >= 50 percent, the stock of consumables the raid rules ask for (phase 5; ignored until then).

A bot that is not ready is never put into a raid roster by the module (`raid_rules`, guild raids); in a scenario the `ScenarioGear` grant stands in for stages 1 to 5, so scenarios
are unchanged. Explained in the log as `raid_ready bot=.. raid=.. ok=0 why=avg_ilvl|slot|wishlist|attunement:<name>`.

## What the core has to offer (gaps; the core is not changed here)

| Need | Today | Gap |
| --- | --- | --- |
| Which quests the bot has rewarded (attunement done, quest done after a restart) | the module's `Done` set, lost at restart; an accept answers `kAlreadyDone` (SDK `tier2_layout.h` reply reason 6) when the quest is rewarded | a snapshot field or query with the rewarded quests (or a bit per quest of a list the module names). Stopgap: a probe accept on login for the chain's final quests |
| Equipped items by slot (entry, item level), average item level | the snapshot holds the best equipped item level (layout 23, `item_level`) | a per-slot list of equipped entries and levels, or the average, in the snapshot |
| Items in bags | no entry count (ammo only) | a count of a named entry, or a list of the bag's entries (also needed for quest items, see `doc/world-quests.md`) |
| Quests that start from an item or an object | kQuest operation 5 (protocol 27) | none; data rows `startfrom` for them (4264 from item 11446, 4743 from item 12339) need the item to be looted first |
| Ender on another map (Onyxia chain ends in 230, 1, 0; MC portal in Blackrock Depths) | `kTeleport` (gated by `Bot.Actions.Teleport`) | the step in the planner; entering an instance map needs the party instance |
| Quests that start from a game object (7761's Orb of Command) | operation 5 can start from an object entry | the object quest data (`gameobject_queststarter` has no row for 7761; the orb is the ender, not a starter) |
| Need or greed on loot by wishlist | `kLoot` takes what the bot may; no roll choice | a roll policy action or a loot-rule setting for bots (need on wishlist items) |
| Reputation standing (vendor and rep items) | not in the snapshot | standing per faction for the factions of the vendor items |
| Buy a named item at a vendor | `BuyItemForBot(vendor, entry, count)` exists | none |
| Craft | `kCraft` (spell id) exists | none |
| Grant a wishlist item for tests | `Bot.Actions.Grant` exists | none |

## Phased plan

1. **First slice (this change, no core change, no planner change).** Generator, `data/attunements.txt`, the two quest lists, the chains file and the report, the fixture test, this
   doc. Use: `QuestFile*60` on the sim; measure `quest_done` for the lists' quests and the average item level of the bots over a day. Smallest useful: level 60 bots do the
   gear quests that were in no list before.
2. **Raid ready check in the module (small C++, new files).** `raid_ready.h/.cpp`: pure predicate on a snapshot struct, `data/raid_ready.txt`, a log line, unit tests; first with what the snapshot has
   (best item level, the module's `Done` memory, the probe accept), then with the core fields when they exist. Raid rosters (scenario and guild raids) filter by it.
3. **Sources table and dungeon choice.** Generator for `data/prebis_sources.txt` (dungeon, boss, vendor, recipe, quest per list item); the planner picks the dungeon that fills the most
   missing slots of the idle level 60 bots; the loot roll rule when the core has it.
4. **Vendor, reputation and crafting errands.** Buy-list errand after the training errand, the crafting wish, reputation farming as a grind spot or quest list of the faction.
5. **Attunement beyond the planner's reach.** The cross-map enders (hop step), item and object starters (`startfrom`), the boss kills (Drakkisath for the Mark, the Test of Skulls
   targets) through the dungeon runner; consumables and raid buffs for the ready check.
6. **Higher raids.** After a clear (ZG, Onyxia) the list for the next raid is the tier of the raid before: the same machinery with a second `data/prebis`-style list per stage.

Left for later phases: everything in phases 2 to 6; the class-specific quest lists per class (the module's list is per side, so `classes` on the quest does the split); the
`make_world_quests.py` change that would place the quests now reported missing (ender on another map, items from other maps, the raid-flagged attunement quests, group quests need `QuestGroups`).
