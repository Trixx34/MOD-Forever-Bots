// SPDX-License-Identifier: GPL-2.0-or-later
#include "world_events.h"

#include "bot_random.h"
#include "text_number.h"
#include "text_split.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace world_events {

namespace {

constexpr std::uint32_t kSaltWorldEvent = 40;
constexpr std::size_t kNearestChoices = 3;    // a bot picks one of the nearest few sites, so a crowd does not take the same one

std::string Underscored(std::string text)
{
    for (char& c : text)
        if (c == ' ' || c == ',' || c == '=' || c == '|')
            c = '_';
    return text;
}

} // namespace

std::vector<Site> ParseSites(std::string_view text, std::vector<std::string>& errors)
{
    std::vector<Site> sites;
    std::size_t lineNo = 0;
    std::size_t start = 0;
    while (start <= text.size())
    {
        std::size_t const end = text.find('\n', start);
        std::string_view line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        start = end == std::string_view::npos ? text.size() + 1 : end + 1;
        ++lineNo;
        line = text_split::TrimBlanks(line);
        if (line.empty() || line.front() == '#')
            continue;

        auto const fields = text_split::Split(line, '|');
        Site site;
        bool ok = fields.size() == 10 && text_number::ParseWhole(std::string(fields[0]), site.Event) && site.Event != 0 &&
                  text_number::ParseWhole(std::string(fields[1]), site.Side) && site.Side <= 2 &&
                  text_number::ParseWhole(std::string(fields[2]), site.Map) && text_number::ParseDecimal(std::string(fields[3]), site.X) &&
                  text_number::ParseDecimal(std::string(fields[4]), site.Y) && text_number::ParseDecimal(std::string(fields[5]), site.Z) &&
                  text_number::ParseWhole(std::string(fields[6]), site.MinLevel) && text_number::ParseWhole(std::string(fields[7]), site.MaxLevel) &&
                  site.MinLevel <= site.MaxLevel && !text_split::TrimBlanks(fields[8]).empty();
        if (ok)
        {
            site.Name = std::string(text_split::TrimBlanks(fields[8]));
            std::string_view const quests = text_split::TrimBlanks(fields[9]);
            if (!quests.empty())
            {
                for (std::string_view part : text_split::Split(quests, ','))
                {
                    std::uint32_t id = 0;
                    if (!text_number::ParseWhole(std::string(text_split::TrimBlanks(part)), id) || id == 0)
                    {
                        ok = false;
                        break;
                    }
                    site.Quests.push_back(id);
                }
            }
        }
        if (!ok)
        {
            errors.push_back("line " + std::to_string(lineNo) + ": expected event|side|map|x|y|z|minLevel|maxLevel|name|quests (side 0, 1 or 2; quests a comma list or empty)");
            continue;
        }
        sites.push_back(std::move(site));
    }
    return sites;
}

std::vector<std::uint32_t> ParseEventIds(std::string_view text, std::vector<std::string>& errors)
{
    std::vector<std::uint32_t> ids;
    for (std::string_view part : text_split::Split(text, ','))
    {
        part = text_split::TrimBlanks(part);
        if (part.empty())
            continue;
        std::uint32_t id = 0;
        if (!text_number::ParseWhole(std::string(part), id) || id == 0)
        {
            errors.push_back("not an event id: '" + std::string(part) + "'");
            continue;
        }
        if (std::find(ids.begin(), ids.end(), id) == ids.end())
            ids.push_back(id);
    }
    return ids;
}

std::uint32_t AllowedAtOnce(Settings const& settings, std::size_t botCount)
{
    if (!settings.Enabled || settings.SharePct == 0 || settings.MaxBots == 0 || botCount == 0)
        return 0;
    std::uint64_t const share = (std::uint64_t(botCount) * settings.SharePct + 99) / 100;
    return std::uint32_t(std::min<std::uint64_t>(share, settings.MaxBots));
}

bool Suits(Site const& site, Bot const& bot, std::vector<std::uint32_t> const& active)
{
    if (std::find(active.begin(), active.end(), site.Event) == active.end())
        return false;
    if (site.Side != 0 && site.Side != bot.Side)
        return false;
    return bot.Map == site.Map && bot.Level >= site.MinLevel && bot.Level <= site.MaxLevel;
}

