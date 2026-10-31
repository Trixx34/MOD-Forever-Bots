// SPDX-License-Identifier: GPL-2.0-or-later
// Town errands for bots: when an idle bot may fly to a capital for profession training or an Auction House visit, and when it must come back. Pure logic
// that takes the time as input, so it is unit-tested without a clock (tests/errand_test.cpp). Nothing here talks to the core: the module feeds it the
// bots that are free and due, and carries out the starts and ends it returns.
//
// The rules (the owner's decisions): an errand starts only for an idle bot, online or not, that is out of combat and not called to a party or a dungeon.
// Training comes before the Auction House. Each bot has a daily budget of errand minutes and a gap between errands. A bot called away mid-errand abandons
// it. Errands are spread over the capitals: a capital takes at most MaxPerCity bots at once, and a tick starts at most MaxStartsPerTick.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace errand {

enum class Kind : std::uint8_t { kTrain, kAuction };

struct Settings
{
    bool Enabled = false;                 // config key Errands (off by default)
    std::uint32_t DailyMinutes = 60;      // errand minutes a bot may spend in any rolling 24 hours (ErrandDailyMin)
    std::uint32_t GapMinutes = 120;       // a bot's next errand starts no sooner than this after its last one ended (ErrandGapMin)
    std::uint32_t MaxMinutes = 60;        // one errand runs no longer than this (ErrandMaxMin)
    std::uint32_t MaxPerCity = 2;         // bots out on an errand to one capital at once (ErrandPerCity)
    std::uint32_t MaxStartsPerTick = 2;   // errands started in one tick, the whole module (ErrandStartsPerTick)
};

// One bot as the module sees it this tick.
struct Bot
{
    std::uint64_t Guid = 0;
    bool Available = false;               // online, alive, out of combat: an errand may start or go on
    bool Called = false;                  // called to a party or a dungeon: an errand is abandoned
    bool TrainDue = false;                // a profession rank is due at a trainer (the module decides, see the profession rule)
    bool AuctionDue = false;              // sale money, mail or a full bag to take to the house
    std::vector<std::uint32_t> Cities;    // the capitals this bot may travel to (its faction's)
};

struct Start
{
    std::uint64_t Guid = 0;
    Kind What = Kind::kTrain;
    std::uint32_t City = 0;
};

enum class EndReason : std::uint8_t { kDone, kTimeLimit, kCalled, kUnavailable };

struct End
{
    std::uint64_t Guid = 0;
    Kind What = Kind::kTrain;
    EndReason Why = EndReason::kDone;
};

struct Decisions
{
    std::vector<Start> Starts;
    std::vector<End> Ends;
};

class Scheduler
{
public:
    explicit Scheduler(Settings const& settings) : _settings(settings) {}

    void SetSettings(Settings const& settings) { _settings = settings; }

    // One step: ends the errands that are over (time limit, the bot called or no longer available), then starts the due ones that may go.
    Decisions Tick(std::uint64_t nowMs, std::vector<Bot> const& bots);

    // An errand the bot finished at its destination (its training or house visit is done).
    void Finish(std::uint64_t guid, std::uint64_t nowMs);

    // The errand, if the bot is on one.
    bool Active(std::uint64_t guid) const { return _active.count(guid) != 0; }
    std::uint32_t ActiveCount(std::uint32_t city) const;

    // Errand minutes the bot has spent in the last 24 hours.
    std::uint32_t MinutesUsed(std::uint64_t guid, std::uint64_t nowMs) const;

private:
    struct Running
    {
        Kind What = Kind::kTrain;
        std::uint32_t City = 0;
        std::uint64_t StartMs = 0;
    };

    struct Record
    {
        std::uint64_t Guid = 0;
        std::uint64_t StartMs = 0;
        std::uint64_t EndMs = 0;
    };

    void Close(std::uint64_t guid, std::uint64_t nowMs, EndReason why, std::vector<End>& ends);
    void Prune(std::uint64_t nowMs);

    Settings _settings;
    std::unordered_map<std::uint64_t, Running> _active;
    std::unordered_map<std::uint64_t, std::uint64_t> _lastEndMs;
    std::vector<Record> _records;
};

// The journal lines for the errands (one per transition, "key=value" with no commas, logged as "forever-bots: event <line>"; the Grafana errand panels
// count them). Remembers each errand's start so an end can say the city, the kind and how long it took. Pure: the module passes the time in.
//   errand_start bot=<guid> city=<name> kind=train|auction
//   errand_done bot=<guid> city=<name> kind=<kind> took_s=<n>
//   errand_timeout bot=<guid> city=<name> kind=<kind> took_s=<n>
//   errand_abandoned reason=called|unavailable|failed bot=<guid> city=<name> kind=<kind> took_s=<n>
class Journal
{
public:
    using CityName = std::function<std::string(std::uint32_t)>;

    explicit Journal(CityName cityName) : _cityName(std::move(cityName)) {}

    std::string OnStart(Start const& start, std::uint64_t nowMs);
    // The end of an errand: the scheduler's End (time limit, called, unavailable) or a done/failed outcome from the walk. Empty when none was started here.
    std::optional<std::string> OnEnd(std::uint64_t guid, EndReason why, bool failed, std::uint64_t nowMs);

private:
    struct Open
    {
        Kind What = Kind::kTrain;
        std::uint32_t City = 0;
        std::uint64_t StartMs = 0;
    };

    CityName _cityName;
    std::unordered_map<std::uint64_t, Open> _open;
};

} // namespace errand
