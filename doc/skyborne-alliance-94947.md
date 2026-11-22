# Skyborne Alliance exit, step 2: 94947 "Take the Skyborne Portal to Stormwind" (2026-11-20)

The Alliance Skyborne exit chain is 93089 -> 94946 "The Magical City of Dalaran" (the hop from map 2991 to Dalaran on map 0, `doc/world-quests.md`) ->
**94947 "Take the Skyborne Portal to Stormwind"** -> 93963 and 98021 (level 13, Stormwind). Until now 94947 was not in the bot data, and the quest executor had
no way to meet its objective, so Alliance Skyborne bots that reached Dalaran stopped there. The Horde chain does not have this gap (see below).

Config: the existing key `SkyborneExitQuests` (default 0). With the key off nothing changes: 94947 is unknown to the planner, as before.

## What the core does (Trinity-Forever-Modules, read only)

- Quest 94947 (`sql/custom/world/2026_10_01_03_world_forever_sniff.sql`, quest_template ~13683, quest_objectives ~14045): giver Denaaris Stargale 259084
  in Dalaran (creature_queststarter), ender Bolvar Fordragon 1748 (creature_questender), prev 94946 (quest_template_addon). One objective:
  `(476752, 94947, Type 14, StorageIndex 0, ObjectID 231275, Amount 1, Flags 32 ...)` "Take the Skyborne Portal to Stormwind".
- **Objective type 14 = `QUEST_OBJECTIVE_CRITERIA_TREE`** (`src/server/game/Quests/QuestDef.h`). The ObjectID is a CriteriaTree id. At startup
  `CriteriaMgr` maps every type-14 objective's tree to the objective (`Achievements/CriteriaHandler.cpp`, ~4759); the player's `QuestObjectiveCriteriaMgr`
  updates those trees from `Player::UpdateCriteria`, and a completed tree calls `Player::KillCreditCriteriaTreeObjective`
  (`UpdateQuestObjectiveProgress(QUEST_OBJECTIVE_CRITERIA_TREE, ObjectID, 1)`). No areatrigger, no "player in area".
- **CriteriaTree 231275** (client `CriteriaTree.db2`, not in any SQL): "Gain Skyborne Portal to Stormwind Aura", Amount 0, flags 0x1000 (IsForQuest); one
  child, tree 231276 -> **Criteria 116403**: Type 29 = `CriteriaType::CastSpell`, Asset **1284041**, flags 0x20 (IsForQuest). Read with
  `tools/db2_reader.py` from `/srv/forever/data/dbc/enUS/CriteriaTree.db2` and `Criteria.db2`.
- Spell 1284041 "Portal to Stormwind" is the spell of the game object **631299 "Skyborne Portal to Stormwind"** (type 22 `GAMEOBJECT_TYPE_SPELLCASTER`,
  Data0 1284041, Data5 PlayerCondition 152835 = Alliance only), spawned in Dalaran at map 0 (450.66, 446.31, 110.75)
  (`2026_10_03_00_world_forever_sniff_maps.sql` ~1172). `GameObject::Use` on a spellcaster makes the **user** cast the spell; `Spell::_cast` then calls
  `UpdateCriteria(CriteriaType::CastSpell, 1284041)` (Spell.cpp ~3921), which completes the objective. 1284041 triggers 1284040, whose
  `spell_target_position` (`2026_10_10_00_world_classic_skyborne_portals.sql`) puts the player in Stormwind at (-8999.8, 856.45, 29.62), beside the
  Skyborne Portal to Dalaran 654168. So: **click the portal, the objective completes and the player lands in Stormwind**; then the hand-in at Bolvar.
- The quest's turn-in POI is (-8440, 329, 123) on UiMap 1453 (Stormwind), the same point as 98021's giver in the bot data (Bolvar).
- **Horde**: 95349 "The Earthen Ring" (hop 2991 -> Mulgore) -> **95350 "Welcome to Azeroth"** "Travel to Orgrimmar and speak with Thrall": it has **no
  quest_objectives row** (quest_poi only for the turn-in, Orgrimmar 1920, -4126, 43). It is a walk-and-talk quest and is already in
  `data/world_quests_horde.txt` (giver -658.0 426.6 187.2 in Mulgore, ender 1920.1 -4125.7 43.0, prev 95349). Nothing to add for the Horde.
- Race ids: `RACE_SKYBORNE_ALLIANCE = 95`, `RACE_SKYBORNE_HORDE = 96` (`src/server/game/Miscellaneous/RaceMask.h`). The Alliance Skyborne is **95**.

## What the bots now do

Data (`data/world_quests_alliance.txt`, start `W0_0_0`, the start nearest Dalaran on map 0; added by hand, not by `make_world_quests.py`, which still skips
objective type 14):

```
quest 94947 giver 449.0 439.0 111.3 ender -8440.0 329.4 122.6
req 94947 level 13 minlevel 7 prev 94946
use 94947 631299 450.66 446.31 110.75 land -8999.80 856.45 29.62
```

