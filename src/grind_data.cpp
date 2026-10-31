// SPDX-License-Identifier: GPL-2.0-or-later
#include "grind_data.h"

#include "text_lines.h"
#include "text_number.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace {

// Shared single-token parser (text_number.h); see combat_rotation.cpp for the rationale.
bool Number(std::string const& text, float& out)
{
    return text_number::ParseDecimal(text, out);
}

bool Whole(std::string const& text, std::uint32_t& out)
{
    return text_number::ParseWhole(text, out);
}

} // namespace

GrindData ParseGrindData(std::istream& in, std::vector<std::string>& warnings)
{
    GrindData data;
    std::size_t number = 0;
    std::vector<std::string> tokens;
    while (text_lines::NextTokens(in, number, tokens))
    {
        GrindSpot spot;
        std::uint32_t side = 0;
        bool ok = tokens[0] == "spot" && (tokens.size() == 8 || tokens.size() == 9);
        ok = ok && Whole(tokens[1], spot.Map) && Number(tokens[2], spot.X) && Number(tokens[3], spot.Y) && Number(tokens[4], spot.Z) &&
             Whole(tokens[5], spot.MinLevel) && Whole(tokens[6], spot.MaxLevel) && Whole(tokens[7], side) && side <= 2 &&
             (tokens.size() == 8 || Whole(tokens[8], spot.Weight)) && spot.MinLevel >= 1 && spot.MinLevel <= spot.MaxLevel && spot.MaxLevel <= 80;
        if (!ok)
        {
            warnings.push_back("grind spots line " + std::to_string(number) + ": not 'spot <map> <x> <y> <z> <minLevel> <maxLevel> <side> [weight]'; ignored");
            continue;
        }
        spot.Side = static_cast<std::uint8_t>(side);
        if (spot.Weight == 0)
            spot.Weight = 1;
        data.Spots.push_back(spot);
    }
    return data;
}

GrindData LoadGrindData(std::string const& path, std::vector<std::string>& warnings)
{
    std::ifstream in(path);
    if (!in)
    {
        warnings.push_back("grind spots file '" + path + "' cannot be read; no grind spots");
        return {};
    }
    return ParseGrindData(in, warnings);
}

std::vector<std::size_t> GrindSpotsFor(GrindData const& data, std::uint32_t map, std::uint32_t level, std::uint8_t side, float x, float y,
                                       std::vector<std::size_t> const& skip)
{
    auto const usable = [&](GrindSpot const& spot)
    {
        return spot.Map == map && (spot.Side == 0 || side == 0 || spot.Side == side);
    };

    std::vector<std::size_t> candidates;
    for (std::size_t i = 0; i < data.Spots.size(); ++i)
        if (usable(data.Spots[i]) && data.Spots[i].MinLevel <= level && level <= data.Spots[i].MaxLevel)
            candidates.push_back(i);

    if (candidates.empty())
    {
        // Over-leveled for everything on the map: the spots with the highest top level below the bot's.
        std::uint32_t best = 0;
        for (std::size_t i = 0; i < data.Spots.size(); ++i)
            if (usable(data.Spots[i]) && data.Spots[i].MaxLevel < level)
                best = std::max(best, data.Spots[i].MaxLevel);
        if (best != 0)
            for (std::size_t i = 0; i < data.Spots.size(); ++i)
                if (usable(data.Spots[i]) && data.Spots[i].MaxLevel == best)
                    candidates.push_back(i);
    }

    if (candidates.size() > skip.size())
    {
        std::vector<std::size_t> fresh;
        for (std::size_t index : candidates)
            if (std::find(skip.begin(), skip.end(), index) == skip.end())
                fresh.push_back(index);
        if (!fresh.empty())
            candidates = std::move(fresh);
    }

    auto const distance = [&](std::size_t index)
    {
        float const dx = data.Spots[index].X - x;
        float const dy = data.Spots[index].Y - y;
        return std::sqrt(dx * dx + dy * dy);
    };
    // Richer spots count as a bit closer: a spot made from many creatures is worth a longer walk.
    auto const score = [&](std::size_t index)
    {
        return distance(index) / (1.0f + 0.15f * std::log1p(float(data.Spots[index].Weight)));
    };
    std::sort(candidates.begin(), candidates.end(), [&](std::size_t a, std::size_t b) { return score(a) < score(b); });
    return candidates;
}

std::uint8_t SideOfRace(std::uint32_t race)
{
    switch (race)
    {
        case 1: case 3: case 4: case 7: case 11: case 22: case 25: case 29: case 30: case 32: // human dwarf night elf gnome draenei worgen ...
            return 1;
        case 2: case 5: case 6: case 8: case 9: case 10: case 26: case 27: case 28: case 31: case 36:  // orc undead tauren troll goblin blood elf ...
            return 2;
        default:
            return 0;
    }
}
