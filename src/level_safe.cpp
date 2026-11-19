// SPDX-License-Identifier: GPL-2.0-or-later
#include "level_safe.h"

#include "grind_data.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace level_safe
{

LevelMap::CellKey LevelMap::KeyOf(std::uint32_t map, float x, float y) const
{
    return CellKey{ map, int(std::floor(x / _radius)), int(std::floor(y / _radius)) };
}

void LevelMap::Build(GrindData const& data, float radiusYards)
{
    _radius = std::max(radiusYards, 10.0f);
    _cells.clear();
    for (GrindSpot const& spot : data.Spots)
        if (spot.MinLevel != 0)
            _cells[KeyOf(spot.Map, spot.X, spot.Y)].push_back(Entry{ spot.X, spot.Y, spot.MinLevel });
}

std::uint32_t LevelMap::PlaceMin(std::uint32_t map, float x, float y) const
{
    if (_cells.empty())
        return 0;
    auto const [m, cx, cy] = KeyOf(map, x, y);
    float const radiusSq = _radius * _radius;
    std::uint32_t best = 0;
    std::size_t count = 0;
    // The cell is as wide as the radius: the spots within it lie in the 3 x 3 cells around the place.
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
        {
            auto const cell = _cells.find(CellKey{ m, cx + dx, cy + dy });
            if (cell == _cells.end())
                continue;
            for (Entry const& entry : cell->second)
            {
                float const ex = entry.X - x;
                float const ey = entry.Y - y;
                if (ex * ex + ey * ey > radiusSq)
                    continue;
                ++count;
                if (best == 0 || entry.MinLevel < best)
                    best = entry.MinLevel;
            }
        }
    return count >= kMinSpots ? best : 0;
}

bool LevelMap::TooHigh(std::uint32_t map, float x, float y, std::uint32_t level, std::uint32_t margin) const
{
    if (level == 0)
        return false;
    std::uint32_t const place = PlaceMin(map, x, y);
    return place != 0 && place > level + margin;
}

bool LevelMap::Fits(std::uint32_t map, float x, float y, std::uint32_t level, std::uint32_t margin) const
{
    std::uint32_t const place = PlaceMin(map, x, y);
    return place != 0 && level != 0 && place <= level + margin;
}

std::optional<LevelMap::Hit> LevelMap::FirstTooHigh(std::uint32_t map, float fromX, float fromY, float toX, float toY, std::uint32_t level, std::uint32_t margin,
                                                    float step, float skipNear) const
{
    if (level == 0 || _cells.empty())
        return std::nullopt;
    float const dx = toX - fromX;
    float const dy = toY - fromY;
    float const length = std::hypot(dx, dy);
    step = std::max(step, 10.0f);
    auto const check = [&](float x, float y) -> std::optional<Hit>
    {
        std::uint32_t const place = PlaceMin(map, x, y);
        if (place != 0 && place > level + margin)
            return Hit{ x, y, place };
        return std::nullopt;
    };
    // Along the way, from skipNear on; the end is always checked (even a short walk must not end in a high place).
    for (float along = skipNear; along < length; along += step)
        if (auto const hit = check(fromX + dx * along / length, fromY + dy * along / length))
            return hit;
    return check(toX, toY);
}

void Graveyards::Note(std::uint32_t map, float x, float y, float z, std::uint8_t side)
{
    std::vector<Yard>& yards = _maps[map];
    std::uint8_t const bit = side == 1 ? 1 : side == 2 ? 2 : 0;
    for (Yard& yard : yards)
        if (std::hypot(yard.X - x, yard.Y - y) <= kSameYards)
        {
            yard.Sides |= bit;
            return;
        }
    if (yards.size() >= kMaxPerMap)
        return;
    yards.push_back(Yard{ map, x, y, z, bit });
}

Graveyards::Yard const* Graveyards::Fitting(LevelMap const& levels, std::uint32_t map, float x, float y, std::uint32_t level, std::uint32_t margin,
                                            std::uint8_t side, std::function<bool(Yard const&)> const& skip) const
{
    auto const found = _maps.find(map);
    if (found == _maps.end())
        return nullptr;
    std::uint8_t const bit = side == 1 ? 1 : side == 2 ? 2 : 0;
    Yard const* best = nullptr;
    float bestDist = 0.0f;
    for (Yard const& yard : found->second)
    {
        if (bit != 0 && (yard.Sides & bit) == 0)
            continue;
        if (!levels.Fits(map, yard.X, yard.Y, level, margin))
            continue;
        if (skip && skip(yard))
            continue;
        float const dist = std::hypot(yard.X - x, yard.Y - y);
        if (!best || dist < bestDist)
        {
            best = &yard;
            bestDist = dist;
        }
    }
    return best;
}

Graveyards::Yard const* Graveyards::Nearest(std::uint32_t map, float x, float y, std::uint8_t side, std::function<bool(Yard const&)> const& skip) const
{
    auto const found = _maps.find(map);
    if (found == _maps.end())
        return nullptr;
    std::uint8_t const bit = side == 1 ? 1 : side == 2 ? 2 : 0;
    Yard const* best = nullptr;
    float bestDist = 0.0f;
    for (Yard const& yard : found->second)
    {
        if ((bit != 0 && (yard.Sides & bit) == 0) || (skip && skip(yard)))
            continue;
        float const dist = std::hypot(yard.X - x, yard.Y - y);
        if (!best || dist < bestDist)
        {
            best = &yard;
            bestDist = dist;
        }
    }
    return best;
}

Graveyards::Yard const* Graveyards::Nearest(std::uint32_t map, float x, float y) const
{
    auto const found = _maps.find(map);
    if (found == _maps.end())
        return nullptr;
    Yard const* best = nullptr;
    float bestDist = 0.0f;
    for (Yard const& yard : found->second)
    {
        float const dist = std::hypot(yard.X - x, yard.Y - y);
        if (!best || dist < bestDist)
        {
            best = &yard;
            bestDist = dist;
        }
    }
    return best;
}

std::size_t Graveyards::Size() const
{
    std::size_t total = 0;
    for (auto const& [map, yards] : _maps)
        total += yards.size();
    return total;
}

std::string DeathTally::TakeLine(std::size_t online, std::uint64_t windowMs)
{
    std::vector<std::pair<std::uint32_t, std::uint64_t>> perBot; // (deaths, bot)
    perBot.reserve(_deaths.size());
    for (auto const& [bot, deaths] : _deaths)
        perBot.emplace_back(deaths, bot);
    std::sort(perBot.begin(), perBot.end(), [](auto const& a, auto const& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
    std::size_t const died = perBot.size();
    std::size_t const bots = std::max(online, died);
    // The median of the per-bot counts, the bots that did not die counted as 0. perBot is sorted high to low; position i of the low-to-high order is
    // perBot[bots - 1 - i] when that lies within perBot, else 0.
    auto const valueAt = [&](std::size_t lowToHigh) -> double
    {
        std::size_t const fromTop = bots - 1 - lowToHigh;
        return fromTop < died ? double(perBot[fromTop].first) : 0.0;
    };
    auto const median = [&](std::size_t count, auto const& at) -> double
    {
        if (count == 0)
            return 0.0;
        return count % 2 == 1 ? at(count / 2) : (at(count / 2 - 1) + at(count / 2)) / 2.0;
    };
    double const medianAll = median(bots, valueAt);
    double const medianDied = median(died, [&](std::size_t lowToHigh) { return double(perBot[died - 1 - lowToHigh].first); });
    std::uint64_t top10 = 0;
    for (std::size_t i = 0; i < died && i < 10; ++i)
        top10 += perBot[i].first;
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "deaths_top: window_min=%llu deaths=%llu bots_online=%zu bots_died=%zu median=%.1f median_died=%.1f top10=%llu top10_pct=%.1f top=",
                  static_cast<unsigned long long>(windowMs / 60000), static_cast<unsigned long long>(_total), online, died, medianAll, medianDied,
                  static_cast<unsigned long long>(top10), _total == 0 ? 0.0 : 100.0 * double(top10) / double(_total));
    std::string line = buffer;
    for (std::size_t i = 0; i < died && i < 10; ++i)
        line += (i == 0 ? "" : ",") + std::to_string(perBot[i].second) + ":" + std::to_string(perBot[i].first);
    if (died == 0)
        line += "none";
    _deaths.clear();
    _total = 0;
    return line;
}

bool LogThrottle::Due(std::uint64_t bot, std::uint32_t quest, std::string const& reason, std::uint64_t nowMs, std::uint64_t periodMs)
{
    if (periodMs == 0)
    {
        ++_written;
        return true;
    }
    if (_last.size() >= kPruneAt)
        std::erase_if(_last, [&](auto const& entry) { return nowMs - entry.second >= periodMs; });
    auto const key = std::make_tuple(bot, quest, reason);
    auto const found = _last.find(key);
    if (found != _last.end() && nowMs - found->second < periodMs)
    {
        ++_held;
        return false;
    }
    _last[key] = nowMs;
    ++_written;
    return true;
}

std::string LogThrottle::TakeFields()
{
    std::string const fields = " level_safe_lines=" + std::to_string(_written) + " level_safe_held=" + std::to_string(_held);
    _written = 0;
    _held = 0;
    return fields;
}

} // namespace level_safe
