// SPDX-License-Identifier: GPL-2.0-or-later
#include "damage_done.h"

#include <set>

namespace damage_done
{

void Tracker::Observe(std::vector<Sample> const& samples, std::uint64_t nowMs)
{
    // The bots on each target this tick, and the target's health (the same for every bot of it).
    std::unordered_map<std::uint64_t, std::vector<Sample const*>> byTarget;
    for (Sample const& sample : samples)
        if (sample.Target != 0)
            byTarget[sample.Target].push_back(&sample);
    for (auto const& [target, bots] : byTarget)
    {
        std::uint32_t const health = bots.front()->Health;
        auto const it = _seen.find(target);
        if (it != _seen.end() && nowMs >= it->second.AtMs && nowMs - it->second.AtMs <= kMaxGapMs && it->second.Health > health)
        {
            std::uint64_t const drop = it->second.Health - health;
            std::uint64_t const share = drop / bots.size();
            std::uint64_t remainder = drop - share * bots.size();
            for (Sample const* bot : bots)
            {
                std::uint64_t const part = share + (remainder != 0 ? 1 : 0);
                remainder -= remainder != 0 ? 1 : 0;
                _byBot[bot->Bot] += part;
                _byClass[bot->Class] += part;
                _classOfBot[bot->Bot] = bot->Class;
            }
        }
        _seen[target] = Seen{ health, nowMs };
    }
    // A target nobody has now keeps no baseline (a unit that comes back later must not count its regeneration or a gap as damage).
    for (auto it = _seen.begin(); it != _seen.end();)
        it = it->second.AtMs != nowMs ? _seen.erase(it) : std::next(it);
}

void Tracker::Clear()
{
    _byBot.clear();
    _byClass.clear();
    _classOfBot.clear();
}

std::uint64_t Tracker::Total() const
{
    std::uint64_t total = 0;
    for (auto const& [bot, damage] : _byBot)
        total += damage;
    return total;
}

std::string Tracker::Format() const
{
    if (_byBot.empty())
        return {};
    std::map<std::uint32_t, std::uint32_t> botsOfClass;
    for (auto const& [bot, damage] : _byBot)
        ++botsOfClass[_classOfBot.at(bot)];
    std::string out = " dmg_done_by_class=";
    bool first = true;
    for (auto const& [cls, damage] : _byClass)
    {
        out += (first ? "" : ",") + std::to_string(cls) + ":" + std::to_string(damage) + ":" + std::to_string(botsOfClass[cls]);
        first = false;
    }
    out += " dmg_done_by_bot=";
    first = true;
    for (auto const& [bot, damage] : _byBot)
    {
        out += (first ? "" : ",") + std::to_string(bot) + ":" + std::to_string(_classOfBot.at(bot)) + ":" + std::to_string(damage);
        first = false;
    }
    return out;
}

} // namespace damage_done
