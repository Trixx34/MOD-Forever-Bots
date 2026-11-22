// SPDX-License-Identifier: GPL-2.0-or-later
// BotSettings: the module's own settings, read from its ModuleConfig (the core sends Module.forever-bots.<Key> with the
// prefix removed; see modules.d/forever-bots.conf.example). Pure logic, unit-tested in tests/settings_test.cpp.
#pragma once

#include <memory>
#include <unordered_set>

#include "ab_split.h"
#include "battlegrounds.h"
#include "repair_policy.h"
#include "quest_group.h"
#include "party_reason.h"
#include "bot_guilds.h"
#include "duels.h"
#include "errand_policy.h"
#include "guild_join.h"
#include "inn_downtime.h"
#include "bot_mail.h"
#include "mail_gifts.h"
#include "auction_policy.h"
#include "bot_brackets.h"
#include "bot_presence.h"
#include "path_variety.h"
#include "bot_ramp.h"
#include "sell_policy.h"
#include "supply_policy.h"
#include "reputation_plan.h"
#include "lfg_queue_fill.h"
#include "death_pause.h"
#include "trade_ads.h"
#include "group_etiquette.h"
#include "party_mana.h"
#include "whisper_matrix.h"
#include "path_jitter.h"
#include "personality.h"
#include "guild_milestone.h"
#include "city_inspect.h"
#include "transport_ride.h"
#include "city_greet.h"
#include "meet_greet.h"
#include "quest_invite.h"
#include "realism.h"
#include "taxi_policy.h"
#include "world_events.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

// A scenario (doc 19): a repeatable test situation. Config keys (all optional; Scenario names it and turns it on):
//   Scenario = deadmines_a            ScenarioRoster = "1:1,1:5,7:8"   (race:class for each bot)
//   ScenarioLevel = 18                ScenarioStaging = "0 -8913 554 94"  (map x y z: where the bots log in)
//   ScenarioEntry = "36 -14.57 -385.48 62.46"  (map x y z: where the party is teleported to)
//   ScenarioDurationSec = 1200        ScenarioRoute<n> = "x y z" (n counts from 1)
//   ScenarioGear<classId> = "item,item,..."  (optional: items granted instead of auto gear for that class)
//   ScenarioGearFile = /path/data/prebis  (optional: a folder with <classId>.txt, one item entry per line, # comments; used for each class without its own
//                                         ScenarioGear<classId>; see doc/pre-raid-gear.md)
//   ScenarioBosses = "644:Rhahk'Zor,643:Sneed,..."  (creature entry[:name] of each boss; the result counts the ones seen dead)
//   ScenarioResultFile = "scenario-results.log"
// Dead-bot recovery (a dead bot releases its spirit, walks back to its corpse and reclaims it; a living priest, paladin or shaman may
// resurrect it first):
//   ScenarioWipeRecovery = 0          1: when every bot is down, the run waits for one to come back (ghosts walk back by themselves) instead of
//                                     ending at once; 0 (default): all down ends the run as "wipe"
//   ScenarioWipeRecoverySec = 300     how long an all-down state may last with ScenarioWipeRecovery = 1 (10..3600)
//   ScenarioPortal = "0 -11.0 -385.0 62.0"  (map x y z: the instance door on the outside map, where a ghost walks to before it enters; the
//                                     inside map is ScenarioEntry's. Without it a ghost released outside cannot go back to a corpse inside)
//   ScenarioMinAlive = 0              the run ends as "understrength" when fewer bots than this are alive for a while (0 = off, default)
//   ScenarioLeaderDeadAbandonSec = 0  the run ends as "abandoned_dead_leader" when the party leader stays down this long (0 = off, default)
//   ScenarioBossAt = "6:644,11:643"   (route point number from 1 : boss entry) the leader holds at such a point until that boss is dead
//   ScenarioRequireBosses = 0         1: the run is complete when every boss of ScenarioBosses has been seen dead (not when the last waypoint is reached)
// A dungeon file (doc/dungeons.md) fills most of these keys: DungeonFile = "data/dungeons.txt" and Dungeon = "rfc" (see dungeon_data.h).
// Several dungeons in one run (bots 0.29.0): Scenario = "dm:10,rfc:10" lists dungeon ids of the dungeon files, each with its number of parties, in order (the
// first one's parties are spawned first); at most 40 parties in all. Each listed dungeon takes all its scenario keys from its dungeon file entry; a key with
// ".<id>" after it changes one key for that dungeon only: ScenarioRoster.dm = "1:1,1:5,1:8,3:3,7:9", ScenarioLevel.dm = 18, ScenarioStaging.dm = "0 -8913 554 94".
// In the list form the plain dungeon keys (ScenarioRoster, ScenarioLevel, ScenarioStaging, ScenarioEntry, ScenarioPortal, ScenarioRoute<n>, ScenarioBosses,
// ScenarioBossAt, ScenarioKillPriority, ScenarioParties and Dungeon) are ignored, with a warning: they belong to one dungeon. The other keys (ScenarioDurationSec,
// ScenarioGear<class>, ScenarioSupplies, ScenarioWipeRecovery*, ScenarioMinAlive, ScenarioResultFile, ...) apply to every dungeon of the list. A single name
// without ":n" is the single form, as before, with ScenarioParties. A roster is one faction (a party cannot mix the sides): a roster with both Alliance and Horde
// races is refused with a warning, and in the list form every roster must have the size of the first one's (the parties are cut by one size).
struct ScenarioSettings
{
    std::string Name;                                    // empty: no scenario
    std::vector<std::array<std::uint32_t, 2>> Roster;    // race, class
    std::vector<std::string> RosterSpec;                  // the spec of each roster entry (race:class:spec), "" for none; the same size as Roster
    std::uint32_t Level = 18;
    bool HasStaging = false;
    std::array<float, 4> Staging{};                      // map, x, y, z
    // Where a hunter without a pet goes to tame one before the party forms (ScenarioTameSpot = "0 -10439 929 39": map x y z, a place with tameable beasts near the hunters' level,
    // such as Westfall); the hunters are taken there after the dressing and back. Without it hunters without a pet stay where they are.
    bool HasTameSpot = false;
    std::array<float, 4> TameSpot{};
    bool HasEntry = false;
    std::array<float, 4> Entry{};
    std::uint32_t DurationSec = 1200;
    std::vector<std::array<float, 3>> Route;
    std::map<std::uint32_t, std::vector<std::uint32_t>> Gear;
    // Food and drink (and anything else) every bot of the scenario is given, after its gear: item entry and count (ScenarioSupplies = "4540:5,1205:5").
    std::vector<std::pair<std::uint32_t, std::uint32_t>> Supplies;
    // Per class (ScenarioClassSupplies = "9:6265:10": class:item:count, comma separated; ScenarioClassQuests = "9:1504,3:6061": class:quest). The defaults give a
    // warlock Soul Shards (the Voidwalker and Succubus summons use one each). ScenarioClassQuests (off by default: the core refuses a quest that is not in the log, and the
    // refusals paused the bots' quest planning) sends core quest operation 4. ScenarioAmmo is the count of ammunition a hunter without any is given per grant (0 = none; at most 200, a stack).
    std::map<std::uint32_t, std::vector<std::pair<std::uint32_t, std::uint32_t>>> ClassSupplies{ { 9, { { 6265, 10 } } } };
    std::map<std::uint32_t, std::vector<std::uint32_t>> ClassQuests;
    std::uint32_t Ammo = 200;
    std::vector<std::pair<std::uint32_t, std::string>> Bosses; // creature entry, name
    std::vector<std::pair<std::uint32_t, std::uint32_t>> BossAt; // route point (0-based) and the boss entry standing there (ScenarioBossAt)
    std::string ResultFile = "scenario-results.log";
    bool WipeRecovery = false;                           // all down is not the end while a ghost may still come back
    std::uint32_t WipeRecoverySec = 300;
    std::uint32_t MinAlive = 0;                          // 0 = the understrength stop is off
    std::uint32_t LeaderDeadAbandonSec = 0;              // 0 = off
    bool RequireBosses = false;
    // ScenarioParties (1 to 40, default 1): this many parties of the roster run at the same time, each with its own director and its own dungeon instance (the core makes an
    // instance per group). Bot guids in order are cut into parties of the roster's size, as the policy's party logic does. MaxBots must cover Parties x roster.
    std::uint32_t Parties = 1;
    // ScenarioKillPriority = "entry,entry,..": creature entries in the order the party kills them (the dungeon file's kill_priority).
    std::vector<std::uint32_t> KillPriority;
    // ScenarioSummons = "entry,entry,..": the creature entries this dungeon's summoners make (the dungeon file's summons). Used only when DungeonSummons lists the dungeon.
    std::vector<std::uint32_t> Summons;
    bool HasPortal = false;
    std::array<float, 4> Portal{};                       // map, x, y, z: the instance door, outside
    // ScenarioRaid (0 or 1, default 0; doc 108): a raid scenario's roster may go up to 40 instead of 5, formed into one raid group of
    // 8 subgroups of 5 instead of a flat party (bot_policy.cpp). A five-man scenario (ScenarioRaid unset or 0) is capped at 5 exactly as
    // before -- this flag, not a universal cap raise, is what tells ReadOneScenario and the policy a roster is a raid. The roster is
    // expected to be 8 repeats of the dungeon-style tank/healer/dps composition, one repeat per subgroup, in ScenarioRoster order.
    bool Raid = false;
};