Decisions Scheduler::Tick(std::uint64_t nowMs, std::vector<Site> const& sites, std::vector<std::uint32_t> const& active, std::vector<Bot> const& bots)
{
    Decisions out;
    if (!_settings.Enabled)
    {
        for (auto const& [guid, run] : _running)
            out.Ends.push_back({ guid, run.Site, EndReason::kUnavailable, nowMs - run.StartMs });
        _running.clear();
        return out;
    }

    std::unordered_map<std::uint64_t, Bot const*> byGuid;
    for (Bot const& bot : bots)
        byGuid[bot.Guid] = &bot;
    auto const eventActive = [&active](std::size_t site, std::vector<Site> const& all)
    {
        return site < all.size() && std::find(active.begin(), active.end(), all[site].Event) != active.end();
    };

    // The visited sites of an event that is over are forgotten: the event may come back next year.
    for (auto it = _visited.begin(); it != _visited.end();)
        it = eventActive(it->second, sites) ? std::next(it) : _visited.erase(it);

    // Ends, in guid order so the result is the same every run.
    std::vector<std::uint64_t> running;
    for (auto const& [guid, run] : _running)
        running.push_back(guid);
    std::sort(running.begin(), running.end());
    for (std::uint64_t guid : running)
    {
        Running const run = _running[guid];
        auto const found = byGuid.find(guid);
        std::optional<EndReason> why;
        if (found == byGuid.end() || !found->second->Available)
            why = found != byGuid.end() && found->second->Called ? EndReason::kCalled : EndReason::kUnavailable;
        else if (found->second->Called)
            why = EndReason::kCalled;
        else if (!eventActive(run.Site, sites))
            why = EndReason::kEventOver;
        else if (nowMs - run.StartMs >= std::uint64_t(_settings.MaxMinutes) * 60000)
            why = EndReason::kTimeLimit;
        if (why)
        {
            out.Ends.push_back({ guid, run.Site, *why, nowMs - run.StartMs });
            _lastEndMs[guid] = nowMs;
            _running.erase(guid);
        }
    }

    if (active.empty() || sites.empty())
        return out;
    std::uint32_t const allowed = AllowedAtOnce(_settings, bots.size());
    if (_running.size() >= allowed)
        return out;

    std::unordered_map<std::size_t, std::uint32_t> perSite;
    for (auto const& [guid, run] : _running)
        ++perSite[run.Site];

    // The bots in an order that changes every 10 seconds, so the same low guids do not always go first.
    std::vector<Bot const*> order;
    for (Bot const& bot : bots)
        if (bot.Available && !bot.Called && _running.count(bot.Guid) == 0)
            order.push_back(&bot);
    std::uint64_t const bucket = nowMs / 10000;
    std::sort(order.begin(), order.end(), [&](Bot const* a, Bot const* b)
    {
        return bot_random::Draw(_settings.Seed, a->Guid, kSaltWorldEvent, bucket) < bot_random::Draw(_settings.Seed, b->Guid, kSaltWorldEvent, bucket);
    });

    for (Bot const* bot : order)
    {
        if (_running.size() >= allowed || out.Starts.size() >= _settings.MaxStartsPerTick)
            break;
        if (auto const last = _lastEndMs.find(bot->Guid); last != _lastEndMs.end() && nowMs - last->second < std::uint64_t(_settings.GapMinutes) * 60000)
            continue;
        std::vector<std::pair<float, std::size_t>> options;
        for (std::size_t i = 0; i < sites.size(); ++i)
        {
            if (!Suits(sites[i], *bot, active) || _visited.count({ bot->Guid, i }) != 0 || perSite[i] >= _settings.MaxPerSite)
                continue;
            options.emplace_back(std::hypot(sites[i].X - bot->X, sites[i].Y - bot->Y), i);
        }
        if (options.empty())
            continue;
        std::sort(options.begin(), options.end());
        std::size_t const pool = std::min(options.size(), kNearestChoices);
        std::size_t const pick = options[bot_random::Below(bot_random::Draw(_settings.Seed, bot->Guid, kSaltWorldEvent + 1, bucket), std::uint32_t(pool))].second;
        _running[bot->Guid] = Running{ pick, nowMs };
        ++perSite[pick];
        out.Starts.push_back({ bot->Guid, pick });
    }
    return out;
}

std::uint64_t Scheduler::Finish(std::uint64_t guid, std::uint64_t nowMs)
{
    std::uint64_t took = 0;
    if (auto const it = _running.find(guid); it != _running.end())
    {
        took = nowMs - it->second.StartMs;
        _visited.insert({ guid, it->second.Site });
        _running.erase(it);
    }
    _lastEndMs[guid] = nowMs;
    return took;
}

std::string StartLine(std::uint64_t guid, Site const& site)
{
    return "world_event_start bot=" + std::to_string(guid) + " event=" + std::to_string(site.Event) + " site=" + Underscored(site.Name) +
           " quests=" + std::to_string(site.Quests.size());
}

std::string EndLine(std::uint64_t guid, Site const& site, EndReason why, bool failed, std::uint64_t tookMs)
{
    std::string const common = " bot=" + std::to_string(guid) + " event=" + std::to_string(site.Event) + " site=" + Underscored(site.Name) +
                               " took_s=" + std::to_string(tookMs / 1000);
    if (failed)
        return "world_event_abandoned reason=failed" + common;
    switch (why)
    {
        case EndReason::kDone:
            return "world_event_done" + common;
        case EndReason::kTimeLimit:
            return "world_event_abandoned reason=timeout" + common;
        case EndReason::kCalled:
            return "world_event_abandoned reason=called" + common;
        case EndReason::kEventOver:
            return "world_event_abandoned reason=event_over" + common;
        case EndReason::kUnavailable:
            break;
    }
    return "world_event_abandoned reason=unavailable" + common;
}

} // namespace world_events
