// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Private to main*.cpp: the helpers, feeds and tuning constants that were an anonymous namespace in main.cpp, now shared by its split files.

#include "bot_pick.h"
#include "bot_policy.h"
#include "bot_ramp.h"
#include "bot_roster.h"
#include "trade_policy.h"
#include "bot_settings.h"
#include "bot_presence.h"
#include "auction_policy.h"
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
inline std::string BesideTheModule(std::string const& path)
{
    if (path.empty() || path.front() == '/')
        return path;
    std::error_code ec;
    std::filesystem::path const exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec)
    {
        std::filesystem::path const candidate = exe.parent_path() / path;
        if (std::filesystem::exists(candidate, ec))
            return candidate.string();
    }
    return path;
}

constexpr std::uint32_t kItemClassQuest = 12;       // ITEM_CLASS_QUEST

// Vendor sales (sell_policy.h, doc/economy.md): the white items a bot loots are kept for its next vendor visit, and a bot sells from a vendor in reach.
constexpr std::size_t kSellItemsPerBot = 200;       // looted white items kept per bot for selling
constexpr float kSellReachYards = 4.0f;             // a bot this close to its vendor can sell to it (the core allows 5)

inline char const* ActionTypeName(forever_module::ActionType type)
{
    switch (type)
    {
        case forever_module::ActionType::kCastSpell: return "cast";
        case forever_module::ActionType::kMoveTo: return "move";
        case forever_module::ActionType::kAttackTarget: return "attack";
        case forever_module::ActionType::kFollow: return "follow";
        case forever_module::ActionType::kStop: return "stop";
        case forever_module::ActionType::kRest: return "rest";
        case forever_module::ActionType::kVendor: return "vendor";
        case forever_module::ActionType::kTrain: return "train";
        case forever_module::ActionType::kGroup: return "group";
        case forever_module::ActionType::kLoot: return "loot";
        case forever_module::ActionType::kQuest: return "quest";
        case forever_module::ActionType::kTeleport: return "teleport";
        case forever_module::ActionType::kGrant: return "grant";
        case forever_module::ActionType::kAutoGear: return "autogear";
        case forever_module::ActionType::kReleaseSpirit: return "release";
        case forever_module::ActionType::kResurrect: return "resurrect";
        case forever_module::ActionType::kReclaimCorpse: return "reclaim";
        case forever_module::ActionType::kConsume: return "consume";
        case forever_module::ActionType::kSkillUp: return "skillup";
        case forever_module::ActionType::kCraft: return "craft";
        case forever_module::ActionType::kGather: return "gather";
        case forever_module::ActionType::kSummon: return "summon";
        case forever_module::ActionType::kRevive: return "revive";
        case forever_module::ActionType::kMount: return "mount";
        case forever_module::ActionType::kPetCommand: return "pet";
        case forever_module::ActionType::kTaxi: return "taxi";
        case forever_module::ActionType::kUseObject: return "useobject";
        case forever_module::ActionType::kSellToVendor: return "sell";
        default: break;
    }
    return "action";
}

// Milliseconds on a monotonic clock, for the policy's cooldowns, the fight timer and the ramp.
inline std::uint64_t nowMs()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// Seconds since local midnight, for the ramp's stop time.
inline std::uint32_t LocalSecondsOfDay()
{
    std::time_t const now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    return static_cast<std::uint32_t>(local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec);
}

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
};

inline forever_module::SpawnParams ParamsFor(RaceClass const& combo)
{
    forever_module::SpawnParams params;
    params.set_race(combo.Race);
    params.set_class_id(combo.Class);
    return params;
}

// Stored value of a bot's row: "race class". Old rows hold "1" and carry no race or class.
inline bool ParseRaceClass(std::string const& value, RaceClass& combo)
{
    auto const space = value.find(' ');
    if (space == std::string::npos)
        return false;
    try
    {
        combo.Race = static_cast<std::uint32_t>(std::stoul(value.substr(0, space)));
        combo.Class = static_cast<std::uint32_t>(std::stoul(value.substr(space + 1)));
    }
    catch (std::exception const&)
    {
        return false;
    }
    return true;
}

