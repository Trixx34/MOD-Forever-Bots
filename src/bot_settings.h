// BotSettings: the module's own settings, read from its ModuleConfig (the core sends Module.forever-bots.<Key> with the
// prefix removed; see modules.d/forever-bots.conf.example). Pure logic, unit-tested in tests/settings_test.cpp.
#pragma once

#include "auction_policy.h"
#include "bot_presence.h"
#include "bot_ramp.h"
#include "supply_policy.h"

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
//   ScenarioRequireBosses = 0         1: the run is complete when every boss of ScenarioBosses has been seen dead (not when the last waypoint is reached)
// A dungeon file (doc/dungeons.md) fills most of these keys: DungeonFile = "data/dungeons.txt" and Dungeon = "rfc" (see dungeon_data.h).
struct ScenarioSettings
{
    std::string Name;                                    // empty: no scenario
    std::vector<std::array<std::uint32_t, 2>> Roster;    // race, class
    std::uint32_t Level = 18;
    bool HasStaging = false;
    std::array<float, 4> Staging{};                      // map, x, y, z
    bool HasEntry = false;
    std::array<float, 4> Entry{};
    std::uint32_t DurationSec = 1200;
    std::vector<std::array<float, 3>> Route;
    std::map<std::uint32_t, std::vector<std::uint32_t>> Gear;
    std::vector<std::pair<std::uint32_t, std::string>> Bosses; // creature entry, name
    std::string ResultFile = "scenario-results.log";
    bool WipeRecovery = false;                           // all down is not the end while a ghost may still come back
    std::uint32_t WipeRecoverySec = 300;
    std::uint32_t MinAlive = 0;                          // 0 = the understrength stop is off
    std::uint32_t LeaderDeadAbandonSec = 0;              // 0 = off
    bool RequireBosses = false;
    bool HasPortal = false;
    std::array<float, 4> Portal{};                       // map, x, y, z: the instance door, outside
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
    // Seconds between vendor visits (sell grey, repair, equip), for an idle bot (config key ServiceEverySec, 60..86400). The
    // trainer check (config key TrainEverySec, 60..86400, also at once after a level-up) is off by default (0): bots learn
    // their spells on level-up, so they do not need a trainer.
    std::uint32_t ServiceEverySec = 600;
    std::uint32_t TrainEverySec = 0;
    // Parties (M8): PartySize bots per party (config key PartySize, 0 = no parties, at most 5) and PartyCount parties,
    // cut from the lowest-guid bots (config key PartyCount, 1..100).
    std::uint32_t PartySize = 0;
    std::uint32_t PartyCount = 1;
    // Hubs (M7): where an idle bot walks when it has seen nothing for a while, per map, in the order given by n. Config keys
    // Hub<map>_<n> = "x y z" (n counts from 1).
    std::map<std::uint32_t, std::vector<std::array<float, 3>>> Hubs;
    // Quests (M5): the starter quest data file (config key QuestFile, see quest_data.h). Empty turns quests off.
    std::string QuestFile;
    // QuestTeleport (0 or 1, off by default): a quest walk that has not moved for two minutes teleports to the giver. Needs
    // Bot.Actions.Teleport = 1 in the core. Off, that start is dropped and the bot grinds instead of retrying a refused teleport.
    bool QuestTeleport = false;
    // Grind spots (config key GrindFile, see grind_data.h): the places idle bots walk to for their level. Empty turns it off (hubs are used).
    std::string GrindFile;
    // Rotations (config key RotationFile, see combat_rotation.h): fight rows and upkeep buffs per class from a file. Empty = the built-in rotations.
    std::string RotationFile;
    // Auto gear (config key AutoGearing, 0 or 1): the core dresses every bot for its level when it first appears and after it gains
    // AutoGearEveryLevels levels. Off by default; when it is off, the .autogear command in a party does it for that party's bots.
    bool AutoGearing = false;
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
    // Supplies (layout 15, config keys Supplies, SupplyAmmoLow, SupplyAmmoTarget, SupplyFoodLow, SupplyFoodTarget, SupplyDrinkLow, SupplyDrinkTarget,
    // SupplyTripSec; off by default): bots buy ammunition, food and drink at vendors that sell what they can use, and eat and drink while resting.
    // Layout 16, each off by default: SupplyPotions (health and mana potions, drunk in a fight below UseHealthPotionBelowPct and
    // UseManaPotionBelowPct), SupplyBandages (used by a resting bot below UseBandageBelowPct that has no food), SupplyHealthPotionLow,
    // SupplyManaPotionLow and SupplyBandageLow (the counts under which a bot goes shopping; the core's settings decide how many it buys).
    // SupplyReagents (off by default): a bot whose cast was refused for lack of a reagent goes to a vendor that sells it; the core names the item.
    supply::Settings Supplies;
    // Presence (config keys Presence, PresenceOnlineMinMin, PresenceOnlineMaxMin, PresenceOfflineMinMin, PresenceOfflineMaxMin, PresenceMinOnline,
    // PresenceMaxOnline, PresenceDayPercent; off by default): bots log off and on like players, see bot_presence.h. Needs Bot.Presence.LogoutOnRelease
    // in the core, or a released bot stays in the world.
    presence::Settings Presence;
    // Auction House (doc/economy.md; config keys Auction, AuctionSellPctGreen, AuctionSellPctBlue, AuctionSellPctEpic, AuctionBuyoutPct, AuctionRelistPct,
    // AuctionRunHours (2, 8 or 24), AuctionMinVendorCopper, AuctionBagFullPct, AuctionPerVisit, AuctionListedMax, AuctionEverySec, AuctionBuyMaxPct,
    // AuctionBrowsePerVisit, AuctionCallsPerTick; off by default). The decisions are built (auction_policy.h); the module does not carry them out yet.
    auction::Settings Auction;
    std::uint32_t AutoGearEveryLevels = 5;
    ScenarioSettings Scenario;
};

// The most bots any configuration may ask for; also the Tier 2 bot bound requested at handshake.
constexpr std::uint32_t kMaxBotsCeiling = 2500;

// The number of bots the module may control: the configured MaxBots, capped at the Tier 2 bot grant the core returned at
// handshake. Without a Tier 2 region the module cannot drive bots, so it asks for none.
std::uint32_t EffectiveMaxBots(std::uint32_t configured, std::uint32_t grantedMaxBots, bool tier2Granted);

// Reads the settings from the config the core delivered. Missing or invalid values fall back to the defaults, and each
// fallback adds one line to `warnings` so the operator can see it in the module's log.
BotSettings ParseBotSettings(std::unordered_map<std::string, std::string> const& config, std::vector<std::string>& warnings);
