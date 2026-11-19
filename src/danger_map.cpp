// SPDX-License-Identifier: GPL-2.0-or-later
// The shared danger map (danger_map.h).
#include "danger_map.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

namespace danger_map
{

std::uint32_t BandOf(std::uint32_t level)
{
    return level == 0 ? 0u : (level - 1) / kBandLevels;
}

std::string BandName(std::uint32_t band)
{
    return std::to_string(band * kBandLevels + 1) + "-" + std::to_string((band + 1) * kBandLevels);
}

std::size_t Map::KeyHash::operator()(Key const& key) const
{
    std::size_t h = std::hash<std::uint32_t>{}(std::get<0>(key));
    h = h * 1000003u ^ std::hash<int>{}(std::get<1>(key));
    h = h * 1000003u ^ std::hash<int>{}(std::get<2>(key));
    return h;
}

Map::Key Map::KeyOf(std::uint32_t map, float x, float y)
{
    return Key{ map, int(std::floor(x / kCellYards)), int(std::floor(y / kCellYards)) };
}

float Map::CellRate(Cell const& cell, std::uint32_t band, std::uint64_t nowMs) const
{
    if (_options.DecayMs == 0)
        return 0.0f;
    double weight = 0.0;
    for (Death const& death : cell.Deaths)
    {
        if (death.Band < band || nowMs < death.AtMs)
            continue;
        std::uint64_t const age = nowMs - death.AtMs;
        if (age >= _options.DecayMs)
            continue;
        weight += 1.0 - double(age) / double(_options.DecayMs);
    }
    // A steady rate r over the decay window adds up to r * hours / 2.
    double const halfHours = double(_options.DecayMs) / 3600000.0 / 2.0;
    return float(weight / halfHours);
}

std::optional<HotCell> Map::NoteDeath(std::uint32_t map, float x, float y, std::uint32_t level, std::uint32_t killerEntry, std::uint64_t nowMs)
{
    Key const key = KeyOf(map, x, y);
    if (_cells.find(key) == _cells.end() && _cells.size() >= kMaxCells)
        Prune(nowMs);
    if (_cells.find(key) == _cells.end() && _cells.size() >= kMaxCells)
    {
        // Still full: the cell whose last death is oldest goes.
        auto oldest = _cells.begin();
        for (auto it = _cells.begin(); it != _cells.end(); ++it)
            if (it->second.LastMs < oldest->second.LastMs)
                oldest = it;
        _cells.erase(oldest);
    }
    Cell& cell = _cells[key];
    std::uint32_t const band = BandOf(level);
    float const before = CellRate(cell, band, nowMs);
    cell.Deaths.push_back(Death{ nowMs, killerEntry, band });
    if (cell.Deaths.size() > kDeathsPerCell)
        cell.Deaths.erase(cell.Deaths.begin());
    cell.LastMs = nowMs;
    ++_deathsNoted;
    float const after = CellRate(cell, band, nowMs);
    float const limit = float(_options.DeathsPerHour);
    if (_options.DeathsPerHour == 0 || after < limit || before >= limit)
        return std::nullopt;
    auto const logged = cell.HotLoggedMs.find(band);
    if (logged != cell.HotLoggedMs.end() && nowMs - logged->second < kHotLogEveryMs)
        return std::nullopt;
    cell.HotLoggedMs[band] = nowMs;
    HotCell hot;
    hot.Map = map;
    hot.X = (float(std::get<1>(key)) + 0.5f) * kCellYards;
    hot.Y = (float(std::get<2>(key)) + 0.5f) * kCellYards;
    hot.Band = band;
    hot.Rate = after;
    hot.Deaths = std::uint32_t(cell.Deaths.size());
    std::map<std::uint32_t, std::uint32_t> byEntry;
    for (Death const& death : cell.Deaths)
        ++byEntry[death.Entry];
    std::uint32_t best = 0;
    for (auto const& [entry, count] : byEntry)
        if (entry != 0 && count > best)
        {
            best = count;
            hot.TopEntry = entry;
        }
    hot.TopShare = hot.Deaths == 0 ? 0 : best * 100 / hot.Deaths;
    return hot;
}

float Map::Rate(std::uint32_t map, float x, float y, std::uint32_t level, std::uint64_t nowMs) const
{
    if (_cells.empty())
        return 0.0f;
    Key const centre = KeyOf(map, x, y);
    std::uint32_t const band = BandOf(level);
    float best = 0.0f;
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
        {
            auto const found = _cells.find(Key{ map, std::get<1>(centre) + dx, std::get<2>(centre) + dy });
            if (found != _cells.end())
                best = std::max(best, CellRate(found->second, band, nowMs));
        }
    return best;
}

bool Map::Dangerous(std::uint32_t map, float x, float y, std::uint32_t level, std::uint64_t nowMs) const
{
    if (level == 0 || _options.DeathsPerHour == 0)
        return false;
    return Rate(map, x, y, level, nowMs) >= float(_options.DeathsPerHour);
}

std::optional<Map::Hit> Map::FirstDangerous(std::uint32_t map, float fromX, float fromY, float toX, float toY, std::uint32_t level, std::uint64_t nowMs,
                                            float skipNear) const
{
    if (_cells.empty() || level == 0)
        return std::nullopt;
    float const length = std::hypot(toX - fromX, toY - fromY);
    auto const judge = [&](float along) -> std::optional<Hit>
    {
        float const t = length > 0.0f ? along / length : 1.0f;
        float const x = fromX + (toX - fromX) * t;
        float const y = fromY + (toY - fromY) * t;
        float const rate = Rate(map, x, y, level, nowMs);
        if (_options.DeathsPerHour != 0 && rate >= float(_options.DeathsPerHour))
            return Hit{ x, y, rate };
        return std::nullopt;
    };
    for (float along = skipNear; along < length; along += kCellYards)
        if (std::optional<Hit> const hit = judge(along))
            return hit;
    if (length >= skipNear)
        return judge(length);
    return std::nullopt;
}

std::uint32_t Map::TopKiller(std::uint32_t map, float x, float y) const
{
    Key const centre = KeyOf(map, x, y);
    std::map<std::uint32_t, std::uint32_t> byEntry;
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
        {
            auto const found = _cells.find(Key{ map, std::get<1>(centre) + dx, std::get<2>(centre) + dy });
            if (found == _cells.end())
                continue;
            for (Death const& death : found->second.Deaths)
                if (death.Entry != 0)
                    ++byEntry[death.Entry];
        }
    std::uint32_t top = 0;
    std::uint32_t best = 0;
    for (auto const& [entry, count] : byEntry)
        if (count > best)
        {
            best = count;
            top = entry;
        }
    return top;
}

void Map::Prune(std::uint64_t nowMs)
{
    for (auto it = _cells.begin(); it != _cells.end();)
    {
        auto& deaths = it->second.Deaths;
        std::erase_if(deaths, [&](Death const& death) { return nowMs >= death.AtMs && nowMs - death.AtMs >= _options.DecayMs; });
        it = deaths.empty() ? _cells.erase(it) : std::next(it);
    }
}

std::string Map::TakeFields(std::uint64_t nowMs)
{
    Prune(nowMs);
    struct Top
    {
        Key K;
        float Rate;
    };
    std::vector<Top> tops;
    std::size_t hot = 0;
    for (auto const& [key, cell] : _cells)
    {
        float const rate = CellRate(cell, 0, nowMs);
        if (_options.DeathsPerHour != 0 && rate >= float(_options.DeathsPerHour))
            ++hot;
        tops.push_back(Top{ key, rate });
    }
    std::sort(tops.begin(), tops.end(), [](Top const& a, Top const& b) { return a.Rate > b.Rate; });
    std::string line = " danger_cells=" + std::to_string(_cells.size()) + " danger_hot=" + std::to_string(hot) + " danger_deaths=" + std::to_string(_deathsNoted) +
                       " danger_skips=";
    if (_skips.empty())
        line += "none";
    bool first = true;
    for (auto const& [what, count] : _skips)
    {
        line += (first ? "" : ",") + what + ":" + std::to_string(count);
        first = false;
    }
    line += " danger_top=";
    if (tops.empty())
        line += "none";
    for (std::size_t i = 0; i < tops.size() && i < 3; ++i)
    {
        auto const& [map, cx, cy] = tops[i].K;
        float const x = (float(cx) + 0.5f) * kCellYards;
        float const y = (float(cy) + 0.5f) * kCellYards;
        char rate[32];
        std::snprintf(rate, sizeof(rate), "%.1f", tops[i].Rate);
        line += (i == 0 ? "" : ",") + std::to_string(map) + ":" + std::to_string(int(x)) + "," + std::to_string(int(y)) + ":" + std::to_string(TopKiller(map, x, y)) +
                ":" + rate;
    }
    _skips.clear();
    _deathsNoted = 0;
    return line;
}

} // namespace danger_map
