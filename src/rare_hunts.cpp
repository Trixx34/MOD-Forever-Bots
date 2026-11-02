// SPDX-License-Identifier: GPL-2.0-or-later
#include "rare_hunts.h"

#include "text_number.h"
#include "text_split.h"

#include <algorithm>
#include <cmath>

namespace rare_hunts {

std::vector<Hunt> ParseHunts(std::string_view text, std::vector<std::string>& errors)
{
    std::vector<Hunt> hunts;
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
        std::uint32_t entry = 0, map = 0, minLevel = 0, maxLevel = 0;
        float x = 0, y = 0, z = 0, radius = 0;
        if (fields.size() != 9 || !text_number::ParseWhole(std::string(fields[0]), entry) || fields[1].empty() ||
            !text_number::ParseWhole(std::string(fields[2]), map) || !text_number::ParseDecimal(std::string(fields[3]), x) ||
            !text_number::ParseDecimal(std::string(fields[4]), y) || !text_number::ParseDecimal(std::string(fields[5]), z) ||
            !text_number::ParseWhole(std::string(fields[6]), minLevel) || !text_number::ParseWhole(std::string(fields[7]), maxLevel) ||
            minLevel > maxLevel || !text_number::ParseDecimal(std::string(fields[8]), radius) || radius <= 0.0f)
        {
            errors.push_back("line " + std::to_string(lineNo) + ": expected entry|name|map|x|y|z|minLevel|maxLevel|radius");
            continue;
        }
        Hunt hunt;
        hunt.Entry = entry;
        hunt.Name = std::string(fields[1]);
        hunt.Map = map;
        hunt.X = x;
        hunt.Y = y;
        hunt.Z = z;
        hunt.MinLevel = minLevel;
        hunt.MaxLevel = maxLevel;
        hunt.Radius = radius;
        hunts.push_back(std::move(hunt));
    }
    return hunts;
}

std::optional<std::size_t> ChooseHunt(std::vector<Hunt> const& hunts, std::uint32_t map, float x, float y, std::uint32_t level,
                                      std::vector<std::uint32_t> const& done)
{
    std::optional<std::size_t> best;
    float bestDistance = 0.0f;
    for (std::size_t i = 0; i < hunts.size(); ++i)
    {
        Hunt const& hunt = hunts[i];
        if (hunt.Map != map || level + kLevelSlack < hunt.MinLevel || level > hunt.MaxLevel + kLevelSlack)
            continue;
        if (std::find(done.begin(), done.end(), hunt.Entry) != done.end())
            continue;
        float const d = std::hypot(hunt.X - x, hunt.Y - y);
        if (!best || d < bestDistance)
        {
            best = i;
            bestDistance = d;
        }
    }
    return best;
}

std::vector<std::uint32_t> HuntEntries(std::vector<Hunt> const& hunts)
{
    std::vector<std::uint32_t> entries;
    for (Hunt const& hunt : hunts)
        if (std::find(entries.begin(), entries.end(), hunt.Entry) == entries.end())
            entries.push_back(hunt.Entry);
    return entries;
}

} // namespace rare_hunts
