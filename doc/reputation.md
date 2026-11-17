# Reputation

Two parts, both off by default: the **vendor goals** (a bot buys reputation-gated gear; built earlier, section "Reputation goals") and the **planner** (a bot picks a progression-path faction and a source to earn it; section "The planner"). Core support missing for both is collected under "What the core must still provide" at the end.

## Reputation goals

Status: module side done, off by default (`ReputationGrind = 0`). The core does not send the facts it needs yet, so with the key on nothing happens until it does.

## Idea

Bots at a level pick one faction whose reputation gates equipment, earn the reputation with the quests, dungeons and kills they already play (the existing quest lists and
level dungeons give reputation as a side effect), and walk to the faction's vendor when the core says that vendor sells them a reputation-gated upgrade they may buy now. The
buy is the core's: it picks the best upgrade by its gear score (the same scoring as the equip pass, by the numbers, never by item id) and checks the rank at the vendor.
The module never names an item.

## What the sim data holds (2026-11-14)

`tools/gen_reputation_goals.py` reads the hotfix `item_sparse` gate (`MinFactionID`, `MinReputation`), `npc_vendor` and the quest reward factions, and writes
`data/reputation_goals.txt`. Result: 6 factions, and only one has a vendor in `npc_vendor` (Nightclaw Druids, one level 15 piece at Exalted). The classic reputation vendors
(Argent Dawn, Thorium Brotherhood, Timbermaw, Cenarion Circle, ...) sell recipes, patterns and quest items in this database, not gear, and the Darkspear Raiders / Theramore
Expeditionary Force gear (level 28 to 60 cloaks, weapons, gloves and shoulders at Honored to Exalted) has no vendor. The hotfix table holds only 117 gated items: the base
`ItemSparse.db2` is a sparse table the repository's reader cannot read, so gated classic items that exist only there are not in the list. A faction with no vendor is never a goal, so
the feature is idle by data today and starts to work as vendors are added (run the tool again; nothing is selected by item entry).

## What was built (module)

