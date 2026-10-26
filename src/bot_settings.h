// BotSettings: the module's own settings, read from its ModuleConfig (the core sends Module.forever-bots.<Key> with the
// prefix removed; see modules.d/forever-bots.conf.example). Pure logic, unit-tested in tests/settings_test.cpp.
#pragma once

#include "errand_policy.h"
#include "auction_policy.h"
#include "bot_presence.h"
#include "bot_ramp.h"
#include "sell_policy.h"
#include "supply_policy.h"
#include "taxi_policy.h"

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
//   ScenarioBossAt = "6:644,11:643"   (route point number from 1 : boss entry) the leader holds at such a point until that boss is dead
//   ScenarioRequireBosses = 0         1: the run is complete when every boss of ScenarioBosses has been seen dead (not when the last waypoint is reached)
// A dungeon file (doc/dungeons.md) fills most of these keys: DungeonFile = "data/dungeons.txt" and Dungeon = "rfc" (see dungeon_data.h).
// Several dungeons in one run (bots 0.29.0): Scenario = "dm:10,rfc:10" lists dungeon ids of the dungeon files, each with its number of parties, in order (the
// first one's parties are spawned first); at most 20 parties in all. Each listed dungeon takes all its scenario keys from its dungeon file entry; a key with
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
    std::uint32_t Level = 18;
    bool HasStaging = false;
    std::array<float, 4> Staging{};                      // map, x, y, z
    bool HasEntry = false;
    std::array<float, 4> Entry{};
    std::uint32_t DurationSec = 1200;
    std::vector<std::array<float, 3>> Route;
    std::map<std::uint32_t, std::vector<std::uint32_t>> Gear;
    // Food and drink (and anything else) every bot of the scenario is given, after its gear: item entry and count (ScenarioSupplies = "4540:5,1205:5").
    std::vector<std::pair<std::uint32_t, std::uint32_t>> Supplies;
    std::vector<std::pair<std::uint32_t, std::string>> Bosses; // creature entry, name
    std::vector<std::pair<std::uint32_t, std::uint32_t>> BossAt; // route point (0-based) and the boss entry standing there (ScenarioBossAt)
    std::string ResultFile = "scenario-results.log";
    bool WipeRecovery = false;                           // all down is not the end while a ghost may still come back
    std::uint32_t WipeRecoverySec = 300;
    std::uint32_t MinAlive = 0;                          // 0 = the understrength stop is off
    std::uint32_t LeaderDeadAbandonSec = 0;              // 0 = off
    bool RequireBosses = false;
    // ScenarioParties (1 to 20, default 1): this many parties of the roster run at the same time, each with its own director and its own dungeon instance (the core makes an
    // instance per group). Bot guids in order are cut into parties of the roster's size, as the policy's party logic does. MaxBots must cover Parties x roster.
    std::uint32_t Parties = 1;
    // ScenarioKillPriority = "entry,entry,..": creature entries in the order the party kills them (the dungeon file's kill_priority).
    std::vector<std::uint32_t> KillPriority;
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
    // Randomness (bot_random.h): PartyPercent is the share of the bots that take part in the parties at all (config key PartyPercent, 0..100); the others
    // play on their own, and the parties are of random sizes from 2 to PartySize. RandomPct is how much the bots' timers and walks vary, +- this many percent
    // (config key RandomPct, 0..90, 0 = none). Both follow RandomSeed, so a run with the same seed behaves the same.
    std::uint32_t PartyPercent = 30;
    std::uint32_t RandomPct = 25;
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
    // Spell costs (config key SpellCostFile, see spell_costs.h): what each spell costs in mana, rage or energy, so a bot does not try a cast it cannot pay for.
    // Empty = no check (the core refuses the cast instead).
    std::string SpellCostFile;
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
    // Dungeons (doc/dungeons.md): DungeonFile = the routes (data/dungeons.txt); with it set, "tank lead" works in every dungeon of the file. DungeonEventsFile = the
    // events table for all dungeons (data/dungeon_events.txt, dungeon_events.h): doors to open, charges to fire.
    bool DungeonTeleport = false;   // DungeonTeleport = 1: lost dungeon bots are put back by teleport (tests and scenarios; needs Bot.Actions.Teleport in the core)
    // AggressiveStart = 1: bots start Aggressive, so a bot on its own pulls the creatures of its level and levels. The default stance is defensive (a bot fights only
    // what attacks it, or what its group fights); a bot that only defends itself earns almost no experience: on the 300-bot soak the deaths equalled the kills.
    bool AggressiveStart = false;
    // Crafting = 1 (default): a bot with nothing to fight makes bandages from its cloth and cooks its meat (kSkillUp, SDK 0.17; the core must have the action), which
    // raises First Aid and Cooking and uses up what fills its bags. 0 turns it off.
    bool Crafting = true;
    // DungeonTactics = "focus,interrupt,heal,rest,recover" or "all" or "none" (default: none): which of the dungeon party's tactics are on (bot_policy.h). In the first sim
    // runs (docs/99-deadmines-runs.md) all of them on did slightly worse than none (2.0 bosses and 36 deaths against 2.25 and 14), and no single tactic is proven yet, so they
    // are off until each has a run of its own that shows a gain.
    bool TacticFocus = false, TacticInterrupt = false, TacticHeal = false, TacticRest = false, TacticRecover = false;
    // Bounded policy-lab knobs; defaults match the built-in dungeon policy.
    std::uint32_t DungeonHealBelowPct = 55;
    std::uint32_t DungeonTankHealBelowPct = 75;
    std::uint32_t HealerDpsManaPct = 40;
    std::uint32_t HealerLosMoveYards = 6;
    // ActionStats = 1: every minute the module logs the ten (bot, action, spell) combinations it sent most often in that minute ("actions_top"), to find a bot that
    // repeats an action every tick. Off by default.
    bool ActionStats = false;
    // MountMinLevel (default 40): a bot asks to mount up for a long walk only from this level (classic riding is learned at 40). 0 = at any level.
    std::uint32_t MountMinLevel = 40;
    // Gathering = 1: a bot with Mining, Herbalism or Skinning asks the core to gather what is near it (kGather, SDK 0.18). Off by default.
    bool Gathering = false;
    // CraftingFociFile (data/crafting_foci.txt): the anvils, forges and cooking fires of the capitals. With it, a bot whose recipes need a focus (kNeedsFocus) walks to
    // the nearest one on its map; without it the skill just waits. Use an absolute path: the worldserver's working directory is not the module's.
    std::string CraftingFociFile;
    // AvoidGroundEffects = 1 (off by default): a bot standing inside a harmful ground effect (SDK 0.19: the core must have Bot.Snapshot.GroundEffects = 1) walks out of it.
    // AvoidSpellsFile (absolute path, data/avoid_spells.txt): the Classic dungeons' spells that leave one; a spell in it counts as harmful even if the caster is not
    // known to be hostile. The dungeon files' bosses[].strategy.avoid_spells and avoid (fixed zones) add to it for their map.
    bool AvoidGroundEffects = false;
    std::string AvoidSpellsFile;
    // CorpseRunTeleportSec (0 = off, else 10..3600): a ghost whose corpse run takes longer is put at its corpse (needs Bot.Actions.Teleport).
    std::uint32_t CorpseRunTeleportSec = 60;
    bool DungeonReviveOutOfCombat = false;  // DungeonReviveOutOfCombat = 1: a dead member of a dungeon party is revived by its leader out of combat (needs Bot.Chat.Orders.Enabled in the core)
    std::string DungeonFile;
    // DungeonDataDir (default: the folder "dungeons" beside DungeonFile) holds the shipped per-dungeon JSON files; DungeonDir is your own folder of them, whose file for a map
    // replaces the shipped one (doc/dungeon-strategy.md). Both are read again when the config is reloaded.
    std::string DungeonDataDir;
    std::string DungeonDir;
    std::string DungeonEventsFile;
    // Flight points (layout 17; config keys Taxi, TaxiMinWalkYards, TaxiMasterMaxYards, TaxiBlockedSec; off by default): a bot that is not in a party replaces a walk
    // of at least TaxiMinWalkYards with a flight from a flight master within TaxiMasterMaxYards that lies on the way. Needs Bot.Taxi.Enabled in the core (on by default).
    taxi::Settings Taxi;
    // Auction House (doc/economy.md; config keys Auction, AuctionSellPctGreen, AuctionSellPctBlue, AuctionSellPctEpic, AuctionBuyoutPct, AuctionRelistPct,
    // AuctionRunHours (2 only: the core runs every auction 2 hours), AuctionMinVendorCopper, AuctionBagFullPct, AuctionPerVisit, AuctionListedMax,
    // AuctionEverySec, AuctionBuyMaxPct, AuctionBrowsePerVisit, AuctionCallsPerTick; off by default). main.cpp drives the listing visits.
    auction::Settings Auction;
    // Vendor sales (sell policy; config keys SellVendor, SellEverySec, SellPerVisit; off by default). main.cpp sends the sales on a vendor visit.
    sell::Settings Sell;
    // Town errands (config keys Errands, ErrandDailyMin, ErrandGapMin, ErrandMaxMin, ErrandPerCity, ErrandStartsPerTick; off by default): profession
    // training trips to a capital trainer, scheduled by errand::Scheduler (errand_policy.h).
    errand::Settings Errand;
    std::string ErrandTrainerFile = "data/errand_trainers.txt"; // the trainer table (ErrandTrainerFile)
    std::uint32_t AutoGearEveryLevels = 5;
    // The scenario's dungeons, in order: one for the single form, one per listed dungeon for Scenario = "dm:10,rfc:10". Scenario is the first of them (or the empty
    // one when the scenario is off), for the code that only knows one.
    ScenarioSettings Scenario;
    std::vector<ScenarioSettings> Scenarios;
};

// The most parties a scenario runs at once, over all its dungeons.
constexpr std::uint32_t kMaxScenarioParties = 20;

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