struct BotSettings
{
    // Most bots this module controls at once. Never more than kMaxBotsCeiling (the Tier 2 request is sized to it).
    std::uint32_t MaxBots = 1;
    // Race and class ids, or 0 for "random" (config value random): a random valid combination, see bot_pick.h.
    std::uint32_t Race = 1;   // Human
    std::uint32_t Class = 1;  // Warrior
    // Seed for the random picks. The same seed gives the same sequence of bots.
    std::uint32_t RandomSeed = 1;
    // Spell cast at the attack target on a timer (0 = off). A test of the cast path and its refusal reports, until the
    // class rotations exist. Config key TestSpell.
    std::uint32_t TestSpell = 0;
    // How many bots the ramp wants over the run (config keys BotsInitial, BotsStep, BotsStepMinutes, BotsCap, BotsStopTime).
    RampPlan Ramp;
    // The telemetry CSV the bots write to (config key TelemetryFile, relative to the module's working directory). Empty
    // turns it off. The file is rotated at TelemetryMaxMB, keeping TelemetryKeep older files.
    std::string TelemetryFile = "forever-bots-telemetry.csv";
    std::uint32_t TelemetryMaxMB = 64;
    std::uint32_t TelemetryKeep = 5;
    // The live picture (positions of the bots and what is near them) for the dashboard's map: a JSON file rewritten every
    // LiveEveryMs (config keys LiveFile, LiveEveryMs 250..10000). Empty LiveFile (default) turns it off.
    std::string LiveFile;
    std::uint32_t LiveEveryMs = 1000;
    // Seconds between summary lines per bot (config key TelemetrySummarySec, 1..3600).
    std::uint32_t TelemetrySummarySec = 60;
    // Minutes without progress before an alive, idle bot is tagged as stalled in telemetry (config key StallAfterMinutes, 0..1440;
    // 0 = off, the default). Tagging only.
    std::uint32_t StallAfterMinutes = 0;
    // Seconds between vendor visits (sell grey, repair, equip), for an idle bot (config key ServiceEverySec, 60..86400). The
    // trainer check (config key TrainEverySec, 60..86400, also at once after a level-up) is off by default (0): bots learn
    // their spells on level-up, so they do not need a trainer.
    std::uint32_t ServiceEverySec = 600;
    std::uint32_t TrainEverySec = 0;
    // Hunter taming (config key TameDeadPetBackoffSec, 60..86400; 0 = off, the default). When Tame Beast is refused because a dead stored pet fills the pet slot and
    // neither Call Pet nor Revive Pet works (Revive Pet not learned), the hunter stops the Tame / Call Pet / Revive Pet loop and waits this long, with a
    // "tame_failed ... why=pet_dead_no_revive" event, instead of retrying every 20 seconds to 10 minutes. Off keeps the old loop.
    std::uint32_t TameDeadPetBackoffSec = 0;
    // Parties (M8): PartySize bots per party (config key PartySize, 0 = no parties, at most 5) and PartyCount parties,
    // cut from the lowest-guid bots (config key PartyCount, 1..100).
    std::uint32_t PartySize = 0;
    std::uint32_t PartyCount = 1;
    // Randomness (bot_random.h): PartyPercent is the share of the bots that take part in the parties at all (config key PartyPercent, 0..100); the others
    // play on their own, and the parties are of random sizes from 2 to PartySize. RandomPct is how much the bots' timers and walks vary, +- this many percent
    // (config key RandomPct, 0..90, 0 = none). Both follow RandomSeed, so a run with the same seed behaves the same.
    std::uint32_t PartyPercent = 30;
    std::uint32_t RandomPct = 25;
    // Hubs (M7): where an idle bot walks when it has seen nothing for a while, per map, in the order given by n. Config keys
    // Hub<map>_<n> = "x y z" (n counts from 1).
    std::map<std::uint32_t, std::vector<std::array<float, 3>>> Hubs;
    // Quests (M5): the starter quest data file (config key QuestFile, see quest_data.h). Quests run when this or any of the four per-side keys below is set; with none of the five set, quests are off.
    std::string QuestFile;
    // Per faction and band (config keys QuestFileAlliance, QuestFileHorde, QuestFileAlliance60, QuestFileHorde60): a bot of that side uses the list; from level 60 the
    // "60" list. Empty keys fall back (level 60 -> the side's leveling list -> QuestFile).
    std::string QuestFileAlliance, QuestFileHorde, QuestFileAlliance60, QuestFileHorde60;
    // The Onyxia's Lair attunement (config key AttunementFile, data/attunement_quests.txt beside the module): its quests replace the same quests in
    // QuestFile and are taken at level 60 only. Empty turns the attunement off. Used only when QuestFile is set.
    std::string AttunementFile = "data/attunement_quests.txt";
    // QuestTeleport (0 or 1, off by default): a quest walk that has not moved for two minutes teleports to the giver. Needs
    // Bot.Actions.Teleport = 1 in the core. Off, that start is dropped and the bot grinds instead of retrying a refused teleport.
    bool QuestTeleport = false;
    // SkyborneExitQuests (0 or 1, off by default): Skyborne bots on map 2991 take the exit quests ('hop' records: 94946 to Dalaran, 95349 to Mulgore), walk to
    // the dock and are teleported to map 0 or 1 to hand them in. Needs Bot.Actions.Teleport = 1 in the core; a refused teleport blocks the quest (event).
    // The same key: in Dalaran the Alliance bots take 94947 and use the portal to Stormwind ('use' record; Bot.Actions.UseObject = 1), a teleport as fallback.
    // SkyborneExitMakeRoom (0 or 1, off by default): such a bot with full bags visits a vendor before it takes an exit quest (or 93089/93090).
    bool SkyborneExitQuests = false;
    bool SkyborneExitMakeRoom = false;
    // Off-map recovery (off_map.h): OffMapRecover (0 or 1, default 1) teleports a living bot that stood more than OffMapMarginYards (default 500, 100..5000)
    // outside the known places of its map (quest givers and anchors, grind spots, hubs) for OffMapSeconds (default 60, 10..3600) to the nearest known place of
    // its faction, at most once per OffMapCooldownSec (default 600, 60..86400). Needs Bot.Actions.Teleport = 1 in the core (refused otherwise, and logged).
    bool OffMapRecover = true;
    std::uint32_t OffMapSeconds = 60;
    std::uint32_t OffMapCooldownSec = 600;
    std::uint32_t OffMapMarginYards = 500;
    // Level-safe places (2026-11-18, level_safe.h, doc/level-safe.md). The level of a place is the lowest minimum level of the grind spots within
    // LevelSafeRadiusYards (default 250, 50..1000; two spots at least, else unknown and never judged); a place is too high for a bot when that is above its level
    // plus LevelSafeMarginLevels (default 5, 1..30) for a walk to a quest giver or hunting area, plus LevelSafeEscapeMarginLevels (default 10, 1..40, never below
    // the other) for a hand-in walk, the escape and the graveyard.
    // LevelSafeWalks (default 1): a quest walk to or through such a place is dropped (event quest_walk_blocked reason=level_unsafe). LevelSafeGraveyard
    // (default 1): a ghost whose corpse or graveyard lies too high is revived by the spirit healer of the nearest graveyard seen that fits its level (event
    // level_safe_yard). LevelSafeEscape (default 1): a solo bot out of combat standing in such a place is teleported to a grind spot for its level, at most once
    // per LevelSafeEscapeCooldownSec (default 600, 60..86400; event level_unsafe_escape; needs Bot.Actions.Teleport, as OffMapRecover).
    bool LevelSafeWalks = true;
    bool LevelSafeGraveyard = true;
    bool LevelSafeEscape = true;
    std::uint32_t LevelSafeMarginLevels = 5;
    std::uint32_t LevelSafeEscapeMarginLevels = 10;
    std::uint32_t LevelSafeRadiusYards = 250;
    std::uint32_t LevelSafeEscapeCooldownSec = 600;
    // LevelSafeLogThrottleSec (default 60, 0 = every line, 0..3600; logging only): a level-safe event line (quest_walk_blocked reason=level_unsafe,
    // level_unsafe_escape result=no_spot, level_safe_yard result=none_known) repeats for the same bot, quest and reason at most once per this; the block itself
    // stays. The hourly deaths_top line counts the lines written and held back (level_safe_lines=, level_safe_held=).
    std::uint32_t LevelSafeLogThrottleSec = 60;
    // DeathLoopBreaker (default 0; bot_policy_death_loop.cpp, doc/level-safe.md): a solo bot that died 3 times to the same creature entry within 150 yards and 30 minutes
    // gives up its corpse, is moved as a ghost to another graveyard of its map (one that fits its level, else the nearest one 400 yards off the loop; never one with a live
    // hostile 3 levels above it beside the ghost) and revives there; the loop's graveyard is unsafe for that level for 2 hours (LevelSafeGraveyard skips it). No such
    // graveyard known: the ghost waits up to 5 minutes, revives, and is moved to a grind spot at once (out of combat). Never off its map (Skyborne 2991). Event
    // death_loop_break. The graveyard and grind spot teleports need Bot.Actions.Teleport = 1 in the core (the grind spot move walks when refused).
    bool DeathLoopBreaker = false;
    // The survival system (2026-11-19, doc/survival.md; every key default 0 = today's behaviour).
    // SafeRevive: a solo ghost never takes the spirit healer or reclaims its corpse with a live hostile SafeReviveLevelMargin (default 3, 1..20) or more levels above it
    // within 40 yards, nor at a place the danger map counts as dangerous for its level band; it is moved as a ghost to another graveyard of its map, or waits (at most
    // 10 minutes). Event safe_revive. The ghost teleport needs Bot.Actions.Teleport = 1 in the core.
    bool SafeRevive = false;
    std::uint32_t SafeReviveLevelMargin = 3;
    // FightRiskRetreat: a solo bot losing a fight (time to die < time to kill x FightRiskMarginPct / 100; default 100, 50..400) drinks a health potion or bandages, then
    // walks away to the last safe point. FightRiskTeleport (default 0): then a rescue teleport to a level-safe grind spot of the same map; set it only with the core's
    // Bot.Actions.TeleportInCombat = 1 (else the core refuses it, logged as stage=teleport_refused). Event fight_retreat.
    bool FightRiskRetreat = false;
    std::uint32_t FightRiskMarginPct = 100;
    bool FightRiskTeleport = false;
    // DangerMap: grind spots, quest walks and explore walks skip the 40 yard cells where bots of the bot's level band (or higher) died at DangerMapDeathsPerHour
    // (default 6, 1..1000) or more; a death counts less and less over DangerMapDecayMin (default 120, 10..1440). Events danger_cell, quest_walk_blocked reason=danger;
    // the hourly deaths_top line ends with danger_cells= danger_hot= danger_deaths= danger_skips= danger_top=.
    bool DangerMap = false;
    std::uint32_t DangerMapDeathsPerHour = 6;
    std::uint32_t DangerMapDecayMin = 120;
    // Below-map recovery (below_map.h): BelowMapRecover (default 0) teleports a living bot to the nearest known place when the nearest known place within 60 yards
    // lies more than BelowMapDropYards (default 60, 20..1000) above it for 20 s, or after BelowMapStormCount (default 8, 0 = that sign off, 0..100) no-route
    // answers for walks under 100 yards within 5 minutes; at most once per BelowMapCooldownSec (default 600, 60..86400). Event below_map_recover.
    bool BelowMapRecover = false;
    std::uint32_t BelowMapDropYards = 60;
    std::uint32_t BelowMapStormCount = 8;
    std::uint32_t BelowMapCooldownSec = 600;
    // 2026-11-20 (doc/belowmap-noroute-20261120.md), the false positives; each defaults to the safe side. BelowMapChecks (default 10, 1..100): checks in a row
    // (one every 2 s) the drop must hold. BelowMapMinPlaces (default 2, 1..20): known places within 60 yards needed to judge z_below. BelowMapRequireStill
    // (default 1): a moving bot is not judged (below_map_skip reason=moving). BelowMapStormRiseYards (default 30, 0..500): the storm counts a refused walk only
    // to a place this much above the bot. BelowMapStormDistinct (default 1): each refused destination once.
    std::uint32_t BelowMapChecks = 10;
    std::uint32_t BelowMapMinPlaces = 2;
    bool BelowMapRequireStill = true;
    std::uint32_t BelowMapStormRiseYards = 30;
    bool BelowMapStormDistinct = true;
    // No-route backoff (no_route_backoff.h): NoRouteBackoff (default 0) holds back a point walk to a place the core refused for that bot for NoRouteBackoffSec
    // (default 60, 5..3600), doubled with each further refusal of the place up to 30 minutes. Event no_route_backoff stage=armed|held.
    bool NoRouteBackoff = false;
    std::uint32_t NoRouteBackoffSec = 60;
    // LogRefusedReason (default 1, logging only): the "<action> not done: <status>" lines carry the ActionResult reason code the core gave (reason=N, 0 = none).
    bool LogRefusedReason = true;
    // LogThrottleSec (default 0 = off, every line; 0..3600; logging only): the repeating lines that are not events (bot N <action> not done: <status>) are
    // written once per bot, action and reason per this many seconds, and the next line of that run ends with suppressed=N. Events are never throttled here.
    std::uint32_t LogThrottleSec = 0;
    // DeathsTopEveryMin (default 60, 0 = off, 0..1440; logging only): the `forever-bots: deaths_top:` line (deaths, median per bot, share of the worst ten).
    std::uint32_t DeathsTopEveryMin = 60;
    // ArmSplit (default 0 = off, 0..1; sim measuring, doc/sim-ab.md): two configs in one run. The bots with an odd guid are arm B: their own turn uses the config
    // with ArmBOverrides ("Key=Value,Key=Value", default empty) and the Arm.B.<Key> keys applied; the even guids are arm A (the config as written). Every event
    // line naming a bot gets " arm=A|B", and the deaths_top and pump lines come per arm too. Ignored (warning) while a scenario runs. ArmB is arm B's settings
    // (null when off) and ArmBApplied the overrides as "Key=new (A: old)".
    // ArmSplitMode (default "guid"; "guid" or "strata"): guid = the parity split above; strata = each bot gets its arm at first sight per stratum (Skyborne
    // races 95/96 as one group, every other race its own, crossed with the class), alternating A/B within a stratum, so each stratum and the whole run end
    // within one bot of 50/50 (ab_split::Strata). Kept for the life of the process.
    bool ArmSplit = false;
    ab_split::SplitMode ArmSplitMode = ab_split::SplitMode::Guid;
    std::string ArmBOverrides;
    std::shared_ptr<BotSettings> ArmB;
    std::vector<std::string> ArmBApplied;
    // TraceBots (default empty; a comma list of bot guids): every event, action, action result and a state line with the hostiles near (on change and every 5 s)
    // of these bots go to TraceDir/bot-<guid>.trace (TraceDir default "", which turns tracing off; a missing folder is made), at most TraceMaxMB per bot
    // (default 20, 1..1024: rotated at half of it, one old file kept). Nothing of it goes to the journal.
    std::unordered_set<std::uint64_t> TraceBots;
    std::string TraceDir;
    std::uint32_t TraceMaxMB = 20;
    // Grind spots (config key GrindFile, see grind_data.h): the places idle bots walk to for their level. Empty turns it off (hubs are used).
    std::string GrindFile;
    // Rotations (config key RotationFile, see combat_rotation.h): fight rows and upkeep buffs per class from a file. Empty = the built-in rotations.
    // RotationFile and TameBeastFile default to the data folder beside the module's executable (a relative path is looked up there first); an empty value (RotationFile = "") uses the
    // built-in rows, an empty TameBeastFile switches taming off.
    std::string RotationFile = "data/rotations.txt";
    std::string TameBeastFile = "data/tame_beasts.txt";
    // Spell costs (config key SpellCostFile, see spell_costs.h): what each spell costs in mana, rage or energy, so a bot does not try a cast it cannot pay for.
    // Empty = no check (the core refuses the cast instead).
    std::string SpellCostFile;
    // Auto gear (config key AutoGearing, 0 or 1): the core dresses every bot for its level when it first appears and after it gains
    // AutoGearEveryLevels levels. Off by default; when it is off, the .autogear command in a party does it for that party's bots.
    bool AutoGearing = false;
    // Talent respec (config key TalentRespec, 0 or 1, off by default): a bot whose role or spec changed after it spent talent points gets them back and
    // spent by the new role (the core's Bot.Talents.Respec must be on too, since it is what pays the reset). See talent_respec.h.
    bool TalentRespec = false;
    // Chat commands (config keys ChatCommands and ChatOrders, 0 or 1, both off by default; see chat_commands.h). ChatCommands turns on the basic
    // verbs (follow, stay, goto here, release, status) typed by a group leader in party or raid chat or whispered to a bot; ChatOrders adds
    // stop, pull, heal, passive and aggressive. Only the bots of the speaker's own group obey.
    bool ChatCommands = false;
    bool ChatOrders = false;
    bool ChatControl = false;   // the control panel: role, stance, focus, distance
    // Say-back (config keys ChatSay, 0 or 1, off by default; ChatSayMax 0..50, default 3): a bot that takes a chat order answers with a short /say line.
    // ChatSayMax caps how many bots of a group speak for one order, so a big group does not drown the chat; a bot that was whispered always answers.
    bool ChatSay = false;
    std::uint32_t ChatSayMax = 3;
    // Ambient chat (feature 4; config keys AmbientChat 0 or 1, off by default; AmbientPerMin 0..30 lines a minute over all bots, default 2;
    // AmbientBotCooldownSec 0..3600 between two lines of one bot, default 300; AmbientChannels 1 General, 2 Trade, 3 both, default 1;
    // AmbientFile, the line pool, default "data/ambient_lines.txt"). The core must allow it too (Bot.Chat.Ambient.Enabled, see worldserver.conf.dist).
    bool AmbientChat = false;
    std::uint32_t AmbientPerMin = 2;
    std::uint32_t AmbientBotCooldownSec = 300;
    std::uint32_t AmbientChannels = 1;
    std::string AmbientFile = "data/ambient_lines.txt";
    // Whisper replies (config keys ChatWhisperReplies 0 or 1, off by default; WhisperCooldownMs 0..600000, default 60000; WhisperMaxPerMin 1..60,
    // default 6): a bot whispers a short reply to a player who whispers it, and to a player whose invite it cannot take while busy.
    bool ChatWhisperReplies = false;
    std::uint32_t WhisperCooldownMs = 60000;
    std::uint32_t WhisperMaxPerMin = 6;
    // Second-list features (off by default; the policy sends duel, battleground, guild and gift orders through the SDK's BotDuel, BotBattleground, BotGuild and
    // BotGiftMail, which need a core that knows them; an older core refuses them and the policy logs *_gap; InnDowntime is still logic only):
    // Duels, DuelMinLevel, DuelCooldownMin; Battlegrounds, BattlegroundMinLevel, BattlegroundMaxWaitMin; InnDowntime, InnHealthBelowPct,
    // InnPowerBelowPct, InnMaxRestMin; GuildJoin; MailGifts, MailGiftDailyCap.
    duels::Settings Duels;
    battlegrounds::Settings Battlegrounds;
    inn::Settings InnDowntime;
    guild::Settings GuildJoin;
    bot_guilds::Settings Guilds;            // GuildsEnabled and the GuildsLevelCap, GuildsRaidMinLevel, GuildsLevelingShare, ... keys (bot_guilds.h)
    gifts::Settings MailGifts;
    // BotMail (off by default, bot_mail.h, doc/bot-trade-mail.md): bots mail gold and materials to guildmates. BotMailGold, BotMailMaterials, BotMailDailyCap,
    // BotMailCooldownSec, BotMailRecipientDailyCap, BotMailPlanEverySec, BotMailMaxPerPlan, BotMailGoldKeepCopper, BotMailGoldMinCopper, BotMailGoldMaxCopper,
    // BotMailNeedyBelowCopper, BotMailMaterialMinCount, BotMailMaterialMaxCount.
    botmail::Settings BotMail;
    // Realism features without combat (docs/bot-realism-backlog.md items 2 to 5; all off by default): IdleFidget*, ReadDelay*, PassingBuff*, TownAfk*; the
    // round-number auction price is in Auction (AuctionRoundStep, AuctionRoundMaxPct).
    realism::FidgetSettings Fidget;
    realism::ReadSettings ReadDelay;
    realism::BuffSettings PassingBuff;
    realism::AfkSettings TownAfk;
    // Advisor features (all off by default; keys in config/modules.d/forever-bots.conf.example, "Advisor features"):
    lfg_queue_fill::Settings LfgQueueFill;
    death_pause::Settings DeathPause;
    trade_ads::Settings TradeAds;
    std::string TradeAdsFile = "data/trade_ads.txt";
    party_mana::Settings PartyMana;
    std::string PartyManaFile = "data/party_mana.txt";
    whisper_matrix::Settings WhisperMatrix;
    std::string WhisperMatrixFile = "data/whisper_replies.txt";
    path_jitter::Settings PathJitter;
    path_variety::Settings PathVariety;     // PathVariety and the Path* keys (path_variety.h)
    std::string PathZoneFile;               // PathZoneFile: zone samples for the grind spots (path_variety.h); empty = coarse square regions
    personality::Settings Personality;      // Personality and the PersonalityRestStrength, ...IdleStrength, ...ReactionMaxMs, ...CautionStrength, ...MistakeMaxPct, ...SocialStrength keys (personality.h)
    guild_milestone::Settings GuildMilestone;
    std::string GuildMilestoneFile = "data/guild_milestones.txt";
    city_inspect::Settings CityInspect;
    transport_ride::Settings TransportRide;
    city_greet::Settings CityGreet;
    std::string CityGreetFile = "data/city_greetings.txt";
    std::string AuctionTalkFile = "data/auction_talk.txt";
    quest_invite::Settings QuestInvite;
    // Group quests (config QuestGroups, off by default; quest_group.h). WaitMs is QuestGroupWaitMinutes in minutes.
    quest_group::Settings QuestGroups;
    // Dynamic parties (config DynamicParties, on by default; party_reason.h): a world party member is in its party only while there is a reason (a shared
    // unfinished quest, the leader's group quest, a level dungeon due), on the leader's map and near it; it leaves for errands and an errand break.
    party_reason::Settings DynamicParties = [] { party_reason::Settings on; on.Enabled = true; return on; }();
    meet_greet::Settings MeetGreet;
    std::string MeetGreetFile = "data/meet_greetings.txt";
    group_etiquette::Settings GroupEtiquette;
    std::string GroupEtiquetteFile = "data/group_etiquette.txt";
    // ADVISOR-MEMBERS
    // Supplies (layout 15, config keys Supplies, SupplyAmmoLow, SupplyAmmoTarget, SupplyFoodLow, SupplyFoodTarget, SupplyDrinkLow, SupplyDrinkTarget,
    // SupplyTripSec; off by default): bots buy ammunition, food and drink at vendors that sell what they can use, and eat and drink while resting.
    // Layout 16, each off by default: SupplyPotions (health and mana potions, drunk in a fight below UseHealthPotionBelowPct and
    // UseManaPotionBelowPct), SupplyBandages (used by a resting bot below UseBandageBelowPct that has no food), SupplyHealthPotionLow,
    // SupplyManaPotionLow and SupplyBandageLow (the counts under which a bot goes shopping; the core's settings decide how many it buys).
    // SupplyReagents (off by default): a bot whose cast was refused for lack of a reagent goes to a vendor that sells it; the core names the item.
    supply::Settings Supplies;
    // Urgent repair trips (repair_policy.h; on by default, independent of AutoGearing and of the routine ServiceEverySec visit): RepairUrgent (0 or 1),
    // RepairBrokenPct (0..99: a worn piece at or below this sends the bot to its repairman at once; 0 = only a broken piece), RepairAfterDeathPct (0..100:
    // after a death, a piece below this does too; 0 = off), RepairRetrySec (60..86400: the cooldown between urgent trips; a trip that repaired nothing
    // doubles it, up to an hour) and RepairLowCopperPerLevel (0..100000: a bot with less money than level x this also sells its white junk at the visit).
    repair::Settings Repair;
    // VendorSellGear (default on; needs Bot.Vendor.SellGear in the core): a vendor visit also sells the bag gear the worn gear outclasses.
    bool VendorSellGear = true;
    // Protocol 28: SpiritHealerAfterMs (default 15000 since the time quick wins, was 45000; 0 = off), SpiritHealerOnRepeat (default on), HealDownrank (default off); see PolicyOptions.
    std::uint32_t SpiritHealerAfterMs = 15000;
    bool SpiritHealerOnRepeat = true;
    bool HealDownrank = false;
    // Leveling deaths (see PolicyOptions): IdleEnemyAttackersOnly (default on), CasterMeleeWithoutSpell (default on), EngageMaxLevelAbove (default 5, 0 = off, 0..60),
    // EngageEliteSolo (default off), GhostFallbackSec (default 30, 0 = off, 0..3600), GhostMaxSec (default 600, 0 = no bound, else 60..86400), DeathAreaCount (default 3,
    // 0 = off, else 2..50), DeathAreaWindowSec (default 900, 60..86400), DeathAreaYards (default 60, 10..500), DeathAreaAvoidSec (default 1800, 60..86400).
    bool IdleEnemyAttackersOnly = true;
    bool CasterMeleeWithoutSpell = true;
    std::uint32_t EngageMaxLevelAbove = 5;
    bool EngageEliteSolo = false;
    std::uint32_t GhostFallbackSec = 30;
    std::uint32_t GhostMaxSec = 600;
    std::uint32_t DeathAreaCount = 3;
    std::uint32_t DeathAreaWindowSec = 900;
    std::uint32_t DeathAreaYards = 60;
    std::uint32_t DeathAreaAvoidSec = 1800;
    // DPS threat discipline (config keys ThreatHold, ThreatHoldPct, ThreatReleasePct, ThreatHoldMaxSec, ThreatNearDeathPct, ThreatDropAbilities); see PolicyOptions.
    bool ThreatHold = true;
    std::uint32_t ThreatHoldPct = 90;
    std::uint32_t ThreatReleasePct = 70;
    std::uint32_t ThreatHoldMaxSec = 8;
    std::uint32_t ThreatNearDeathPct = 15;
    bool ThreatDropAbilities = true;
    // Presence (config keys Presence, PresenceOnlineMinMin, PresenceOnlineMaxMin, PresenceOfflineMinMin, PresenceOfflineMaxMin, PresenceMinOnline,
    // PresenceMaxOnline, PresenceDayPercent; off by default): bots log off and on like players, see bot_presence.h. Needs Bot.Presence.LogoutOnRelease
    // in the core, or a released bot stays in the world.
    presence::Settings Presence;
    // Brackets (bot_brackets.h; config keys BracketsEnabled, BracketTargets, BracketSpotCap, BracketStalledMinutes, BracketMoveDown,
    // RetireLevel60; off by default). The ramp places each new bot in the level bracket furthest short of its share. The level counts come from the
    // presence feed, so brackets need Presence on.
    brackets::Settings Brackets;
    // Dungeons (doc/dungeons.md): DungeonFile = the routes (data/dungeons.txt); with it set, "tank lead" works in every dungeon of the file. DungeonEventsFile = the
    // events table for all dungeons (data/dungeon_events.txt, dungeon_events.h): doors to open, charges to fire.
    bool DungeonTeleport = false;   // DungeonTeleport = 1: lost dungeon bots are put back by teleport (tests and scenarios; needs Bot.Actions.Teleport in the core)
    // DungeonSummons = "dm,rfc" (off by default): the scenarios (by their names) whose summons the policy puts first: a summoner's adds (the Deadmines' golem and Squallshapers,
    // Ragefire's Voidwalker minion) are engaged past the engagement cap and the party turns on them once they are in view, as with focus off. Needs the dungeon file's summons.
    std::vector<std::string> DungeonSummons;
    // ScenarioConfigGuard = 1 (off by default): a scenario holds its dressing until the core has applied a teleport the module sent (ScenarioPlan::VerifyConfig); a
    // config the core refuses (Bot.Actions.Teleport or Bot.Actions.Grant off) ends the run as config_refused instead of running in the wrong place.
    bool ScenarioConfigGuard = false;
    // Level dungeons (doc/dungeons.md): a levelling bot party runs each dungeon of the dungeon file when its level reaches the dungeon's max level (LevelDungeons, on by
    // default), at most LevelDungeonRuns times per dungeon (1..5, default 2). LfgCommand: "LFG <dungeon>" in the General or Trade channel brings free bots to the speaker.
    bool LevelDungeons = true;
    std::uint32_t LevelDungeonRuns = 2;
    // Level dungeon rules (doc/dungeons.md), all off by default. LevelDungeonEarlyLevels (0..20): due from this many levels below the dungeon's max level, never below its
    // min level. LevelDungeonSides ("rfc:horde,hot:alliance"): a party of the other side is never sent there. LevelDungeonFileDuration: the run cap is the file's
    // run.duration_sec instead of 45 minutes. LevelDungeonNeedRoles: a party goes only with a tank and a healer. LevelDungeonLog: level_dungeon_due / level_dungeon_skip lines.
    std::uint32_t LevelDungeonEarlyLevels = 0;
    std::map<std::string, std::uint8_t> LevelDungeonSides;
    bool LevelDungeonFileDuration = false;
    bool LevelDungeonNeedRoles = false;
    bool LevelDungeonLog = false;
    bool LfgCommand = true;
    // LfgFill = 1 (default 0): a human's party that lacks a tank or a healer is offered free bots, which whisper their role to the leader (lfg_fill.h).
    bool LfgFill = false;
    // AggressiveStart = 1: bots start Aggressive, so a bot on its own pulls the creatures of its level and levels. The default stance is defensive (a bot fights only
    // what attacks it, or what its group fights); a bot that only defends itself earns almost no experience: on the 300-bot soak the deaths equalled the kills.
    bool AggressiveStart = false;
    // Crafting = 1 (default): a bot with nothing to fight makes bandages from its cloth and cooks its meat (kSkillUp, SDK 0.17; the core must have the action), which
    // raises First Aid and Cooking and uses up what fills its bags. 0 turns it off.
    bool Crafting = true;
    // DungeonTactics = "focus,interrupt,heal,rest,recover,cc" or "all" or "none" (default: none): which of the dungeon party's tactics are on (bot_policy.h). In the first sim
    // runs (docs/99-deadmines-runs.md) all of them on did slightly worse than none (2.0 bosses and 36 deaths against 2.25 and 14), and no single tactic is proven yet, so they
    // are off until each has a run of its own that shows a gain.
    bool TacticFocus = false, TacticInterrupt = false, TacticHeal = false, TacticRest = false, TacticRecover = false, TacticCc = false;
    // Planned healing (heal_plan.h, doc/planned-healing.md): HealPlan (default on; needs PartyPlanner), HealPlanPreHealMs (default 4000, 0 = no pre-heal), HealPlanPreHealPct (default 96).
    bool HealPlan = true;
    std::uint32_t HealPlanPreHealMs = 4000;
    std::uint32_t HealPlanPreHealPct = 96;
    bool PartyPlanner = true;     // the party planner (party_planner.h): its own key, PartyPlanner = 0 turns it off
    bool PartyViewPlanner = false; // PartyViewPlanner = 1: the planner also reads the core's shared party view (needs the core's Bot.Snapshot.PartyView); default off
    // Bounded policy-lab knobs; defaults match the built-in dungeon policy.
    std::uint32_t DungeonHealBelowPct = 55;
    std::uint32_t DungeonTankHealBelowPct = 75;
    std::uint32_t HealerDpsManaPct = 40;
    std::uint32_t HealerCombatManaPct = 25; // HealerCombatManaPct: a healer in a party fight keeps this share of its mana for heals (0 = no floor; 0..80)
    bool TankPickup = true;       // TankPickup: a tank taunts the mobs that are on party members (needs PartyViewPlanner and the core's party view)
    // TankLoosePickup = 1 (default): in a dungeon, raid or tank-led party (the routeOn gate, as PullTankFirst) the tanks elect ONE tank per loose mob (the nearest with a ready taunt),
    // healers' and casters' attackers first, within TankLooseRangeYards of the tank, in line of sight; TankLooseCooldownMs between two picks of one tank; TankLooseChaseYards (0 = never
    // walk): a tank out of the taunt's reach walks at most this far toward the mob, only inside the chase leash. Needs TankPickup and PartyViewPlanner.
    bool TankLoosePickup = true;
    std::uint32_t TankLooseCooldownMs = 2500;
    std::uint32_t TankLooseRangeYards = 40;
    std::uint32_t TankLooseChaseYards = 8;
    std::uint32_t HealerLosMoveYards = 6;
    // PullTankFirst = 1 (default): in a dungeon, raid or tank-led party the members wait for the tank to be in combat before they engage its target (PullTankHoldMs: at most
    // this long, default 12000; PullMemberRangeYards: a target this near a member needs no walk, default 12).
    bool PullTankFirst = true;
    // HealWaitTankHold = 0 (default, 1 = on): a healer casts no heal (Renew) on the party's tank at full health until the tank holds its target (heal_hold event).
    bool HealWaitTankHold = false;
    // HealSpellMix = 0 (default, 1 = on): healers pick between their heal spells (priest Flash Heal, Heal, Greater Heal, Renew, Power Word: Shield; paladin Flash of Light, Holy Light,
    // Lay on Hands; shaman Healing Wave, Lesser Healing Wave, Chain Heal; druid Healing Touch, Regrowth, Rejuvenation) by the missing health, incoming damage, tank, mana and rank (heal_spell.h).
    bool HealSpellMix = false;
    std::uint32_t PullTankHoldMs = 12000;
    std::uint32_t PullMemberRangeYards = 12;
    // PullMinHpPct = 60 (default): a member of a dungeon, raid or tank-led party below this health percent that nothing attacks stands and rests instead of engaging or walking into the
    // tank's fight (0 = off; at most 2 minutes, then it may engage again). Revived-at-49% members walked back into the pack that killed them (ZG, 2026-11-13).
    std::uint32_t PullMinHpPct = 60;
    // PullReadyManaPct = 60 (default): the ready gate before a pull in a route party: a mana user below this mana percent rests instead of pulling or joining a pull while the leader
    // is out of combat (pull_hold reason=low_mana), and the route leader does not walk on to the next pack while a member is below PullMinHpPct health or this mana. At most
    // PullReadyMaxMs (default 90000, 1000 to 600000), so a party with no drink is not frozen. 0 = off (the health gate PullMinHpPct stays).
    std::uint32_t PullReadyManaPct = 60;
    std::uint32_t PullReadyMaxMs = 90000;
    // DotPerTarget = 0 (default): 1 tracks damage-over-time and debuff rows per (spell, target) instead of one slot per spell (bug 14 of the bot AI plan).
    bool DotPerTarget = false;
    // The ready gate before a pull (doc/ai-plan-next.md). SoloPullManaPct = 0 (default, 0..90): a solo mana user below this mana percent starts no fight, it sits and drinks up to this plus 20
    // (event pull_wait reason=mana). PullBuffGate = 0: a solo bot, or a route party's leader, waits for the upkeep buffs its rotation casts out of combat (pull_wait / pull_hold reason=buff).
    // PullWaitMaxMs = 60000 (1000..600000): the longest each of these waits; a wait that times out logs pull_wait_timeout and does not start again until the need was over once.
    // KillerAvoidMs = 0 (default; 0, or 10000..86400000): a solo bot that died to a creature does not pull creatures of that entry for this long (event killer_avoid; 1800000 is 30 minutes).
    std::uint32_t KillerAvoidMs = 0;
    // AoeCcProtect = 0 (default): 1 skips area-damage rows while a creature a party member holds with Polymorph, Sap or Banish stands within 12 yards of the target (event aoe_cc_protect).
    bool AoeCcProtect = false;
    std::uint32_t SoloPullManaPct = 0;
    bool PullBuffGate = false;
    std::uint32_t PullWaitMaxMs = 60000;
    // Death fixes (2026-11-17, doc/death-fixes.md), all default off. SoloPullHpPct = 0 (0..95): a solo bot out of combat below this health percent starts no fight and rests up to this
    // plus 10 (event pull_wait reason=hp); 0 = the fixed 75 percent mark without the hysteresis. PullMaxGroup = 0 (0..3): a solo bot does not pull a creature with more than this many
    // hostiles (itself included) within 10 yards (1 = singles only); 0 = the built-in 3. ResurrectBackoff = 0: a refused resurrect answer waits 4, 8, 16, 32, 60 s (not three tries two
    // seconds apart) and the corpse run goes on meanwhile (event resurrect_backoff).
    std::uint32_t SoloPullHpPct = 0;
    std::uint32_t PullMaxGroup = 0;
    std::uint32_t PullMaxLevelAbove = 2;   // 0..2: a solo bot pulls no creature more than this many levels above it (2 = the built-in rule)
    bool ResurrectBackoff = false;
    // Wipe recovery (a scenario run with ScenarioWipeRecovery = 1; Karrz1's raid logic): after a wipe (everybody down, or nearly) the party holds, nobody pulls, until all have revived
    // and have WipeReadyPct health and mana (0 = off, 1 to 100, default 80) or WipeReadyMaxMs has passed (default 240000, 10000 to 1800000); the leader (tank) is freed first. After
    // WipeBackoffCount wipes at the same route segment (default 3, 0 = off) the run backs off: it waits WipeBackoffMs longer (default 90000, 0 to 900000) and the party is put back at a
    // safe route point two points before the pack (and the leader set back to it) instead of pulling the same pack again.
    std::uint32_t WipeReadyPct = 80;
    std::uint32_t WipeReadyMaxMs = 240000;
    std::uint32_t WipeBackoffCount = 3;
    std::uint32_t WipeBackoffMs = 90000;
    // WarderPartyHold = 1 (default): in the Onyxia raid a member with no target of its own does not follow the tank into an Onyxian Warder's Fire Nova; it holds outside the nova
    // while the tank pulls the Warder alone (2026-11-13 sim: 40 members followed the tank to 0-6 yards of the Warder and died to Fire Nova, 700 of 798 deaths).
    bool WarderPartyHold = true;
    // DamageEvents = 1 (default): take in the core's damage events (PlayerScript.OnDamaged, protocol 29: a bot's killing blows and big hits with the real attacker and spell) for the
    // died: line and the run telemetry. DamageEventMinPct = 20: the module drops a non-lethal hit below this share of the bot's max health (the core's Modules.DamageEvent.MinPct is
    // the floor under it; killing blows are never dropped).
    bool DamageEvents = true;
    std::uint32_t DamageEventMinPct = 20;
    // ActionStats = 1: every minute the module logs the ten (bot, action, spell) combinations it sent most often in that minute ("actions_top"), to find a bot that
    // repeats an action every tick. Off by default.
    bool ActionStats = false;
    // MountMinLevel (default 40): a bot asks to mount up for a long walk only from this level (classic riding is learned at 40). 0 = at any level.
    std::uint32_t MountMinLevel = 40;
    // Gathering = 1: a bot with Mining, Herbalism or Skinning asks the core to gather what is near it (kGather, SDK 0.18). Off by default.
    bool Gathering = false;
    // Riding (RidingTraining = 1, off by default): a bot learns the riding ranks of RidingRankFile (data/riding_ranks.txt) from a stable master
    // when its level and gold allow (riding_training.h). The table ships empty until the stable masters' spells and costs are checked.
    bool RidingTraining = false;
    std::string RidingRankFile = "data/riding_ranks.txt";
    // Mounts (MountBuy = 1, off by default; needs Errands = 1 and the core's mount refusal as its sign): a bot that the core refused a mount walks to a riding trainer
    // (RidingTraining = 1: the ranks of RidingRankFile at RidingTrainerFile's trainers) and then to a vendor of its race (MountVendorFile) and buys a mount. Both
    // trips keep MountReserveCopperPerLevel copper per level in the bot's purse. mount_plan.h, doc/mounts.md.
    bool MountBuy = false;
    std::uint32_t MountReserveCopperPerLevel = 5000;
    std::string RidingTrainerFile = "data/riding_trainers.txt";
    std::string MountVendorFile = "data/mount_vendors.txt";
    // Reputation goals (ReputationGrind = 1, off by default; keys ReputationMinLevel, ReputationTripSec, ReputationMinCopper, ReputationGoalFile): a bot picks a faction
    // whose reputation gates equipment (data/reputation_goals.txt) and buys the reward gear at the faction's vendor when the core says one sells it something it can
    // use now (reputation.h, doc/reputation.md). Needs the core's reputation facts and vendor buy, which do not exist yet: until then the keys change nothing.
    reputation::Settings Reputation;
    std::string ReputationGoalFile = "data/reputation_goals.txt";
    // Reputation planner (ReputationPlan = 1 on top of ReputationGrind = 1, off by default; keys ReputationPlanBudget, ReputationPlanSec, ReputationStallSec,
    // ReputationTargetFile, ReputationSourceFile): a bot picks a faction of the progression path and a quest or creature that earns its reputation (reputation_plan.h).
    reputation::PlanSettings ReputationPlan;
    std::string ReputationTargetFile = "data/reputation_targets.txt";
    std::string ReputationSourceFile = "data/reputation_sources.txt";
    // Weapon skills (WeaponTraining = 1, off by default): a bot learns its class's weapon skills at its class trainer (weapon_training.h).
    bool WeaponTraining = false;
    // Fishing (Fishing = 1, off by default): a bot with a pole walks to the nearest spot of FishingSpotFile (data/fishing_spots.txt) and fishes there
    // (fishing.h). The cast needs a core action that targets the water (doc/fishing.md).
    bool Fishing = false;
    std::string FishingSpotFile = "data/fishing_spots.txt";
    // Rare hunts (RareHunts = 1, off by default): a bot fights the rare creatures of RareHuntFile (data/rare_hunts.txt) in range of its level
    // (rare_hunts.h). Its entries join the fight filter while a hunt is on.
    bool RareHunts = false;
    std::string RareHuntFile = "data/rare_hunts.txt";
    // CraftingFociFile (data/crafting_foci.txt): the anvils, forges and cooking fires of the capitals. With it, a bot whose recipes need a focus (kNeedsFocus) walks to
    // the nearest one on its map; without it the skill just waits. Use an absolute path: the worldserver's working directory is not the module's.
    std::string CraftingFociFile;
    // GatherWalk = 1 (off by default; with Gathering): a bot that wants to gather and has no node in sight walks to the nearest one of its skill in
    // GatherNodesFile (data/gather_nodes.txt; an absolute path). Without the file the walk does nothing.
    bool GatherWalk = false;
    std::string GatherNodesFile;
    // GatherWalkGapSec (0..86400, default 0 = none): after a walk to a node ends, no new walk starts for this long, so a gatherer goes back to grinding in between.
    std::uint32_t GatherWalkGapSec = 0;
    // AvoidGroundEffects = 1 (off by default): a bot standing inside a harmful ground effect (SDK 0.19: the core must have Bot.Snapshot.GroundEffects = 1) walks out of it.
    // AvoidSpellsFile (absolute path, data/avoid_spells.txt): the Classic dungeons' spells that leave one; a spell in it counts as harmful even if the caster is not
    // known to be hostile. The dungeon files' bosses[].strategy.avoid_spells and avoid (fixed zones) add to it for their map.
    bool AvoidGroundEffects = false;
    std::string AvoidSpellsFile;
    // TelegraphEscape (default ON, only acts on maps that have raid rules): a bot inside the area of an enemy cast (layout 25 telegraphs: Onyxia's Fireball and Deep Breath) steps out.
    // A tank holds unless the rule table marks the spell lethal (`avoid|spell`) or frontal; a bot mid-cast holds unless the spell is lethal or TelegraphEscapeCasters = 1.
    bool TelegraphEscape = true;
    bool TelegraphEscapeCasters = false;
    // RaidSpread, RaidAvoidEnemy, RaidPurge, RaidMindControl (default ON each; they act only through rows of the raid rule table, so with no RaidRulesFile or no such row nothing changes):
    // `spread` rows keep the members of a group apart while the row's boss is engaged, `avoid_enemy` rows keep non-tank members a distance from a hostile of an entry,
    // `purge` rows let a priest or shaman remove a listed enemy buff, `mindcontrol` rows make nobody attack a member carrying the aura and let a mage polymorph it. doc/raid-rules.md.
    bool RaidSpread = true;
    bool RaidAvoidEnemy = true;
    bool RaidPurge = true;
    bool RaidMindControl = true;
    // ZG levers (2026-11-14, default OFF each; the sim turns them on in a drop-in; doc/raid-rules.md "ZG levers"):
    // RaidAvoidEnemyMelee / RaidAvoidEnemyTank: `avoid_enemy` rows of group melee move melee damage dealers, rows of group tanks move the tank (otherwise neither moves).
    // RaidFocusAvoidEnemy: a ranged damage dealer of a route party takes a fighting hostile of an avoid_enemy entry as its target first and ignores that one unit's circle.
    // PullOnePack: while the party fights, nobody picks a hostile that fights nobody (a second pack); the leader does not pull while a member is stuck far away
    // (within the pull hold's caps). TankLooseSkipAvoidEnemy: the tank neither taunts nor walks to a loose mob of an avoid_enemy entry (tank_loose_skip).
    // RegroupRallyMs (0 = off, else 5000..600000): a stuck member is waited for this long after it was flagged (route walk and pull), and walks to the leader (regroup_rally).
    bool RaidAvoidEnemyMelee = false;
    bool RaidAvoidEnemyTank = false;
    bool RaidFocusAvoidEnemy = false;
    bool PullOnePack = false;
    bool TankLooseSkipAvoidEnemy = false;
    // FlexRoles (default OFF): a party with no warrior gets a paladin or druid as its tank (and one healer from priest, shaman, druid, paladin); they pull at most one mob fewer than a warrior.
    // Explicit roles and roster specs are unchanged. flex_roles.h; log event flex_roles.
    bool FlexRoles = false;
    std::uint32_t RegroupRallyMs = 0;
    // fight_stalled (map 2991 start-zone creatures, 3-4 yards away on the ground, low health, no damage for the whole stall; the cause is believed to be a target in the air or
    // below the ledge: the ground distance says in reach, the 3D distance does not). FightHeightAvoidMs (0 = off, else 3000..60000): a melee bot avoids such a target after this long
    // (event fight_height_avoid). FightStallDiag (0 = off): the fight_stalled event also carries dz, 3D distance, class, power, moving, the last action and its status.
    std::uint32_t FightHeightAvoidMs = 0;
    bool FightStallDiag = false;
    // WalkDecideEvery (default 2, 1 = off, 1..10): a solo bot that is walking to a place the policy sent it to, out of combat with no hostile near, in no group, dungeon or
    // errand, takes its full decision turn on one tick in this many (calm_walk.h). Any change (a new result, a hostile, combat, an invite, the end of the walk) ends the calm
    // and the next tick is a full turn. At a world update of 500..1700 ms the delay is at most (N-1) ticks before a calm walker reacts to something that is not in the snapshot.
    std::uint32_t WalkDecideEvery = 2;
    // DecideScheduler (default OFF; doc/decide-scheduler.md, decide_scheduler.h): per-bot decide tiers. Combat, party, dungeon, a ghost at its corpse, a cast, a transport or a
    // hostile within 30 yd: every tick. Walking or calm questing: one tick in DecideTierWalk (1..10). Resting, idle, dead, corpse run: one tick in DecideTierIdle (1..20). A pending
    // reaction (death, resurrect, loot, invite, trade, a new action result or refusal, the end of a walk) runs the bot at once. DecideBudgetUs (0 = off, else 500..200000): the
    // optional turns of a tick stop when the tick has used this many microseconds; the rest carry over. DecideMaxSkipTicks (1..60): no bot waits longer than this.
    bool DecideScheduler = false;
    std::uint32_t DecideBudgetUs = 0;
    std::uint32_t DecideTierWalk = 3;
    std::uint32_t DecideTierIdle = 6;
    std::uint32_t DecideMaxSkipTicks = 12;
    // RaidBossPositions (default OFF; Onyxia fix 1, doc/raid-rules.md "Boss positions"): the `tank_spot` and `flank` rows act. The tank the boss attacks holds it from the
    // tank spot (so the boss faces the way the row sets), the other members that fight it stand on its flanks, none in its front or rear cone. Without rows nothing changes.
    bool RaidBossPositions = false;
    // RaidPullSingle (default OFF; Onyxia fix 3, doc/raid-rules.md "Pull one at a time"): the `pull_single` rows act (a listed creature is pulled only when no second one of
    // its entry stands within the row's yards of it; the leader holds its walk meanwhile, 60 s at most; in a fight nobody picks one that fights nobody).
    bool RaidPullSingle = false;
    // RaidGhostFightHoldMaxMs (0 = off, the default, else 10000..3600000; 300000 is a good value): on a map with a `pull_single` row a dungeon ghost's teleport into the
    // instance waits while its party fights, at most this long per death.
    std::uint32_t RaidGhostFightHoldMaxMs = 0;
    // RaidRulesFile (absolute path, data/raid_rules.txt; empty = none): the raid rule table, see raid_rules.h and the README.
    std::string RaidRulesFile;
    // CorpseRunTeleportSec (0 = off, else 10..3600): a ghost whose corpse run takes longer is put at its corpse (needs Bot.Actions.Teleport).
    std::uint32_t CorpseRunTeleportSec = 30; // was 60 before the time quick wins (docs/dungeon-time-loss-20261115.md item 7)
    // PullHoldBudgetSec (default 600, 0 = no budget, else 30..86400): the most a dungeon leader's pull holds (party not ready, tank rest) may hold in one run; once it is
    // used up the leader pulls without waiting for health or mana (hold_budget_spent event). Each hold is also cut at kPullHoldAbsMaxMs (90 s) and ends with the leader's fight.
    std::uint32_t PullHoldBudgetSec = 600;
    // GhostTeleportPerSecond (0 = no limit, else 1..100; default 2): at most this many ghost teleports a second, so a wiped raid does not teleport 20 ghosts in one tick.
    std::uint32_t GhostTeleportPerSecond = 2;
    // GhostTeleportHold (default 1): a dungeon ghost's entrance/party teleport waits while a live hostile is within 30 yards of the destination, GhostTeleportHoldMaxMs
    // (default 120000, 1000..3600000) at most per death. DeathLockoutCount (default 4, 0 = off, else 2..50) deaths within DeathLockoutWindowMs (default 600000, 10000..86400000)
    // make the bot wait DeathLockoutHoldMs (default 90000, 0..3600000) more before the teleport.
    bool GhostTeleportHold = true;
    std::uint32_t GhostTeleportHoldMaxMs = 120000;
    // GhostHoldAvoidEnemyMaxMs (default 0 = off, else up to 3600000; 300000 is a good value): a dungeon ghost's teleport (entrance, party, leader rejoin, corpse) waits while a fighting hostile of
    // an avoid_enemy row stands within max(30, the row's yards) of the destination, at most this long per death (ghost_teleport_hold reason=avoid_enemy).
    std::uint32_t GhostHoldAvoidEnemyMaxMs = 0;
    // GhostHoldPartyFightMaxMs (default 0 = off, else 1000..3600000; 180000 is a good value): a dungeon ghost's entrance teleport (the core revives it there at half health) waits while a
    // living member of its party is in combat, this long at most per death; after the fight it is revived beside the leader (ghost_teleport_hold reason=party_fight).
    std::uint32_t GhostHoldPartyFightMaxMs = 0;
    // DungeonRevivesPerMinute (default 0 = off, else 1..40): at most this many ghosts of one dungeon party are put back into the instance (entrance, leader, leader rejoin) in 60 s
    // (ghost_teleport_hold reason=revive_cap).
    std::uint32_t DungeonRevivesPerMinute = 0;
    std::uint32_t DeathLockoutCount = 4;
    std::uint32_t DeathLockoutWindowMs = 600000;
    std::uint32_t DeathLockoutHoldMs = 90000;
    // Combat drift of dungeon parties (0.45.0; PolicyOptions has the details). DungeonPullLeashYards (0 = off, else 10..80): the route leader pulls only hostiles this
    // close to the leg of the route it walks. DungeonChaseLeashYards (0 = off, else 10..80): a party bot does not chase or pick a hostile farther than this from both
    // that leg and its leader. DungeonLeaderRejoin = 1: a dead leader comes back next to its quiet party, not alone at the entrance.
    std::uint32_t DungeonPullLeashYards = 30;
    std::uint32_t DungeonChaseLeashYards = 40;
    bool DungeonLeaderRejoin = true;
    bool DungeonReviveOutOfCombat = false;  // DungeonReviveOutOfCombat = 1: a dead member of a dungeon party is revived by its leader out of combat (needs Bot.Chat.Orders.Enabled in the core)
    std::string DungeonFile;
    // DungeonDataDir (default: the folder "dungeons" beside DungeonFile) holds the shipped per-dungeon JSON files; DungeonDir is your own folder of them, whose file for a map
    // replaces the shipped one (doc/dungeon-strategy.md). Both are read again when the config is reloaded.
    std::string DungeonDataDir;
    std::string DungeonDir;
    // RunTelemetryDir (absolute directory path, default empty = off): the party_tl:, party_stalled: and boss_attempt: lines of a scenario run are also appended to
    // <dir>/run-telemetry.log (doc/dungeon-strategy.md, "Telemetry"). A relative path is refused with a warning. (The key TelemetryFile is the bots' CSV, a different file.)
    std::string RunTelemetryDir;
    // DamageTelemetry (default 0; the sim sets 1): the boss_attempt: end lines of a scenario run carry the estimated damage done per class and per bot (dmg_done_by_class=, dmg_done_by_bot=,
    // the health the party takes off its targets, shared among the bots on each) and the attempt's time split (split_s=approach,boss,adds,waiting,mana_rest,hp_rest,dead,idle); a damage_done: line
    // closes the run. Journal only, no extra actions.
    bool DamageTelemetry = false;
    std::string DungeonEventsFile;
    // Flight points (layout 17; config keys Taxi, TaxiMinWalkYards, TaxiMasterMaxYards, TaxiBlockedSec; off by default): a bot that is not in a party replaces a walk
    // of at least TaxiMinWalkYards with a flight from a flight master within TaxiMasterMaxYards that lies on the way. Needs Bot.Taxi.Enabled in the core (on by default).
    taxi::Settings Taxi;
    // Auction House (doc/economy.md; config keys Auction, AuctionBuy (a bot also buys upgrades), AuctionSellPctGreen, AuctionSellPctBlue, AuctionSellPctEpic, AuctionBuyoutPct, AuctionRelistPct,
    // AuctionRunHours (2 only: the core runs every auction 2 hours), AuctionMinVendorCopper, AuctionBagFullPct, AuctionPerVisit, AuctionListedMax,
    // AuctionEverySec, AuctionBuyMaxPct, AuctionBrowsePerVisit, AuctionCallsPerTick; off by default). main.cpp drives the listing and buy visits.
    auction::Settings Auction;
    // Vendor sales (sell policy; config keys SellVendor, SellEverySec, SellPerVisit; off by default). main.cpp sends the sales on a vendor visit.
    sell::Settings Sell;
    // Town errands (config keys Errands, ErrandDailyMin, ErrandGapMin, ErrandMaxMin, ErrandPerCity, ErrandStartsPerTick; off by default): profession
    // training trips to a capital trainer, scheduled by errand::Scheduler (errand_policy.h).
    errand::Settings Errand;
    std::string ErrandTrainerFile = "data/errand_trainers.txt"; // the trainer table (ErrandTrainerFile)
    // World events and holidays (config keys WorldEvents, WorldEventsFile, WorldEventsActive, WorldEventsPct, WorldEventsMaxBots, WorldEventsPerSite,
    // WorldEventsMaxMin, WorldEventsGapMin, WorldEventsStartsPerTick; off by default): while a game event is active a few bots visit its sites
    // (world_events.h, doc/world-events.md). Settings.Seed is filled from RandomSeed by the module.
    world_events::Settings WorldEvents;
    std::string WorldEventsFile = "data/world_events.txt";
    std::vector<std::uint32_t> WorldEventsActive;   // the active game events, as the operator lists them (until the core reports them)
    std::uint32_t AutoGearEveryLevels = 5;
    // The scenario's dungeons, in order: one for the single form, one per listed dungeon for Scenario = "dm:10,rfc:10". Scenario is the first of them (or the empty
    // one when the scenario is off), for the code that only knows one.
    ScenarioSettings Scenario;
    std::vector<ScenarioSettings> Scenarios;
};