New record `use <quest> <entry> <x> <y> <z> [land <x> <y> <z>]` (`src/quest_data.h`): the objective is met by using the game object at that point; `land` is
where the object's spell puts the bot (same map). The planner knows a `use` quest only with `SkyborneExitQuests` on **and** when its `prev` is a usable hop
quest (94946); any other `use` quest is ignored. It is part of the exit chain (taken when grey).

Planner (`src/quest_planner.cpp`, step 0b, before the hand-in rule), with 94947 in the log and the bot within 200 yards of the portal on map 0:

1. after the hop and the hand-in of 94946 the bot stands at Denaaris: **Accept** 94947 (a hop leg, so LevelSafeWalks does not drop it in Alterac);
2. **WalkTo** the portal (7 yards, within 4 yards of its point);
3. **UseObject**: the policy sends `kUseObject` (spell_id 631299, dest = the portal's point); the core uses the object as a click (`object->Use(&bot)`);
4. **Wait** until the bot has moved more than 100 yards (or changed map): the portal worked (`skyborne_portal_done`); then the normal hand-in walk to Bolvar
   and the **HandIn**; then 98021 in Stormwind.
5. A use the core refuses (`Bot.Actions.UseObject = 0`, out of range, no object) or one that does not move the bot within 20 s is a failed use
   (`skyborne_portal_refused`). After **2 failed uses** the fallback: a **Hop** step, `kTeleport` to the land point on map 0
   (`skyborne_portal_fallback`, needs `Bot.Actions.Teleport = 1`); refused or silent, it blocks the quest like a refused hop (1 h, 6 h, for good).
   After a fallback teleport the objective is not met (no spell cast): the hand-in will be refused and the stuck rule abandons 94947 after 10 minutes; the bot
   is then in Stormwind anyway and goes on with its quests there (98021 stays closed to it: its prev 94947 is not done).

A 94947 that is complete but whose bot is still in Dalaran uses the portal again rather than walking across the continent to Bolvar. The stuck rule does not
abandon 94947 while the bot works at the portal. With 94946 not known done (a module restart forgot it), only race 95/96 (or an unknown race) tries 94947.

The fallback teleport is the only new teleport, stays on map 0, and exists only for this quest of the exit chain; no hop for zone spread was added
(owner rule: Skyborne leave map 2991 only through the exit quest).

## How to verify on the sim

Needs: bots with this branch, `SkyborneExitQuests = 1` in the bots config, and in the core `Bot.Actions.Teleport = 1` (the hop) and
`Bot.Actions.UseObject = 1` (the portal; on on the sim since 2026-10-25). Bot: an **Alliance Skyborne (race 95)** of level 7 or more that has done 93089.

Journal (`journalctl -u forever-sim`), in order for one bot guid:

```
skyborne_hop_start: bot=<g> quest=94946 ... to=0:448,439,111
skyborne_hop_done: bot=<g> level=.. map=0 pos=448,439,111
(the accept of 94947 is not logged by the module; a quest_refused: ... quest=94947 line means the core refused it)
skyborne_portal_use: bot=<g> quest=94947 object=631299 level=.. map=0 pos=450,44x,110
skyborne_portal_done: bot=<g> quest=94947 level=.. map=0 pos=-8999,856,29      <- the portal worked
(hand-in of 94947 at Bolvar; then 98021 accepted)
```

Failure signs: `skyborne_portal_refused: ... reason=use_refused status=refused` (UseObject off in the core), `reason=no_move` (the use did nothing: the spell
or its target position), then `skyborne_portal_fallback:` and `skyborne_hop_done` (the teleport) or `skyborne_hop_refused: ... quest=94947`. Grep:
`journalctl -u forever-sim --since "<start>" | grep -E "skyborne_(hop|portal)_" | grep -E "quest=9494[67]|skyborne_hop_done"`. A read-only DB check of a bot that
reached Stormwind: `character_queststatus_rewarded` has 94947 for its guid.

## Unverified

- That `kUseObject` (a direct `GameObject::Use`, no `MeetsInteractCondition` check) casts 1284041 for a bot and that the criterion counts for it on the sim
  (bots are players, so `Spell::_cast` should update the criteria). The cast time of 1284041 is not known (its SpellMisc row is not in the readable client
  data): a cast time with the bot moving would interrupt it; the bot stands still while it waits.
- That the 1284040 teleport works for a bot (near teleport on map 0) and lands at the `spell_target_position` point.
- That the prod world database has the same quest, objective, portal spawn and `spell_target_position` rows as the sim; prod probably has
  `Bot.Actions.UseObject = 0` (then every bot takes the fallback teleport and does not get 94947's credit).
- Bolvar's ender point is taken from the quest POI and 98021's giver, not from a spawn row.
- 93963 (the other follow-up of 94947) is not in the bot data.