- `src/reputation.{h,cpp}`: the ranks (the core's `ReputationRank`, thresholds -42000, -6000, -3000, 0, 3000, 9000, 21000, 42000), the goal table parser, the goal choice and the visit rules.
- Goal choice: a faction of the bot's side, with gear within 5 levels of the bot's level, with a vendor, not yet Exalted; the one with the most reputation quests, ties spread by a hash of the bot's guid; kept while it stays a candidate.
- `BotPolicy::TurnReputation` (`src/bot_policy_reputation.cpp`): an idle, solo bot out of combat whose core facts name a reputation vendor walks there (`rep_vendor` move, 3 minute limit) and sends `kVendor` with the buy bit. Judged 7 seconds later: less money or fewer free slots is a purchase; otherwise the next visit waits 30 minutes, doubling up to 6 hours.
- Journal lines: `rep_goal`, `rep_buy`, `rep_buy_done`, `rep_buy_failed`.
- Keys: `ReputationGrind` (0), `ReputationMinLevel` (10), `ReputationTripSec` (600), `ReputationMinCopper` (10000), `ReputationGoalFile`.

## What the core needs (not done, protocol 29 stays compatible)

1. Snapshot facts, appended per the layout 25 growth rule (a new section or a tail on `BotSnapshot`; the Tier 2 layout version goes up, the base stays):
   - `rep_vendor_valid`, `rep_vendor_x/y/z`, `rep_vendor_guid`: the nearest vendor on the bot's map that sells a reputation-gated piece the bot can buy now (`Player::GetReputationRank(faction) >= ItemTemplate::GetRequiredReputationRank()` with `GetRequiredReputationFaction()`), usable by the bot's class and better than what it wears by the core's gear score. Same shape as `ammo_vendor_*`, `goods_vendor_*`.
   - Optionally a standings section: `{faction id, standing int32, rank uint8}` for every faction with a non-zero standing (`ReputationMgr::GetReputation(factionId)`, `GetRank`). The module only needs it to drop Exalted goals; the ranks are read from `ReputationMgr` thresholds, which the PvP rank track of the Karrz1 merge changes for faction 2800 (renown), so the PvP faction must be left out of the block or reported with its own curve.
2. `kVendor` bit 32768 (`spell_id`; bits 12 to 14 carry the gear profile as for bit 2048): buy, at the vendor in `target_guid`, the best reputation-gated upgrade by the gear score, one piece per action, keeping the money reserve; refused with a reason when none is buyable. Documented in `tier2_layout.h` next to the other service bits, config key on the core side (for example `Bot.Vendor.BuyReputationGear`, off by default).
3. Wire `PolicyOptions::ReputationFacts` in `main_options.cpp` to the new snapshot fields once the SDK has them (today it is empty on purpose).

## Not done / unverified

- Nothing was run against a core: the policy test uses a fake facts provider. The judge rule (money or slots went down) is the only signal the module has without a vendor result.
- Faction side is read from the parent chain in `Faction.db2` (Alliance 469, Horde 67); the SoD-style factions have neither parent, so they are side 0 (both).
- Steering the earning (preferring dungeons or quests of the goal faction) is not built: the goal is only logged and used to choose vendors. The data gives the number of reputation quests per faction, which a later quest-planner weight can use.

## What the module and the core already know (2026-11-16 survey)

- Module: `git grep -i reputation` finds only this feature, the PvP ladder generator (`data/pvp/`, Marks and battleground reputation vendors), `doc/pvp-track.md` (battleground factions are the usual reputation system, "not read by the module yet") and `doc/pre-raid-progression.md` (reputation items wait for the reputation, phase 4, "standing per faction: not in the snapshot"). `AutoGearing` (core) gives gear by level and never reputation gear. `kQuest` plays quests; quest rewards of reputation are a side effect.
- Core (read only, `Trinity-Forever-Modules`): `ReputationMgr` holds the standings; the Classic 1.60 layer changes faction 2800 (PvP rank points, doc/pvp-track.md); the bot snapshot has no standing.
- World DB (sim, `forever_test_world`, read only): `creature_onkill_reputation` 2119 rows (creature, up to 2 factions, value, `MaxStanding` = highest rank the kill still pays up to, `IsTeamAward`); `quest_template` 18354 of 48858 quests reward reputation (`RewardFactionID1..5`, `RewardFactionValue` index into QuestFactionReward 10/25/75/150/250/350/500/1000, `RewardFactionOverride` in hundredths, `RewardFactionCapIn` the rank cap); `quest_objectives` type 1 = item hand-in, type 0 = kills. The world DB has no creature levels (scaling comes from client data) and no faction templates beyond the ids; faction names and parents come from `Faction.db2`.

## Which reputations matter on the path

| Stage | Faction (id) | Why | Goal rank | Sources in the data (quests / kills) |
|---|---|---|---|---|
| Pre-raid, Naxxramas/Scholomance | Argent Dawn (529) | attunement, Revered gear, Scourgestone hand-ins | Revered | 95 / 164 |
| Winterspring, Felwood | Timbermaw Hold (576) | Furbolg access | Friendly | 9 / 17 |
| ZG, Onyxia period | Zandalarian Emissaries (270) | ZG coins and quests, enchants | Exalted | 51 / 41 |
| Silithus, AQ attunement | Cenarion Circle (609) | Revered gear, AQ chain | Revered | 90 / 36 |
| MC, BRD | Thorium Brotherhood (59) | recipes, Honored | Honored | 5 / 0 |
| MC | Hydraxian Waterlords (749) | Hydraxis quests | Honored | 0 / 23 |
| AQ | Brood of Nozdormu (910) | access, Scarab Lord | Friendly | 13 / 18 |

Battleground factions (Stormpike 730, Frostwolf 729, Silverwing 890, Warsong 889, League of Arathor 509, Defilers 510) are not planned: the core pays them at the end of a battle (`doc/pvp-track.md`), kills inside a battleground give nothing of their own, so the PvP track earns them by queueing, which the module already does. Lower City and the other Outland factions do not exist in 1.60. Tabards are not used: the world DB has no tabard reputation rule beyond the dungeon kills, and the item is an equip choice the bot cannot make by id.

Activities that pay: quests (the quest planner already walks most of them), item hand-ins (quests with an item objective: Scourgestones, Cenarion Circle mission items, Zandalar coins), kills of faction creatures (`creature_onkill_reputation`; `MaxStanding` ends the pay at a rank), dungeon runs (the kills inside; they are in the data but belong to the dungeon planner, so the planner takes open-world kills only, maps 0 and 1).

## The planner

Files: `data/reputation_targets.txt` (hand written: faction, side, minimum level, goal rank, tier), `data/reputation_sources.txt` (made by `tools/gen_reputation_sources.py` from the world DB and the quest levels of `data/world_quests*.txt`, so every listed quest is one the quest planner can walk), `src/reputation_plan.{h,cpp}`, `tests/reputation_plan_test.cpp`, hook in `BotPolicy::TurnReputation`.

- Target: a faction of the bot's side and level, below its goal rank, with sources; lowest tier first; ties spread by a hash of the guid (never a random number); kept while it stays a candidate.
- Source: among at most `ReputationPlanBudget` sources of that faction (a cursor moves on at every plan so large factions are covered over several plans): side and level fit, the rank cap not reached, not given up before; quests over kills, then the biggest reward, then a guid hash.
- State machine per bot (`Track`): idle -> active (a source is chosen) -> idle when the source stops paying (rank cap reached) or when the standing has not risen for `ReputationStallSec` (the source goes to a memory of 8 given-up sources). Without standings from the core nothing can be observed, so nothing is given up.
- Caps: a bot plans at most every `ReputationPlanSec`; a plan looks at no more than `ReputationPlanBudget` sources; a bot with an active plan costs one list search of its faction.
- Output today: a `rep_plan` journal line (`bot`, `level`, `faction`, `quest=` or `kill=`, `points`) when the plan changes. The quest planner does not yet take a bias, so the plan does not move the bot (see below).
- Keys (all in `config/modules.d/forever-bots.conf.example`): `ReputationPlan` (0), `ReputationPlanBudget` (64), `ReputationPlanSec` (120), `ReputationStallSec` (1800), `ReputationTargetFile`, `ReputationSourceFile`; needs `ReputationGrind = 1`.
- Regenerate: `tools/gen_reputation_sources.py --mysql "ssh forever-test sudo -n mysql" --out data/reputation_sources.txt` (SELECT only). 2026-11-16 sim result: 263 quests, 299 kill sources.

Cost (local, Release, 800 bots of level 50 to 60, `reputation_plan_test` prints it): the first plan of a bot 1 to 2 us (35582 sources looked at for all 800, at most 64 each), a steady-state call 0.1 us per bot, about 0.1 ms per tick for 800 bots, which is far below one percent of a tick. Budget 64 and a plan every 120 s keep it flat as the data grows.

## What the core must still provide (nothing was changed in the core)

1. Standings in the snapshot (see the vendor section above): per faction `{id, standing, rank}`; without them the planner cannot see progress, drop finished sources or end a goal. The PvP faction 2800 needs its own curve.
2. The vendor visit facts and the `kVendor` buy bit (above).
3. A bias or goal call for the quest planner: either the module's quest planner takes `preferred quest ids` (module work, next step: the planner's candidate scan adds a weight for the quests in the active `Track`), or the core accepts a `kQuest` operation for a given quest id from anywhere. Creature kills need the grind spot code to accept a creature entry as the spot (module work, `grind_spots`).
4. A creature level source: the world DB has none for creatures, so open-world kill sources cannot be level-filtered; the core could report the target's level in the unit snapshot (already there for units in range) or the tool could read it from the DB2 scaling tables.
5. Item hand-ins: the quest objective items must be in the bot's bags; the core's loot or a vendor purchase has to supply them (Scourgestones drop from Undead in the Plaguelands: a kill source of the Argent Dawn is the realistic supply).
6. Dungeon reputation (Stratholme, Scholomance for Argent Dawn; ZG for the Zandalarian) needs the dungeon planner to prefer those dungeons; the core already pays the kills.