// The most parties a scenario runs at once, over all its dungeons.
constexpr std::uint32_t kMaxScenarioParties = 200;

// One entry of a Scenario list ("dm:10").
struct ScenarioListItem
{
    std::string Name;
    std::uint32_t Parties = 1;
};

// Whether a Scenario value is the list form ("dm:10,rfc:10", or one "dm:10") rather than one plain name.
bool IsScenarioList(std::string const& value);

// The entries of a Scenario list, in order. A bad entry or a repeated name is dropped with a warning; the parties are cut at kMaxScenarioParties in all (the
// entry that crosses it is shortened, the ones after it dropped).
std::vector<ScenarioListItem> ParseScenarioList(std::string const& value, std::vector<std::string>& warnings);

// The item entries of one gear list file (ScenarioGearFile, <classId>.txt): an entry at the start of a line, '#' comments, blank lines skipped, a repeated entry kept once.
std::vector<std::uint32_t> ParseGearList(std::string const& text);

// The config one dungeon of a Scenario list reads its scenario from: `config` without the plain dungeon keys and without every "<key>.<id>" override, then
// the overrides of `name` as plain keys, and Scenario = name, ScenarioParties = parties, Dungeon = name. The dungeon file merge (dungeon_data.h) and the
// settings both use it, so they see the same keys.
std::unordered_map<std::string, std::string> ScenarioConfigFor(std::unordered_map<std::string, std::string> const& config, std::string const& name,
                                                               std::uint32_t parties);

