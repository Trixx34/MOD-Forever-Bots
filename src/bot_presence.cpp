// SPDX-License-Identifier: GPL-2.0-or-later
#include "bot_presence.h"

#include <algorithm>
#include <unordered_set>

namespace presence {

Scheduler::Scheduler(Settings const& settings, std::uint64_t seed) : _settings(settings), _rng(seed) {}

std::uint64_t Scheduler::Minutes(std::uint32_t low, std::uint32_t high)
{
    if (high < low)
        high = low;
    std::uniform_int_distribution<std::uint64_t> pick(std::uint64_t(low) * 60000, std::uint64_t(high) * 60000);
    return pick(_rng);
}

std::uint32_t Scheduler::AllowedOnline(std::uint32_t secOfDay, std::uint32_t population) const
{
    // Linear between the hour's value and the next one's, so the crowd changes steadily rather than in steps on the hour.
    std::uint32_t const hour = (secOfDay / 3600) % 24;
    std::uint32_t const next = (hour + 1) % 24;
    std::uint64_t const into = secOfDay % 3600;
    std::uint64_t const percentTimes3600 = std::uint64_t(_settings.DayPercent[hour]) * (3600 - into) + std::uint64_t(_settings.DayPercent[next]) * into;
    // Rounded up, so a few percent of a small population is still one bot.
    std::uint64_t allowed = (percentTimes3600 * population + 3600ull * 100 - 1) / (3600ull * 100);
    allowed = std::min<std::uint64_t>(allowed, population);
    allowed = std::max<std::uint64_t>(allowed, std::min<std::uint32_t>(_settings.MinOnline, population));
    if (_settings.MaxOnline != 0) // the cap on bots at once wins over the minimum
        allowed = std::min<std::uint64_t>(allowed, _settings.MaxOnline);
    return std::uint32_t(allowed);
}

void Scheduler::Forget(std::uint64_t guid)
{
    _bots.erase(guid);
}

void Scheduler::LogOnFailed(std::uint64_t guid, std::uint64_t nowMs)
{
    auto const it = _bots.find(guid);
    if (it == _bots.end())
        return;
    it->second.Away = true;
    it->second.Returning = false;
    it->second.LeftMs = 0;
    it->second.DueMs = nowMs + _settings.RetryMs;
}

std::uint32_t Scheduler::OfflineCount() const
{
    std::uint32_t count = 0;
    for (auto const& [guid, entry] : _bots)
        if (entry.Away)
            ++count;
    return count;
}

Decisions Scheduler::Tick(std::uint64_t nowMs, std::uint32_t secOfDay, std::vector<Online> const& online)
{
    Decisions out;
    if (!_settings.Enabled)
        return out;

    // Bots seen online: new ones get a session that is already partly over (so they do not all leave together), returning ones are back.
    std::unordered_set<std::uint64_t> seen;
    for (Online const& bot : online)
    {
        auto [it, added] = _bots.try_emplace(bot.Guid);
        Entry& entry = it->second;
        if (entry.Away && nowMs - entry.LeftMs < _settings.LeaveGraceMs && !added)
            continue;   // told to log off and not gone yet
        seen.insert(bot.Guid);
        entry.SeenMs = nowMs;
        if (added)
        {
            std::uint64_t const session = Minutes(_settings.OnlineMinMin, _settings.OnlineMaxMin);
            std::uniform_int_distribution<std::uint64_t> part(session / 10, session);
            entry.DueMs = nowMs + part(_rng);
        }
        else if (entry.Away || entry.Returning)
        {
            // Back in the world (a returning bot), or never left (an absence the module did not carry out): a fresh session.
            entry.Away = false;
            entry.Returning = false;
            entry.DueMs = nowMs + Minutes(_settings.OnlineMinMin, _settings.OnlineMaxMin);
        }
    }
    // A bot sent back that has not shown up in time is away again; one that vanished without being logged off is forgotten after a while.
    for (auto it = _bots.begin(); it != _bots.end();)
    {
        Entry& entry = it->second;
        if (entry.Returning && !seen.count(it->first) && nowMs >= entry.DueMs)
        {
            entry.Returning = false;
            entry.Away = true;
            entry.LeftMs = 0;
            entry.DueMs = nowMs + _settings.RetryMs;
        }
        if (!entry.Away && !entry.Returning && !seen.count(it->first) && nowMs - entry.SeenMs > _settings.ForgetAfterMs)
            it = _bots.erase(it);
        else
            ++it;
    }

    std::uint32_t onlineNow = 0;
    for (auto const& [guid, entry] : _bots)
        if (!entry.Away)
            ++onlineNow; // online and returning both count: a returning bot is about to be there
    std::uint32_t const population = std::uint32_t(_bots.size());
    std::uint32_t const allowed = AllowedOnline(secOfDay, population);
    std::uint32_t changes = 0;

    // Log offs: a session that has run its time, or the crowd is bigger than the hour allows (then the soonest-due go first). Only idle bots.
    std::vector<Online const*> candidates;
    for (Online const& bot : online)
    {
        auto const it = _bots.find(bot.Guid);
        if (it != _bots.end() && !it->second.Away && !it->second.Returning)
            candidates.push_back(&bot);
    }
    std::sort(candidates.begin(), candidates.end(), [this](Online const* a, Online const* b)
    {
        std::uint64_t const da = _bots.at(a->Guid).DueMs;
        std::uint64_t const db = _bots.at(b->Guid).DueMs;
        return da != db ? da < db : a->Guid < b->Guid;
    });
    for (Online const* bot : candidates)
    {
        if (changes >= _settings.MaxChangesPerTick)
            break;
        Entry& entry = _bots.at(bot->Guid);
        bool const due = nowMs >= entry.DueMs;
        bool const crowded = onlineNow > allowed;
        if (!due && !crowded)
            continue;
        if (onlineNow <= _settings.MinOnline)
            break;
        if (!bot->Idle)
        {
            if (due)
                entry.DueMs = nowMs + _settings.RetryMs;
            continue;
        }
        entry.Away = true;
        entry.LeftMs = nowMs;
        entry.DueMs = nowMs + Minutes(_settings.OfflineMinMin, _settings.OfflineMaxMin);
        out.LogOff.push_back(bot->Guid);
        --onlineNow;
        ++changes;
    }

    // Log ons: an absence that has run its time and there is room under the hour's ceiling and the cap on bots at once; or the minimum online is
    // not met (then the soonest-due come back early).
    std::vector<std::pair<std::uint64_t, std::uint64_t>> away; // due, guid
    for (auto const& [guid, entry] : _bots)
        if (entry.Away && !seen.count(guid))
            away.emplace_back(entry.DueMs, guid);
    std::sort(away.begin(), away.end());
    for (auto const& [due, guid] : away)
    {
        if (changes >= _settings.MaxChangesPerTick)
            break;
        bool const capOk = _settings.MaxOnline == 0 || onlineNow < _settings.MaxOnline;
        bool const wanted = capOk && ((nowMs >= due && onlineNow < allowed) || onlineNow < _settings.MinOnline);
        if (!wanted)
        {
            if (nowMs >= due)
                _bots.at(guid).DueMs = nowMs + _settings.RetryMs;
            continue;
        }
        Entry& entry = _bots.at(guid);
        // Sent back: it has LoginGraceMs to appear, and a session once it has.
        entry.Away = false;
        entry.Returning = true;
        entry.DueMs = nowMs + _settings.LoginGraceMs;
        out.LogOn.push_back(guid);
        ++onlineNow;
        ++changes;
    }
    return out;
}

} // namespace presence