// One ramp step: spawns up to kSpawnsPerRampTick bots when the run's target is above the bots owned or pending. Stops
// for good at the stop time, and backs off for a while after a refused spawn.
inline void RampTick(RunContext& run)
{
    std::uint64_t const now = nowMs();
    std::uint64_t const elapsed = now - run.StartMs;
    if (RunStopped(run.Settings.Ramp, run.StartSecOfDay, elapsed))
    {
        if (!run.StopLogged)
        {
            run.StopLogged = true;
            std::cerr << "forever-bots: stop time reached after " << elapsed / 60000 << " minutes; no more bots are added\n";
        }
        return;
    }
    if (run.Halted || now < run.BackoffUntilMs)
        return;

    std::uint32_t const target = std::min(RampTarget(run.Settings.Ramp, elapsed), run.Roster.MaxBots());
    std::uint32_t spawned = 0;
    while (spawned < kSpawnsPerRampTick && run.Roster.SpawnedCount() + run.Roster.PendingCount() < target &&
           run.Roster.CanRequest())
    {
        RaceClass combo{};
        std::string reason;
        if (!pick::Pick(run.Rng, run.Settings.Race, run.Settings.Class, combo, reason))
        {
            run.Halted = true;
            std::cerr << "forever-bots: " << reason << "; no bots are spawned\n";
            return;
        }

        ++spawned;
        run.Roster.NoteRequestSent(0);
        run.Client.RequestControl(0, ParamsFor(combo), [&run, combo](forever_module::ControlResponse const& reply)
        {
            if (!reply.granted())
            {
                run.Roster.OnDenied(0);
                run.BackoffUntilMs = nowMs() + kSpawnBackoffMs;
                std::cerr << "forever-bots: spawn refused for race " << combo.Race << " class " << combo.Class << ": "
                          << reply.reason() << "; waiting 30 s\n";
                return;
            }

            std::uint64_t const guid = reply.bot_guid();
            run.Roster.OnGranted(0, guid);
            run.Classes.Set(guid, combo.Class);
            run.Races.Set(guid, combo.Race);
            run.Client.StoreSet(bot_store::KeyFor(guid), std::to_string(combo.Race) + " " + std::to_string(combo.Class));
            std::cerr << "forever-bots: spawned bot " << guid << " race " << combo.Race << " class " << combo.Class << "\n";
        });
    }
}

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
    std::vector<BotPolicy::TalentOrder> Talents;                // talent orders for the Run() thread to send (SpendBotTalents)
    std::vector<std::uint64_t> TalentRefused;                   // bots the core refused a talent order, for the pump thread to retry
};

// The words from index `from` on, joined with single spaces: a character name can have a last name ("Tinky Winky").
inline std::string JoinWords(std::vector<std::string> const& words, std::size_t from)
{
    std::string out;
    for (std::size_t i = from; i < words.size(); ++i)
        out += (out.empty() ? "" : " ") + words[i];
    return out;
}

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

inline std::string LowerCopy(std::string text)
{
    return text_case::ToLower(std::move(text));
}

struct PresenceFeed
{
    std::mutex Lock;
    std::vector<presence::Online> Bots;
    std::uint64_t AtMs = 0;
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
};

// The loot event as a listing candidate. A weapon or a piece of armour is marked Needed: the module does not know what a bot wears or keeps
// for an upgrade, so it never lists equipment (the core's bag description would decide that; doc/economy.md).
inline auction::Candidate CandidateOf(forever_module::PlayerItemLootedEvent const& event)
{
    auction::Candidate item;
    item.ItemGuid = event.item_guid();
    item.Entry = event.item_entry();
    item.Count = event.count() ? event.count() : 1;
    item.Quality = static_cast<std::uint8_t>(event.quality());
    item.VendorSellCopper = event.sell_price();
    item.QuestItem = event.item_class() == kItemClassQuest;
    item.Needed = event.item_class() == kItemClassWeapon || event.item_class() == kItemClassArmor;
    item.Stackable = item.Count != 1;   // the core lists single items only
    return item;
}

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
inline sell::Item SellItemOf(forever_module::PlayerItemLootedEvent const& event)
{
    sell::Item item;
    item.Entry = event.item_entry();
    item.Count = event.count() ? event.count() : 1;
    item.Quality = static_cast<std::uint8_t>(event.quality());
    item.VendorSellCopper = event.sell_price();
    item.QuestItem = event.item_class() == kItemClassQuest;
    item.Equipment = event.item_class() == kItemClassWeapon || event.item_class() == kItemClassArmor;
    return item;
}