// Where the bot of spawn slot `slot` (0-based, in spawn order) belongs: the dungeon (index in the list), the party over all dungeons (the first dungeon's parties
// first), and the race and class of its place in that dungeon's roster. Valid is false for a slot past the last party.
struct ScenarioSlot
{
    bool Valid = false;
    std::size_t Dungeon = 0;
    std::size_t Party = 0;
    std::uint32_t Race = 0;
    std::uint32_t Class = 0;
    std::string Spec;      // the roster entry's spec (race:class:spec), "" when it has none
};
ScenarioSlot ScenarioSlotOf(std::vector<ScenarioSettings> const& scenarios, std::size_t slot);

// The bots and the parties of all the scenario's dungeons, and the dungeon of party `party` (counted over all of them; the last dungeon for a party past the end).
std::uint32_t ScenarioBotCount(std::vector<ScenarioSettings> const& scenarios);
std::uint32_t ScenarioPartyCount(std::vector<ScenarioSettings> const& scenarios);
std::size_t ScenarioDungeonOfParty(std::vector<ScenarioSettings> const& scenarios, std::size_t party);

// The most bots any configuration may ask for; also the Tier 2 bot bound requested at handshake.
constexpr std::uint32_t kMaxBotsCeiling = 2500;

// The number of bots the module may control: the configured MaxBots, capped at the Tier 2 bot grant the core returned at
// handshake. Without a Tier 2 region the module cannot drive bots, so it asks for none.
std::uint32_t EffectiveMaxBots(std::uint32_t configured, std::uint32_t grantedMaxBots, bool tier2Granted);

// Reads the settings from the config the core delivered. Missing or invalid values fall back to the defaults, and each
// fallback adds one line to `warnings` so the operator can see it in the module's log.
BotSettings ParseBotSettings(std::unordered_map<std::string, std::string> const& config, std::vector<std::string>& warnings);
