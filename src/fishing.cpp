// SPDX-License-Identifier: GPL-2.0-or-later
#include "fishing.h"

#include "text_number.h"
#include "text_split.h"

#include <cmath>

namespace fishing {

namespace {

constexpr std::uint32_t kMinFishingLevel = 5;
constexpr std::uint32_t kSkillCapPerLevel = 5;

} // namespace

std::vector<Spot> ParseSpots(std::string_view text, std::vector<std::string>& errors)
{
    std::vector<Spot> spots;
    std::size_t lineNo = 0;
    std::size_t start = 0;
    while (start <= text.size())
    {
        std::size_t const end = text.find('\n', start);
        std::string_view line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        start = end == std::string_view::npos ? text.size() + 1 : end + 1;
        ++lineNo;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.remove_suffix(1);
        if (line.empty() || line.front() == '#')
            continue;

        auto const fields = text_split::Split(line, '|');
        std::uint32_t map = 0;
        float x = 0, y = 0, z = 0, radius = 0;
        if (fields.size() != 6 || !text_number::ParseWhole(std::string(fields[0]), map) || !text_number::ParseDecimal(std::string(fields[1]), x) ||
            !text_number::ParseDecimal(std::string(fields[2]), y) || !text_number::ParseDecimal(std::string(fields[3]), z) ||
            !text_number::ParseDecimal(std::string(fields[4]), radius) || radius <= 0.0f || fields[5].empty())
        {
            errors.push_back("line " + std::to_string(lineNo) + ": expected map|x|y|z|radius|name");
            continue;
        }
        Spot spot;
        spot.Map = map;
        spot.X = x;
        spot.Y = y;
        spot.Z = z;
        spot.Radius = radius;
        spot.Name = std::string(fields[5]);
        spots.push_back(std::move(spot));
    }
    return spots;
}

std::optional<std::size_t> NearestSpot(std::vector<Spot> const& spots, std::uint32_t map, float x, float y)
{
    std::optional<std::size_t> best;
    float bestDistance = 0.0f;
    for (std::size_t i = 0; i < spots.size(); ++i)
    {
        if (spots[i].Map != map)
            continue;
        float const d = std::hypot(spots[i].X - x, spots[i].Y - y);
        if (!best || d < bestDistance)
        {
            best = i;
            bestDistance = d;
        }
    }
    return best;
}

bool FishingDue(std::uint32_t level, std::uint32_t fishingSkill, bool hasPole)
{
    return hasPole && level >= kMinFishingLevel && fishingSkill < level * kSkillCapPerLevel;
}

} // namespace fishing
