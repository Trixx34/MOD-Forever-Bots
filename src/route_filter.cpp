// SPDX-License-Identifier: GPL-2.0-or-later
#include "route_filter.h"

#include <algorithm>
#include <cmath>

namespace route_filter {

std::uint64_t PointKey(std::uint32_t map, float x, float y, std::uint32_t cellYards)
{
    float const cell = float(std::max<std::uint32_t>(cellYards, 1));
    std::int64_t const cx = std::int64_t(std::floor(x / cell));
    std::int64_t const cy = std::int64_t(std::floor(y / cell));
    // 16 bits of map, 24 bits of x and 23 of y (a cell index never nears 4 million; a wrap only merges far cells); bit 63 stays free for SpotKey.
    return (std::uint64_t(map & 0xFFFF) << 47) | ((std::uint64_t(cx) & 0xFFFFFF) << 23) | (std::uint64_t(cy) & 0x7FFFFF);
}

std::uint64_t SpotStartKey(std::size_t spot, float fromX, float fromY, std::uint32_t startCellYards)
{
    if (startCellYards == 0)
        return SpotKey(spot);
    float const cell = float(startCellYards);
    std::uint64_t const cx = std::uint64_t(std::int64_t(std::floor(fromX / cell))) & 0xFFF;
    std::uint64_t const cy = std::uint64_t(std::int64_t(std::floor(fromY / cell))) & 0xFFF;
    return kSpotBit | (1ull << 62) | ((std::uint64_t(spot) & 0xFFFFF) << 24) | (cx << 12) | cy;
}

std::uint64_t Memory::Lifetime(std::uint32_t fails) const
{
    std::uint64_t ttl = std::uint64_t(_settings.TtlSec) * 1000;
    std::uint64_t const cap = std::uint64_t(std::max(_settings.MaxTtlSec, _settings.TtlSec)) * 1000;
    for (std::uint32_t i = 1; i < fails && ttl < cap; ++i)
        ttl *= 2;
    return std::min(ttl, cap);
}

bool Memory::Blocked(std::uint64_t bot, std::uint64_t key, std::uint64_t nowMs, std::uint64_t sharedKey)
{
    if (!_settings.Enabled)
        return false;
    if (auto const it = _own.find(bot); it != _own.end())
        for (Own const& entry : it->second)
            if (entry.Key == key && nowMs < entry.UntilMs)
                return true;
    if (auto const it = _shared.find(sharedKey ? sharedKey : key); it != _shared.end() && nowMs < it->second.UntilMs)
        return true;
    return false;
}

bool Memory::Refuse(std::uint64_t bot, std::uint64_t key, std::uint64_t nowMs, std::uint64_t sharedKey)
{
    if (sharedKey == 0)
        sharedKey = key;
    if (!_settings.Enabled)
        return false;
    std::vector<Own>& list = _own[bot];
    Own* mine = nullptr;
    for (Own& entry : list)
        if (entry.Key == key)
            mine = &entry;
    if (!mine)
    {
        // Expired entries first, then the one forgotten soonest.
        if (list.size() >= kPerBotMax)
        {
            std::erase_if(list, [nowMs](Own const& e) { return nowMs >= e.ForgetMs; });
            if (list.size() >= kPerBotMax)
                list.erase(std::min_element(list.begin(), list.end(), [](Own const& a, Own const& b) { return a.ForgetMs < b.ForgetMs; }));
        }
        list.push_back(Own{ key, 0, 0, 0 });
        mine = &list.back();
    }
    if (nowMs >= mine->UntilMs) // a refusal while the entry still holds (a stale answer) does not lengthen it
    {
        mine->Fails = nowMs < mine->ForgetMs ? mine->Fails + 1 : 1;
        std::uint64_t const ttl = Lifetime(mine->Fails);
        mine->UntilMs = nowMs + ttl;
        mine->ForgetMs = mine->UntilMs + ttl;
    }

    if (_settings.SharedBots == 0)
        return false;
    if (_shared.size() >= _settings.MaxEntries)
        PruneShared(nowMs);
    if (_shared.size() >= _settings.MaxEntries && _shared.find(sharedKey) == _shared.end())
        return false; // full of live entries: no new shared key (the bot's own memory still holds)
    Shared& entry = _shared[sharedKey];
    if (nowMs >= entry.ForgetMs)
        entry = Shared{};
    if (std::find(entry.Bots.begin(), entry.Bots.end(), bot) == entry.Bots.end() && entry.Bots.size() < _settings.SharedBots)
        entry.Bots.push_back(bot);
    // Pending reports wait for the other bots for two first lifetimes.
    if (entry.Bots.size() < _settings.SharedBots)
    {
        entry.ForgetMs = std::max(entry.ForgetMs, nowMs + 2 * Lifetime(1));
        return false;
    }
    if (nowMs < entry.UntilMs)
        return false; // already shared
    ++entry.Fails;
    std::uint64_t const shared = Lifetime(entry.Fails);
    entry.UntilMs = nowMs + shared;
    entry.ForgetMs = entry.UntilMs + shared;
    return true;
}

void Memory::Reached(std::uint64_t bot, std::uint64_t key, std::uint64_t sharedKey)
{
    if (auto const it = _own.find(bot); it != _own.end())
    {
        std::erase_if(it->second, [key](Own const& e) { return e.Key == key; });
        if (it->second.empty())
            _own.erase(it);
    }
    _shared.erase(sharedKey ? sharedKey : key);
}

void Memory::ForgetBot(std::uint64_t bot)
{
    _own.erase(bot);
}

std::size_t Memory::BotEntries(std::uint64_t bot) const
{
    auto const it = _own.find(bot);
    return it == _own.end() ? 0 : it->second.size();
}

void Memory::PruneShared(std::uint64_t nowMs)
{
    for (auto it = _shared.begin(); it != _shared.end();)
        it = nowMs >= it->second.ForgetMs ? _shared.erase(it) : std::next(it);
}

std::uint64_t ExploreIdleMs(Settings const& settings, std::uint64_t baseMs, std::uint32_t streak)
{
    if (!settings.Enabled || settings.ExploreBackoffMaxSec == 0 || streak < 3)
        return baseMs;
    std::uint64_t ms = baseMs << std::min<std::uint32_t>(streak / 3, 12);
    return std::max(baseMs, std::min<std::uint64_t>(ms, std::uint64_t(settings.ExploreBackoffMaxSec) * 1000));
}

} // namespace route_filter
