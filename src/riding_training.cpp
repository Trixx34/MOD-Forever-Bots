// SPDX-License-Identifier: GPL-2.0-or-later
#include "riding_training.h"

#include "text_number.h"
#include "text_split.h"

namespace riding {

std::vector<Rank> ParseRanks(std::string_view text, std::vector<std::string>& errors)
{
    std::vector<Rank> ranks;
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
        std::uint32_t spell = 0, minLevel = 0, skill = 0, copper = 0;
        if (fields.size() != 5 || !text_number::ParseWhole(std::string(fields[0]), spell) ||
            !text_number::ParseWhole(std::string(fields[1]), minLevel) || !text_number::ParseWhole(std::string(fields[2]), skill) ||
            !text_number::ParseWhole(std::string(fields[3]), copper) || fields[4].empty())
        {
            errors.push_back("line " + std::to_string(lineNo) + ": expected spell|minLevel|skill|copper|name");
            continue;
        }
        Rank rank;
        rank.Spell = spell;
        rank.MinLevel = minLevel;
        rank.Skill = skill;
        rank.CostCopper = copper;
        rank.Name = std::string(fields[4]);
        ranks.push_back(std::move(rank));
    }
    return ranks;
}

std::optional<std::size_t> NextRank(std::vector<Rank> const& ranks, std::uint32_t level, std::uint32_t ridingSkill, std::uint64_t copper)
{
    for (std::size_t i = 0; i < ranks.size(); ++i)
    {
        Rank const& rank = ranks[i];
        if (level >= rank.MinLevel && ridingSkill < rank.Skill && copper >= rank.CostCopper)
            return i;
    }
    return std::nullopt;
}

} // namespace riding
