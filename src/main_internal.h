// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Private to main*.cpp: the feeds, tuning constants and helper declarations that were an anonymous namespace in main.cpp, now shared by its split files.
// The helpers are defined in main_util.cpp (paths, names, text, dungeon routes), main_spawn.cpp (clock, ramp and scenario spawns) and main_sales.cpp (loot
// candidates and vendor sales).

#include "bot_pick.h"
#include "bot_policy.h"
#include "chat_reply.h"
#include "bot_brackets.h"
#include "bot_ramp.h"
#include "bot_roster.h"
#include "trade_policy.h"
#include "bot_settings.h"
#include "bot_presence.h"
#include "auction_policy.h"
#include "gather_nodes.h"
#include "sell_policy.h"
#include "move_gate.h"
#include "errand_policy.h"
#include "errand_trainers.h"
#include "bot_store.h"
#include "bot_live.h"
#include "bot_telemetry.h"
#include "rotating_file.h"
#include "dungeon_data.h"
#include "dungeon_events.h"
#include "dungeon_files.h"
#include "scenario.h"
#include "ambient_chat.h"
#include "text_case.h"

#include "forever_module/module_client.h"
#include "forever_module/tier2_session.h"

#include <algorithm>
#include <map>
#include <tuple>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
namespace main_detail {

// Tier 2 bounds asked for at handshake. The core may grant less. Handshake comes before config, so the bot count asked
// for here is the ceiling; MaxBots from config limits how many bots are actually spawned. Every bot may act each tick, so
// the action buffer holds two actions per bot ceiling slot.
constexpr std::uint32_t kRequestedNearbyUnits = 16;
constexpr std::uint32_t kRequestedActions = 2 * kMaxBotsCeiling;

constexpr std::chrono::milliseconds kPumpInterval(5);
constexpr std::uint64_t kPumpReportMs = 60000;
constexpr std::uint64_t kServiceNoticeMs = 600000; // a refused vendor or trainer is logged once per bot per ten minutes

// The spawn ramp. Every kRampInterval the module asks for up to kSpawnsPerRampTick new bots: 2 spawn requests a second at
// the defaults. The core logs bots in at most Bot.Login.MaxPerSecond a second, so a ramp step of 100 bots takes about 50
// seconds to spawn and about 10 seconds more to log in.
constexpr std::chrono::milliseconds kRampInterval(5000);

// Presence (bot_presence.h): the pump thread tells the Run() thread which ordinary bots are online and idle every kPresenceFeedMs; the Run()
// thread decides every kPresenceInterval who logs off and who comes back.
constexpr std::chrono::milliseconds kPresenceInterval(5000);
constexpr std::uint64_t kPresenceFeedMs = 2000;
constexpr std::uint64_t kPresenceStaleMs = 15000; // a feed older than this is not acted on (the pump stalled)
constexpr std::uint32_t kSpawnsPerRampTick = 10;
constexpr std::uint64_t kSpawnBackoffMs = 30000; // after a refused spawn, wait this long before the next

// Auction house (auction_policy.h, doc/economy.md): the Run() thread visits the house for each idle bot that is due, every kAuctionInterval,
// with the bot's free bag slots and its auctioneer from the pump's feed (the same feed as presence, every kPresenceFeedMs).
constexpr std::chrono::milliseconds kAuctionInterval(10000);
constexpr std::uint32_t kBagSlotsAssumed = 16;      // the snapshot gives free slots only; a backpack's 16 stands for the total
constexpr std::size_t kAuctionItemsPerBot = 200;    // looted items kept per bot for listing
constexpr std::uint32_t kItemClassWeapon = 2;       // ITEM_CLASS_WEAPON
constexpr std::uint32_t kItemClassArmor = 4;        // ITEM_CLASS_ARMOR
// A relative data path (the defaults of RotationFile and TameBeastFile are "data/...") is looked up beside the module's executable first, since the core starts a module
// from a working directory of its own; a path that exists nowhere else is returned as given, so the load reports it.
std::string BesideTheModule(std::string const& path);

constexpr std::uint32_t kItemClassQuest = 12;       // ITEM_CLASS_QUEST
constexpr std::uint32_t kItemClassTradeGoods = 7;    // ITEM_CLASS_TRADE_GOODS: herbs, ore, cloth, leather, reagents (the goods listings)

// Vendor sales (sell_policy.h, doc/economy.md): the white items a bot loots are kept for its next vendor visit, and a bot sells from a vendor in reach.
constexpr std::size_t kSellItemsPerBot = 200;       // looted white items kept per bot for selling
constexpr float kSellReachYards = 4.0f;             // a bot this close to its vendor can sell to it (the core allows 5)

char const* ActionTypeName(forever_module::ActionType type);

// Milliseconds on a monotonic clock, for the policy's cooldowns, the fight timer and the ramp.
std::uint64_t nowMs();

// Seconds since local midnight, for the ramp's stop time.
std::uint32_t LocalSecondsOfDay();

struct PresenceFeed;

// The state the ramp and the reclaim share. Everything in it is used on the Run() thread only.
struct RunContext
{
    forever_module::ModuleClient& Client;
    BotRoster& Roster;
    pick::ClassTable& Classes;
    pick::ClassTable& Races;   // race of each bot (same table type; the value is a race id), for its faction
    BotSettings& Settings; // updated by a config reload
    pick::Stream Rng;
    std::uint64_t StartMs;
    std::uint32_t StartSecOfDay;
    std::uint64_t BackoffUntilMs = 0;
    bool StopLogged = false;
    bool Halted = false; // a fixed race and class with no valid combination: nothing is spawned
    pick::SpecTable Specs; // the spec of each scenario bot whose roster entry names one (race:class:spec)
    std::shared_ptr<PresenceFeed> Levels; // set when Presence is on: the bracket counts (brackets need it)
};

forever_module::SpawnParams ParamsFor(RaceClass const& combo);

// Stored value of a bot's row: "race class". Old rows hold "1" and carry no race or class.
bool ParseRaceClass(std::string const& value, RaceClass& combo);

// One ramp step: spawns up to kSpawnsPerRampTick bots when the run's target is above the bots owned or pending. Stops
// for good at the stop time, and backs off for a while after a refused spawn.
void RampTick(RunContext& run);

// Issuers of the .autogear command, handed from the Run() thread (where the command arrives) to the pump thread.
struct CommandQueue
{
    std::mutex Lock;
    std::vector<std::uint64_t> AutoGear; // player guids that typed .autogear
    std::vector<std::uint64_t> GearBots; // bot guids to check their bags for better gear now (a trade with them has finished)
    // Chat commands parsed on the Run() thread, applied on the pump thread; and the lines to send back to the speakers.
    struct ChatItem
    {
        BotPolicy::ChatContext Context;
        ChatCommand Command;
    };
    std::vector<ChatItem> Chat;
    std::vector<std::pair<std::uint64_t, std::string>> Replies; // speaker character guid, text
    // Town errands from the Run() thread: start (with the trainer) or cancel, applied on the pump thread before the decisions.
    struct ErrandItem
    {
        std::uint64_t Guid = 0;
        bool Cancel = false;
        ErrandOrder Order;
    };
    std::vector<ErrandItem> Errands;
    std::vector<ChatAck> Says;                                  // lines a bot says out loud for an order it took (ChatSay)
    // Ambient lines for the Run() thread to say on General or Trade (AmbientChat); the pump thread picks them.
    struct AmbientLine
    {
        std::uint64_t Bot = 0;
        bool Trade = false; // the Trade channel, else General
        std::string Text;
    };
    std::vector<AmbientLine> Ambient;
    std::vector<BotPolicy::TalentOrder> Talents;                // talent orders for the Run() thread to send (SpendBotTalents)
    std::vector<std::uint64_t> TalentRefused;                   // bots the core refused a talent order, for the pump thread to retry
    // Whispers that are not orders (WhisperReplies): read on the Run() thread, answered on the pump thread, sent from the timer.
    struct WhisperItem
    {
        std::uint64_t Player = 0;
        std::uint64_t Recipient = 0;
        std::string Text;
    };
    std::vector<WhisperItem> Whispers;
    std::vector<BotPolicy::WhisperReply> WhisperReplies;        // the answers the pump thread decided, sent from the timer
    std::set<std::uint64_t> BusyBots;                           // bots busy now: a trade request to one of them is refused (pump thread writes, Run() reads)
};

// The words from index `from` on, joined with single spaces: a character name can have a last name ("Tinky Winky").
std::string JoinWords(std::vector<std::string> const& words, std::size_t from);

// The alts this module controls (characters of players' accounts playing as bots): name (lower case) to bot guid. Read by the pump thread
// (IsAlt) and written on the Run() thread.
struct AltRegistry
{
    std::mutex Lock;
    std::map<std::string, std::uint64_t> ByName;
    std::map<std::uint64_t, std::string> ByGuid;
    bool Has(std::uint64_t guid)
    {
        std::lock_guard<std::mutex> lock(Lock);
        return ByGuid.count(guid) != 0;
    }
};

std::string LowerCopy(std::string text);

struct PresenceFeed
{
    std::mutex Lock;
    std::vector<presence::Online> Bots;
    std::uint64_t AtMs = 0;
    // The level of each ordinary bot seen in the feed, kept after it goes offline: the brackets count every bot, online or not (bot_brackets.h).
    std::unordered_map<std::uint64_t, std::uint32_t> Levels;
};

// One ordinary bot as the auction visit needs it: idle (alive and out of combat), its free bag slots and the auctioneer NPC the core found.
struct AuctionBot
{
    std::uint64_t Guid = 0;
    std::uint32_t FreeSlots = 0;
    std::uint64_t AuctioneerGuid = 0;
    bool Idle = false;
    std::uint64_t Copper = 0;   // for the budget of a purchase (AuctionBuy)
    std::uint32_t Level = 0;
};

struct AuctionFeed
{
    std::mutex Lock;
    std::vector<AuctionBot> Bots;
    std::uint64_t AtMs = 0;
};

// Town errands (errand_policy.h, errand_trainers.h): the pump's view of each bot, the orders for the pump, and how each errand ended. The Run() thread
// decides the starts and ends from the feed every kErrandInterval; the pump applies the orders and reports the outcomes.
constexpr std::chrono::milliseconds kErrandInterval(10000);
struct ErrandBot
{
    std::uint64_t Guid = 0;
    bool Available = false;   // alive, out of combat, not on a flight
    bool Called = false;      // in a party: called, so an errand is abandoned
    bool TrainDue = false;    // a primary profession below its maximum
    std::uint32_t Side = 0;   // 1 Alliance, 2 Horde (0 = unknown: no errand)
    std::uint32_t Skill = 0;  // the due profession's skill line
    std::uint32_t Map = 0;
    float X = 0.0f, Y = 0.0f, Z = 0.0f;
    bool AuctioneerValid = false; // the snapshot found the bot's faction's auctioneer on its map
    float AuctioneerX = 0.0f, AuctioneerY = 0.0f, AuctioneerZ = 0.0f;
};

struct ErrandFeed
{
    std::mutex Lock;
    std::vector<ErrandBot> Bots;
    std::vector<std::pair<std::uint64_t, ErrandOutcome>> Outcomes; // taken by the Run() thread; kept until then
    std::uint64_t AtMs = 0;
};

// The auction state, used on the Run() thread only (the loot notify and the visit timer both run there).
struct AuctionBook
{
    std::unordered_map<std::uint64_t, std::unordered_map<std::uint64_t, auction::Candidate>> Items; // bot -> item guid -> what it looted
    std::unordered_map<std::uint64_t, auction::Standing> Standing;                                    // bot -> attempts, last visit
    std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> Ends;                             // bot -> when its listings run out
    std::unordered_map<std::uint64_t, std::vector<std::pair<std::uint64_t, std::uint32_t>>> GoodsEnds; // bot -> (when a goods listing runs out, its item entry)
};

// The loot event as a listing candidate. A weapon or a piece of armour is marked Needed: the module does not know what a bot wears or keeps
// for an upgrade, so it never lists equipment (the core's bag description would decide that; doc/economy.md).
auction::Candidate CandidateOf(forever_module::PlayerItemLootedEvent const& event);

// The vendor sales, used on the Run() thread only: what each bot has looted that it may sell, and the sales its visit still has to make.
struct SellBook
{
    std::unordered_map<std::uint64_t, std::unordered_map<std::uint64_t, sell::Item>> Items; // bot -> item guid -> what it looted
    std::unordered_map<std::uint64_t, std::vector<sell::Sale>> Pending;                     // bot -> sales of the visit under way
    std::unordered_map<std::uint64_t, std::uint64_t> LastVisitMs;                           // bot -> when it last visited a vendor to sell
    std::unordered_map<std::uint64_t, sell::InFlight> Flight;                               // bot -> the sale sent, waiting for the core's reply
    std::unordered_map<std::uint64_t, std::uint64_t> WalkNextMs;                            // bot -> no walk to the vendor before this (it was sent every quiet tick)
    std::unordered_map<std::uint64_t, std::uint32_t> WalkRefusedSeq;                        // bot -> the last "no route" answer that ended a visit
};

constexpr std::uint64_t kSellWalkRepeatMs = 3000;    // a walk to the vendor is repeated at most this often

// The loot event as a sale candidate.
sell::Item SellItemOf(forever_module::PlayerItemLootedEvent const& event);

// Vendor sales (sell policy): an ordinary bot, alive, out of combat and with no other action this tick, sells the next item of its visit. A bot
// out of reach of its vendor walks there first. One action a bot a tick, and never past the actions the core granted: PublishActions drops the
// whole tick when the list runs over. A sale leaves the book only when the core has applied it; a refused sale stays for a later visit.
void AppendSales(SellBook& book, BotSettings const& settings, forever_module::WorldTickSnapshotView const& view,
                 std::vector<forever_module::BotAction>& actions, std::uint32_t maxActions, std::uint64_t now, std::shared_ptr<AltRegistry> const& alts);

// What the pump needs beyond the policy: the scenario (if one is configured) and the command queue.
// The places the dungeon files are read from, handed from the config callback to the pump when the config is delivered again (a core reload): the pump then
// reads the dungeons again and gives them to the policy, so a changed JSON file takes effect without a restart.
struct DungeonReloadBox
{
    std::atomic<std::uint64_t> Generation{ 0 };
    std::mutex Lock;
    DungeonSources Sources;
};

// The routes "tank lead" can run, by map: from every dungeon of a loaded set.
std::map<std::uint32_t, PolicyOptions::DungeonRoute> DungeonRoutes(std::vector<Dungeon> const& dungeons);

struct PumpExtras
{
    std::shared_ptr<DungeonReloadBox> Dungeons; // set always; the pump reads the dungeon files again when its Generation moves
    std::shared_ptr<PresenceFeed> Presence;   // set when Presence is on
    std::shared_ptr<AuctionFeed> Auction;     // set when Auction is on
    std::shared_ptr<SellBook> Sell;           // set when vendor sales are on
    std::shared_ptr<ErrandFeed> Errand;       // set when Errands is on
    std::shared_ptr<AltRegistry> Alts;
    // The scenario's dungeons, in order (one for the single form, several for Scenario = "dm:10,rfc:10"): each with its plan, its number of parallel parties (each
    // with its own director) and the route, events, kill priority and portal its parties use.
    struct ScenarioDungeon
    {
        ScenarioPlan Plan;
        std::uint32_t Parties = 1;
        PolicyOptions::ScenarioRoute Route;
    };
    std::vector<ScenarioDungeon> Scenarios;
    std::string ScenarioResultFile;
    std::shared_ptr<CommandQueue> Commands;
};

// The scenario's bot to spawn next, one per tick of the spawn timer, with its level and its staging place. The slots go in party order, the first dungeon's
// parties first, so the bots' guids cut into parties in order (as the policy and the directors cut them) give each party the roster of its own dungeon.
void ScenarioSpawnTick(RunContext& run, std::size_t& spawned);

// Actions per tick with one decimal ("0.4"), so a rate under 1 does not print as 0.
std::string ActionsPerTick(std::uint64_t actions, std::uint64_t ticks);

// main(), step by step: the callbacks and timers it registers and the policy options it builds. main.cpp: WatchConfig to StartSpawns; main_commands.cpp:
// CommandScriptReply to SetUpChat; main_options.cpp: PolicyFromSettings to SetUpScenario; main_feeds.cpp: SetUpPresence to SetUpErrands. A reference
// argument is main()'s own object: the callbacks capture it, so it must live until client.Run() returns (it does, in main()'s frame).
void WatchConfig(forever_module::ModuleClient& client, BotSettings& settings, bool& configured, BotRoster& roster, bool tier2Granted, std::uint32_t grantedBots,
                 std::shared_ptr<DungeonReloadBox> const& dungeonBox);
void WatchRevocations(forever_module::ModuleClient& client, BotRoster& roster, std::shared_ptr<AltRegistry> const& alts);
void ReclaimBot(RunContext& run, std::uint64_t guid);
void StartSpawns(forever_module::ModuleClient& client, RunContext& run, BotRoster& roster, BotSettings& settings, std::uint32_t maxBots, bool scenarioOn,
                 std::size_t& scenarioSpawned);
forever_module::EventReply CommandScriptReply(forever_module::EventRequest const& request, std::shared_ptr<CommandQueue> const& commands,
                                              std::shared_ptr<AltRegistry> const& alts, RunContext& run, BotSettings& settings);
void BotsAltCommand(forever_module::EventReply& reply, std::vector<std::string> const& commandWords, std::uint64_t issuerCounter,
                    std::shared_ptr<AltRegistry> const& alts, RunContext& run);
void BotsSpawnCommand(forever_module::EventReply& reply, std::vector<std::string> const& commandWords, std::uint64_t issuerCounter, RunContext& run);
void SetUpTrade(forever_module::ModuleClient& client, std::shared_ptr<trade::TradeBook> const& trades, std::shared_ptr<std::function<void(std::vector<trade::Step> const&)>> const& runTrade,
                std::shared_ptr<CommandQueue> const& commands);
void SetUpChat(forever_module::ModuleClient& client, std::shared_ptr<CommandQueue> const& commands, BotSettings& settings, std::shared_ptr<trade::TradeBook> const& trades,
               std::shared_ptr<std::function<void(std::vector<trade::Step> const&)>> const& runTrade);
void PolicyFromSettings(PolicyOptions& policyOptions, BotSettings& settings);
std::vector<dungeon_events::Event> LoadDungeons(PolicyOptions& policyOptions, BotSettings& settings, std::shared_ptr<DungeonReloadBox> const& dungeonBox);
void LoadPolicyTables(PolicyOptions& policyOptions, BotSettings& settings);
void ConnectPolicyLookups(PolicyOptions& policyOptions, BotSettings& settings, pick::ClassTable& races, pick::ClassTable& classes, RunContext& run,
                          std::shared_ptr<AltRegistry> const& alts);
void SetUpScenario(PolicyOptions& policyOptions, PumpExtras& extras, BotSettings& settings, std::vector<dungeon_events::Event> const& allEvents);
void SetUpPresence(forever_module::ModuleClient& client, RunContext& run, BotSettings& settings, PumpExtras& extras, std::shared_ptr<trade::TradeBook> const& trades);
void SetUpLootBooks(forever_module::ModuleClient& client, RunContext& run, BotSettings& settings, std::shared_ptr<AuctionBook> const& book,
                    std::shared_ptr<SellBook> const& sells, PumpExtras& extras);
void SetUpAuction(forever_module::ModuleClient& client, RunContext& run, BotSettings& settings, std::shared_ptr<AuctionBook> const& book, PumpExtras& extras);
void SetUpErrands(forever_module::ModuleClient& client, BotSettings& settings, std::shared_ptr<CommandQueue> const& commands, std::shared_ptr<AuctionBook> const& book,
                  PumpExtras& extras);

// Defined in main_pump.cpp.
void PumpTier2(forever_module::Tier2Session& session, std::atomic<bool> const& running, PolicyOptions options,
               std::string const& telemetryPath, BotSettings const& settings, PumpExtras extras);
} // namespace main_detail

using namespace main_detail;