// Vendor sales (sell policy): an ordinary bot, alive, out of combat and with no other action this tick, sells the next item of its visit. A bot
// out of reach of its vendor walks there first. One action a bot a tick, and never past the actions the core granted: PublishActions drops the
// whole tick when the list runs over. A sale leaves the book only when the core has applied it; a refused sale stays for a later visit.
inline void AppendSales(SellBook& book, BotSettings const& settings, forever_module::WorldTickSnapshotView const& view,
                 std::vector<forever_module::BotAction>& actions, std::uint32_t maxActions, std::uint64_t now, std::shared_ptr<AltRegistry> const& alts)
{
    std::unordered_set<std::uint64_t> busy;
    for (forever_module::BotAction const& action : actions)
        busy.insert(action.bot_guid);
    for (forever_module::BotSnapshot const& bot : view.bots)
    {
        if (actions.size() >= maxActions)
            return;
        if (busy.count(bot.guid) || bot.group_role != 0 || (alts && alts->Has(bot.guid)) || bot.life_state != 0 || bot.in_combat != 0 ||
            bot.vendor_valid == 0 || bot.vendor_guid == 0)
            continue;

        // The sale sent last time: the core's reply to it is the bot's last action. Until the reply comes, the bot waits.
        auto const flight = book.Flight.find(bot.guid);
        if (flight != book.Flight.end())
        {
            bool const lastWasSale = bot.last_action_type == static_cast<std::uint8_t>(forever_module::ActionType::kSellToVendor) &&
                                     bot.last_action_spell == flight->second.Sent.Entry;
            sell::Outcome const outcome = sell::Resolve(flight->second, bot.last_action_sequence, lastWasSale,
                                                        bot.last_action_status == forever_module::BotActionStatus::kApplied, now);
            if (outcome == sell::Outcome::kWaiting)
                continue;
            if (outcome == sell::Outcome::kSold)
            {
                auto& items = book.Items[bot.guid];
                for (auto it = items.begin(); it != items.end();)
                    it = it->second.Entry == flight->second.Sent.Entry ? items.erase(it) : std::next(it);
            }
            book.Flight.erase(flight);
        }

        std::vector<sell::Sale>& pending = book.Pending[bot.guid];
        if (!pending.empty() && sell::VisitExpired(book.LastVisitMs[bot.guid], now))
            pending.clear();                    // the bot never got there: the items stay for the next visit
        if (pending.empty())
        {
            std::vector<sell::Item> items;
            for (auto const& [itemGuid, item] : book.Items[bot.guid])
                items.push_back(item);
            if (!sell::VisitDue(settings.Sell, !items.empty(), book.LastVisitMs[bot.guid], now))
                continue;
            book.LastVisitMs[bot.guid] = now;
            pending = sell::PlanSales(settings.Sell, items);
            if (pending.empty())
                continue;
        }

        float const dx = bot.pos_x - bot.vendor_x;
        float const dy = bot.pos_y - bot.vendor_y;
        if (dx * dx + dy * dy > kSellReachYards * kSellReachYards)
        {
            // The core found no route to the vendor (it may be far, or across water): the visit is over, the items stay for the next one. The walk used to be sent on
            // every tick without the policy's pause: 140 bots refused 5,000 times a minute.
            if (bot.last_action_type == static_cast<std::uint8_t>(forever_module::ActionType::kMoveTo) &&
                bot.last_action_status == forever_module::BotActionStatus::kNoRoute && bot.last_action_sequence != book.WalkRefusedSeq[bot.guid])
            {
                book.WalkRefusedSeq[bot.guid] = bot.last_action_sequence;
                pending.clear();
                continue;
            }
            std::uint64_t& next = book.WalkNextMs[bot.guid];
            if (now < next)
                continue;
            next = now + kSellWalkRepeatMs;
            forever_module::BotAction walk;     // the vendor's position is on the bot's map, so the walk stays on it
            walk.bot_guid = bot.guid;
            walk.type = forever_module::ActionType::kMoveTo;
            walk.dest_x = bot.vendor_x;
            walk.dest_y = bot.vendor_y;
            actions.push_back(walk);
            continue;
        }

        sell::Sale const sale = pending.front();
        pending.erase(pending.begin());

        forever_module::BotAction action;
        action.bot_guid = bot.guid;
        action.type = forever_module::ActionType::kSellToVendor;
        action.spell_id = sale.Entry;
        action.dest_x = 0.0f;                   // 0 = every one the bot carries of it
        action.target_guid = bot.vendor_guid;
        actions.push_back(action);
        book.Flight[bot.guid] = sell::InFlight{ sale, bot.last_action_sequence, now };
    }
}

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
inline std::map<std::uint32_t, PolicyOptions::DungeonRoute> DungeonRoutes(std::vector<Dungeon> const& dungeons)
{
    std::map<std::uint32_t, PolicyOptions::DungeonRoute> routes;
    for (Dungeon const& dungeon : dungeons)
    {
        PolicyOptions::DungeonRoute route;
        for (std::size_t i = 0; i < dungeon.Route.size(); ++i)
        {
            route.Route.push_back({ dungeon.Route[i].X, dungeon.Route[i].Y, dungeon.Route[i].Z });
            if (dungeon.Route[i].BossEntry != 0)
                route.BossAt[i] = dungeon.Route[i].BossEntry;
        }
        route.Events = dungeon.Events;
        // The instance entrance (the dungeon file's entry: map x y z), where a ghost of the party is revived.
        route.HasEntry = true;
        route.Entry = { dungeon.Entry[1], dungeon.Entry[2], dungeon.Entry[3] };
        // The bosses' avoid lists (bosses[].strategy.avoid_spells and avoid): the spells to step out of and the fixed places to stay out of.
        for (DungeonBoss const& boss : dungeon.Bosses)
        {
            route.AvoidSpells.insert(route.AvoidSpells.end(), boss.Strategy.AvoidSpells.begin(), boss.Strategy.AvoidSpells.end());
            for (AvoidZone const& zone : boss.Strategy.Avoid)
                route.AvoidZones.push_back(ground_avoid::Circle{ zone.X, zone.Y, zone.Radius });
        }
        routes[dungeon.MapId()] = std::move(route);
    }
    return routes;
}

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
inline void ScenarioSpawnTick(RunContext& run, std::size_t& spawned)
{
    ScenarioSlot const slot = ScenarioSlotOf(run.Settings.Scenarios, spawned);
    if (!slot.Valid || !run.Roster.CanRequest() || nowMs() < run.BackoffUntilMs)
        return;

    ScenarioSettings const& scenario = run.Settings.Scenarios[slot.Dungeon];
    RaceClass const combo{ slot.Race, slot.Class };
    forever_module::SpawnParams params = ParamsFor(combo);
    params.set_level(scenario.Level);
    if (scenario.HasStaging)
    {
        params.set_place(true);
        params.set_map_id(static_cast<std::uint32_t>(scenario.Staging[0]));
        params.set_pos_x(scenario.Staging[1]);
        params.set_pos_y(scenario.Staging[2]);
        params.set_pos_z(scenario.Staging[3]);
    }
    ++spawned;
    run.Roster.NoteRequestSent(0);
    std::string const spec = slot.Spec;
    run.Client.RequestControl(0, params, [&run, combo, spec, &spawned](forever_module::ControlResponse const& reply)
    {
        if (!reply.granted())
        {
            run.Roster.OnDenied(0);
            run.BackoffUntilMs = nowMs() + kSpawnBackoffMs;
            if (spawned > 0)
                --spawned; // try this roster entry again
            std::cerr << "forever-bots: scenario spawn refused for race " << combo.Race << " class " << combo.Class << ": "
                      << reply.reason() << "; waiting 30 s\n";
            return;
        }
        std::uint64_t const guid = reply.bot_guid();
        run.Roster.OnGranted(0, guid);
        run.Classes.Set(guid, combo.Class);
        run.Races.Set(guid, combo.Race);
        if (!spec.empty())
        {
            if (rotation::SpecRole(combo.Class, spec) != 0)
                run.Specs.Set(guid, spec);
            else
                std::cerr << "forever-bots: scenario bot " << guid << ": class " << combo.Class << " has no spec '" << spec << "'; it plays the default spec\n";
        }
        std::cerr << "forever-bots: scenario bot " << guid << " race " << combo.Race << " class " << combo.Class << (spec.empty() ? "" : " spec " + spec) << "\n";
    });
}

// Actions per tick with one decimal ("0.4"), so a rate under 1 does not print as 0.
inline std::string ActionsPerTick(std::uint64_t actions, std::uint64_t ticks)
{
    char text[32];
    std::snprintf(text, sizeof text, "%.1f", ticks ? double(actions) / double(ticks) : 0.0);
    return text;
}

// Defined in main_pump.cpp.
void PumpTier2(forever_module::Tier2Session& session, std::atomic<bool> const& running, PolicyOptions options,
               std::string const& telemetryPath, BotSettings const& settings, PumpExtras extras);
} // namespace main_detail

using namespace main_detail;
