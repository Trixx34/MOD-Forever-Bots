# Professions: why the skills do not rise, and the progression behind `Professions`

Status 2026-11-20, branch `feature/professions-20261120` (from `fix/arm-split-balance-20261120`, 3eba59a). Local only: not built by Jenkins, not run on the
sim. Code: `src/professions.h/.cpp` (pure logic), `src/bot_policy_professions.cpp`, hooks in `bot_policy_turn_cast.cpp` (`TurnCraftGather`),
`bot_policy_result.cpp` (`ReadResult`), `bot_policy_turn_town.cpp` (loot, node walk arrival); data `data/profession_recipes.txt` from
`tools/gen_profession_recipes.py`. Tests: `tests/professions_test.cpp`, `tests/policy_test_12.cpp` (`Professions*`), `tests/settings_test.cpp`.
Background: `doc/economy-and-professions.md` (the chain from the config key to the core handler).

## Findings

Prod, read 2026-11-20: 800 bots, average level 16.2. Skill line, bots that have it, average value:

| Profession | Line | Bots | Avg (max) | Why it does not rise | Confidence |
|---|---|---|---|---|---|
| Mining | 186 | 396 | 1.2 (6) | `Gathering = 0` on prod: no `kGather` is ever sent. Smelting (also line 186) needs ore and a forge. | high (code + config) |
| Skinning | 393 | 313 | 1.0 | `Gathering = 0`. With `Gathering = 1` (sim 2026-11-18) skinning shares the node back-off: after a few "nothing within 30 yards" answers the line waits 12 to 48 minutes (`kGatherFailDoublings`), so the bot's own kills are not skinned. | high (code); sim skill stayed about 1 |
| Herbalism | 182 | 288 | 5.1 (38) | `Gathering = 0` now; the values above 1 are from an earlier prod config that had it on. | medium (doc/economy-and-professions.md) |
| Leatherworking | 165 | 110 | 6.7 | `kSkillUp` (Crafting = 1) is sent, but the core finds no recipe whose materials the bot carries: no Skinning, so no leather. Small rises probably from looted leather or scraps. | high (code); source of the small rises unverified |
| Engineering | 202 | 109 | 2.6 | as Leatherworking: stone and bars come from Mining. | high |
| Blacksmithing | 164 | 94 | 2.1 | as Engineering; recipes also need an anvil or forge (craft_walk, seen for bots 1192, 1271, 1280). | high |
| Alchemy | 171 | 94 | 1.0 | herbs come from Herbalism; the vial is sold by vendors but `kSkillUp` never names a missing reagent (only `kCraft` does, `NoteMissingReagent`), so no supply trip buys it. | high (core code) |
| Tailoring | 197 | 98 | 28.8 (48) | the one crafting line that works: linen cloth drops from humanoids, Bolt of Linen Cloth needs nothing else. | high |
| Enchanting | 333 | 98 | 1.0 | not a craft line at all: the core's `IsCraftSkill` leaves it out (its recipes work on an item; disenchanting is not a bot action). Not changed here. | high |
| Fishing | 356 | 168 | 1.0 | `Fishing = 0` on prod; the cast needs a core action (doc/fishing.md). Not changed here. | high |
| Cooking | 185 | 800 | 37.8 | works: meat drops, the bot lights a campfire. Unchanged. | high |
| First Aid | 129 | 800 | 58.8 | works: cloth drops, bandages need nothing else. Unchanged. | high |

Other points found in the code:

- **No level gate, no missing recipes.** The core teaches every rank and recipe of the bot's professions for free at login and level-up
  (`BotProfessions::Teach`); the rank maximum (75, 150, ...) is not what holds the skills at 1.
- **The core's recipe choice** (`ChooseSkillUpRecipe`): among the recipes the bot knows that still raise the skill and whose materials it carries, the one with
  the highest `MinSkillLineRank`. In this client's data `MinSkillLineRank` is 1 for nearly every recipe, so the choice is close to arbitrary among those; it
  never names what is missing.
- **The gather walk loop.** With `GatherWalk = 1`, a walk that arrives at an empty node (4,859 of 5,300 ore and herb spawns are pooled, the table lists every
  point) gets "nothing near", and the walk logic picks the nearest node again, which is the one the bot stands on (`GatherWalkGapSec` defaults to 0). The sim
  side test (2026-11-18: 462 walks, 614 refusals, skills about 1) fits this. Unchanged when `Professions` is off; with it, the node is marked empty.
