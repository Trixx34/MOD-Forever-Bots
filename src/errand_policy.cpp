#include "errand_policy.h"

#include <algorithm>
#include <unordered_set>

namespace errand {

namespace {

constexpr std::uint64_t kMinMs = 60000;
constexpr std::uint64_t kDayMs = 24ull * 60 * kMinMs;

} // namespace

Decisions Scheduler::Tick(std::uint64_t nowMs, std::vector<Bot> const& bots)
{
    Decisions decisions;
    Prune(nowMs);

    // The errands that are over: the bot was called away, is no longer free, has left the module's view, or has used up its time.
    std::unordered_set<std::uint64_t> present;
    for (Bot const& bot : bots)
        present.insert(bot.Guid);
    std::vector<std::uint64_t> running;
    for (auto const& entry : _active)
        running.push_back(entry.first);
    std::sort(running.begin(), running.end());
    for (std::uint64_t guid : running)
    {
        Bot const* bot = nullptr;
        for (Bot const& candidate : bots)
            if (candidate.Guid == guid)
                bot = &candidate;
        Running const& errand = _active[guid];
        if (!bot)
            Close(guid, nowMs, EndReason::kUnavailable, decisions.Ends);
        else if (bot->Called)
            Close(guid, nowMs, EndReason::kCalled, decisions.Ends);
        else if (!bot->Available)
            Close(guid, nowMs, EndReason::kUnavailable, decisions.Ends);
        else if (nowMs - errand.StartMs >= _settings.MaxMinutes * kMinMs)
            Close(guid, nowMs, EndReason::kTimeLimit, decisions.Ends);
    }

    if (!_settings.Enabled)
        return decisions;

    // The starts: training before the Auction House, one errand per bot, within the bot's daily budget and gap, and to the capital with the fewest
    // bots out on an errand that still has room.
    for (Bot const& bot : bots)
    {
        if (decisions.Starts.size() >= _settings.MaxStartsPerTick)
            break;
        if (!bot.Available || bot.Called || Active(bot.Guid) || (!bot.TrainDue && !bot.AuctionDue) || bot.Cities.empty())
            continue;
        if (MinutesUsed(bot.Guid, nowMs) >= _settings.DailyMinutes)
            continue;
        auto const last = _lastEndMs.find(bot.Guid);
        if (last != _lastEndMs.end() && nowMs < last->second + _settings.GapMinutes * kMinMs)
            continue;

        bool found = false;
        std::uint32_t city = 0;
        std::uint32_t fewest = 0;
        for (std::uint32_t candidate : bot.Cities)
        {
            std::uint32_t const out = ActiveCount(candidate);
            if (out >= _settings.MaxPerCity)
                continue;
            if (!found || out < fewest)
            {
                found = true;
                city = candidate;
                fewest = out;
            }
        }
        if (!found)
            continue;

        Kind const what = bot.TrainDue ? Kind::kTrain : Kind::kAuction;
        _active[bot.Guid] = Running{ what, city, nowMs };
        decisions.Starts.push_back(Start{ bot.Guid, what, city });
    }
    return decisions;
}

void Scheduler::Finish(std::uint64_t guid, std::uint64_t nowMs)
{
    std::vector<End> ignored;
    if (_active.count(guid) != 0)
        Close(guid, nowMs, EndReason::kDone, ignored);
}

std::uint32_t Scheduler::ActiveCount(std::uint32_t city) const
{
    std::uint32_t count = 0;
    for (auto const& entry : _active)
        if (entry.second.City == city)
            ++count;
    return count;
}

std::uint32_t Scheduler::MinutesUsed(std::uint64_t guid, std::uint64_t nowMs) const
{
    std::uint64_t const since = nowMs > kDayMs ? nowMs - kDayMs : 0;
    std::uint64_t used = 0;
    for (Record const& record : _records)
    {
        if (record.Guid != guid || record.EndMs <= since)
            continue;
        used += record.EndMs - std::max(record.StartMs, since);
    }
    if (auto const running = _active.find(guid); running != _active.end())
        used += nowMs - std::max(running->second.StartMs, since);
    return std::uint32_t(used / kMinMs);
}

void Scheduler::Close(std::uint64_t guid, std::uint64_t nowMs, EndReason why, std::vector<End>& ends)
{
    auto const running = _active.find(guid);
    if (running == _active.end())
        return;
    _records.push_back(Record{ guid, running->second.StartMs, nowMs });
    ends.push_back(End{ guid, running->second.What, why });
    _lastEndMs[guid] = nowMs;
    _active.erase(running);
}

void Scheduler::Prune(std::uint64_t nowMs)
{
    std::uint64_t const since = nowMs > kDayMs ? nowMs - kDayMs : 0;
    _records.erase(std::remove_if(_records.begin(), _records.end(), [since](Record const& record) { return record.EndMs <= since; }), _records.end());
}

} // namespace errand
