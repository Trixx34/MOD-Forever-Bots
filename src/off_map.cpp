// SPDX-License-Identifier: GPL-2.0-or-later
#include "off_map.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace off_map
{

void KnownWorld::Add(Place const& place)
{
    MapPlaces& entry = _maps[place.Map];
    if (entry.Places.empty())
    {
        entry.MinX = entry.MaxX = place.X;
        entry.MinY = entry.MaxY = place.Y;
    }
    else
    {
        entry.MinX = std::min(entry.MinX, place.X);
        entry.MaxX = std::max(entry.MaxX, place.X);
        entry.MinY = std::min(entry.MinY, place.Y);
        entry.MaxY = std::max(entry.MaxY, place.Y);
    }
    entry.Places.push_back(place);
}

std::size_t KnownWorld::PlacesOn(std::uint32_t map) const
{
    auto const it = _maps.find(map);
    return it == _maps.end() ? 0 : it->second.Places.size();
}

bool KnownWorld::Knows(std::uint32_t map) const
{
    return PlacesOn(map) >= kMinPlaces;
}

bool KnownWorld::Outside(std::uint32_t map, float x, float y, float margin) const
{
    if (!Knows(map))
        return false;
    MapPlaces const& entry = _maps.at(map);
    return x < entry.MinX - margin || x > entry.MaxX + margin || y < entry.MinY - margin || y > entry.MaxY + margin;
}

Place const* KnownWorld::Home(std::uint32_t map, float x, float y, std::uint8_t side) const
{
    auto const it = _maps.find(map);
    if (it == _maps.end())
        return nullptr;
    Place const* best = nullptr;
    float bestSq = std::numeric_limits<float>::max();
    for (Place const& place : it->second.Places)
    {
        if (side != 0 && place.Side != 0 && place.Side != side)
            continue;
        float const dx = place.X - x;
        float const dy = place.Y - y;
        float const sq = dx * dx + dy * dy;
        if (sq < bestSq)
        {
            bestSq = sq;
            best = &place;
        }
    }
    return best;
}

KnownWorld::Around KnownWorld::Near(std::uint32_t map, float x, float y, float z, float radius) const
{
    Around around;
    auto const it = _maps.find(map);
    if (it == _maps.end())
        return around;
    float const radiusSq = radius * radius;
    for (Place const& place : it->second.Places)
    {
        float const dx = place.X - x;
        float const dy = place.Y - y;
        if (dx * dx + dy * dy > radiusSq)
            continue;
        if (!around.Lowest || place.Z < around.Lowest->Z)
            around.Lowest = &place;
        around.NearestDz = std::min(around.NearestDz, std::fabs(place.Z - z));
    }
    return around;
}

Verdict Check(Options const& options, KnownWorld const& world, Watch& watch, Facts const& facts, std::uint64_t nowMs)
{
    if (facts.Travelling)
    {
        watch = Watch{};
        return Verdict::kTravelling;
    }
    // A dead bot's position is its corpse's or the graveyard's; the death code moves it. Its watch is kept: once alive and still off, the time counts on.
    if (facts.Alive && !world.Outside(facts.Map, facts.X, facts.Y, options.MarginYards))
    {
        watch = Watch{};
        return Verdict::kOnMap;
    }
    if (!facts.Alive && watch.OffSinceMs == 0)
        return Verdict::kOnMap;
    if (watch.OffSinceMs == 0)
        watch.OffSinceMs = nowMs == 0 ? 1 : nowMs;
    if (nowMs - watch.OffSinceMs < options.OffForMs)
        return Verdict::kWaiting;
    if (!facts.Alive || facts.InCombat || facts.Held)
        return Verdict::kBlocked;
    if (nowMs < watch.NextTryMs)
        return Verdict::kCooldown;
    watch.NextTryMs = nowMs + options.CooldownMs;
    ++watch.Tries;
    return Verdict::kSend;
}

char const* VerdictName(Verdict verdict)
{
    switch (verdict)
    {
        case Verdict::kOnMap: return "on_map";
        case Verdict::kTravelling: return "travelling";
        case Verdict::kWaiting: return "waiting";
        case Verdict::kBlocked: return "blocked";
        case Verdict::kCooldown: return "cooldown";
        case Verdict::kSend: return "send";
    }
    return "unknown";
}

} // namespace off_map