- **Skinning reach.** The core skins any looted skinnable corpse within 30 yards at any skill; the skill gain depends on the creature level
  (`EffectSkinning`: level 10 or below needs 1, below 20 `(level - 10) x 10`, up to 73 `level x 5`), so a bot that kills its own level keeps rising.

## What `Professions = 1` does

All of it is off by default. With `Professions = 0` nothing changes: no new action, no new line, the old `Gathering`/`GatherWalk` paths as before.

1. **Skill cap.** A primary profession whose value is at `ProfessionSkillCapPerLevel` x level (default 5 x level), or at the rank maximum, gets no new
   `kSkillUp`, `kGather`, skinning ask or node walk until the bot levels; one `profession_cap` line per cap. Cooking and First Aid have no cap here.
2. **Gathering asks** as with `Gathering = 1` (Mining, Herbalism, Skinning within the core's 30 yards), so `Professions = 1` works with `Gathering = 0`.
3. **Skinning after a loot** (`ProfessionSkinAfterLoot`): the tick a bot loots a corpse it fought, its skinning ask is set 1.5 s later and the back-off
   count starts again. The core picks the nearest looted skinnable corpse within 30 yards.
4. **Safe node walks** (`ProfessionNodeWalk`, needs `GatherNodesFile`): after "nothing near", a walk to the nearest node of the bot's skill that
   - lies at most `ProfessionNodeMaxYards` (150) from the bot, so the walk stays beside the leveling path;
   - passes the node table's own filter (required skill, area level at most 3 above the bot, faction);
   - is not too high for the bot: the level-safe map (`level_safe.h`, from the grind spots) at the node and every 100 yards along the straight
     line there, margin `LevelSafeMarginLevels` (the level map is built when `Professions = 1`);
   - is not dangerous: the shared danger map (`danger_map.h`) at the node and along the line (the map holds deaths only while `DangerMap` or `SafeRevive` is on);
   - was not found empty by any bot in the last `ProfessionNodeEmptyMin` (20) minutes;
   - and the bot is not in a group. One walk per `ProfessionNodeGapSec` (300 s). The walk itself is the existing gather walk (idle and healthy only, a
     no-route or a 150 s timeout ends it); the death, revive and path code is not touched.
5. **Craft plan** (`ProfessionCraftPlan`, needs `ProfessionRecipeFile`): when the core answers a `kSkillUp` of a crafting profession (Blacksmithing,
   Leatherworking, Alchemy, Tailoring, Engineering, Smelting) with "nothing to make", the next ask names one recipe (`kCraft`): the easiest of the table that
   still raises the skill (`value < trivialHigh`, lowest `trivialLow` first). The core then crafts it if the materials are there, or notes the first missing
   reagent, which the supply trip buys when a vendor sells it and `SupplyReagents = 1`. A recipe the bot does not know (`kNotKnown`) is skipped and the next
   one named (6 in a row at most, then the line waits 5 minutes; the list starts again at the next level). Any other answer is read as a `kSkillUp` answer
   (`kNeedsFocus` starts the anvil or forge walk).

## Keys

`Module.forever-bots.<Key>` in `modules.d/forever-bots.conf`. Per the config rule, every key goes into each env's conf (prod keeps `Professions = 0`).

| Key | Default | Range | Meaning |
|---|---|---|---|
| `Professions` | 0 | 0/1 | master switch |
| `ProfessionSkillCapPerLevel` | 5 | 1..75 | cap = this x level, never above the rank maximum |
| `ProfessionSkinAfterLoot` | 1 | 0/1 | skin the corpse just looted |
| `ProfessionNodeWalk` | 1 | 0/1 | safe walks to near nodes (needs `GatherNodesFile`) |
| `ProfessionNodeMaxYards` | 150 | 20..600 | farthest node |
| `ProfessionNodeGapSec` | 300 | 0..86400 | at least this between node walks (the larger of it and `GatherWalkGapSec`) |
| `ProfessionNodeEmptyMin` | 20 | 1..1440 | a node found empty is skipped by every bot this long |
| `ProfessionCraftPlan` | 1 | 0/1 | name a recipe when `kSkillUp` had nothing |
| `ProfessionRecipeFile` | "" | path | `data/profession_recipes.txt`, absolute path; empty = no plan |

Sim example:
```
Module.forever-bots.Professions = 1
Module.forever-bots.GatherNodesFile = /srv/forever/sim/modules_runtime/forever-bots/data/gather_nodes.txt
Module.forever-bots.ProfessionRecipeFile = /srv/forever/sim/modules_runtime/forever-bots/data/profession_recipes.txt
Module.forever-bots.SupplyReagents = 1
```
`Gathering` and `GatherWalk` may stay 0. `Crafting = 1` (default) is needed for the craft plan.

## Events

Each through the module's event journal (`event <name> ...` as `craft_walk` today):

| Line | When |
|---|---|
| `profession_skillup: bot=G skill=S from=A to=B` | a primary profession's value rose between two snapshots (the snapshot carries the two primaries only: no Cooking or First Aid lines) |
| `profession_cap: bot=G skill=S value=V cap=C level=L` | a line reached its cap (once per cap value) |
| `profession_skin: bot=G corpse_level=L` | a skinner looted a corpse; the skinning ask follows |
| `profession_node_walk: bot=G skill=S node=E yards=Y skipped=K` | a safe node walk began; K = nodes nearer than the chosen one left out (empty, level, danger) |
| `profession_node_empty: bot=G skill=S node=E kept=N` | a walk arrived and the core found nothing: the node is skipped by all; N nodes are marked |
| `profession_craft_plan: bot=G skill=S recipe=R status=applied|refused|not_known|needs_focus|other` | the answer to a named recipe |

`profession_skin` comes once per looted corpse of a skinner: the busiest line.

## Measuring on the sim

Read-only, from the build host, output to stdout only (no file on the host):

```
ssh -o BatchMode=yes forever-admin@192.168.34.62 'sudo -n mysql -t' <<'SQL'
SELECT cs.skill,
       CASE cs.skill WHEN 164 THEN 'Blacksmithing' WHEN 165 THEN 'Leatherworking' WHEN 171 THEN 'Alchemy' WHEN 182 THEN 'Herbalism'
                     WHEN 186 THEN 'Mining' WHEN 197 THEN 'Tailoring' WHEN 202 THEN 'Engineering' WHEN 333 THEN 'Enchanting'
                     WHEN 393 THEN 'Skinning' WHEN 356 THEN 'Fishing' WHEN 185 THEN 'Cooking' WHEN 129 THEN 'First Aid' END AS profession,
       COUNT(*) AS bots,
       ROUND(AVG(cs.value), 1) AS avg_value,
       MAX(cs.value) AS max_value,
       SUM(cs.value > 1) AS above_1,
       SUM(cs.value >= LEAST(cs.`max`, 5 * c.level)) AS at_cap,
       ROUND(AVG(c.level), 1) AS avg_level
FROM forever_test_characters.character_skills cs
JOIN forever_test_characters.characters c ON c.guid = cs.guid
JOIN forever_test_auth.account a ON a.id = c.account
WHERE a.username LIKE 'BOT%'
  AND cs.skill IN (129, 164, 165, 171, 182, 185, 186, 197, 202, 333, 356, 393)
GROUP BY cs.skill
ORDER BY cs.skill;
SQL
```

Run it at the start of the run and after 2 to 3 hours (or per arm with `ArmSplit`: add `AND MOD(c.guid, 2) = 1` for the odd guids when the split is by
parity; a strata split needs the arm tag from the journal). Success: Mining, Herbalism and Skinning `avg_value` and `above_1` clearly up from about 1 and 0;
the crafting lines up where their gatherers are; `at_cap` small; deaths per bot-hour and `no_route` counts not worse than the arm without `Professions`.
Journal counts per event: `grep -c 'profession_skillup'` (and the other names) over the run's log.

## Unverified

- Nothing here ran on the sim or prod; the policy tests use made-up snapshots.
- The skinning ask after a loot assumes the core's loot leaves the corpse looted (`isLooted`) and skinnable a tick or two later.
- `data/profession_recipes.txt` comes from the client DB2 files, not the server's hotfix rows; recipes Forever changed may differ (the core checks every
  `kCraft`). `minSkill` is 1 for nearly every row in this client, so the plan orders by `trivialLow`; which of those recipes the bots really know comes only
  from the `kNotKnown` answers.
- Whether the noted reagent is bought: depends on `SupplyReagents = 1`, a vendor that sells it within the core's search, and the supply trip's own rules.
  Ore, herbs and leather are never sold by vendors, so for most crafting lines the plan helps only once the gatherers deliver (mail of materials,
  `BotMailMaterials`, is separate).
- The level-safe and danger checks use the straight line to the node; the core's path may bend. With `DangerMap` and `SafeRevive` off the danger map is empty.
- The level map is built from arm A's options; an arm B with `Professions = 1` and arm A without it walks with the node table's area-level filter only.
- The source of the small prod rises in Leatherworking, Engineering and Blacksmithing.
