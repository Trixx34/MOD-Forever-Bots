// SPDX-License-Identifier: GPL-2.0-or-later
#include "tame_beasts.h"

#include "text_lines.h"
#include "text_number.h"

#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>

using forever_module::BotSnapshot;
using forever_module::NearbyUnit;

namespace tame {

namespace {

// Shared single-token parser (text_number.h); see combat_rotation.cpp for the rationale.
bool Whole(std::string const& text, std::uint32_t& out)
{
    return text_number::ParseWhole(text, out);
}

// Lower is liked more: cats, wolves, boars, bears, then everything else.
std::uint32_t FamilyRank(std::uint32_t family)
{
    switch (family)
    {
        case 2: return 0;   // cat
        case 1: return 1;   // wolf
        case 5: return 2;   // boar
        case 4: return 3;   // bear
        default: return 4;
    }
}

} // namespace

TameBeasts ParseTameBeasts(std::istream& in, std::vector<std::string>& warnings)
{
    TameBeasts data;
    std::size_t number = 0;
    std::vector<std::string> t;
    while (text_lines::NextTokens(in, number, t))
    {
        Beast beast;
        std::uint32_t entry = 0;
        if (t[0] == "beast" && t.size() == 5 && Whole(t[1], entry) && entry != 0 && Whole(t[2], beast.Family) && Whole(t[3], beast.LevelMin) &&
            Whole(t[4], beast.LevelMax) && beast.LevelMin <= beast.LevelMax)
            data.ByEntry[entry] = beast;
        else
            warnings.push_back("tame beasts line " + std::to_string(number) + ": not 'beast <entry> <family> <level min> <level max>'; ignored");
    }
    return data;
}

TameBeasts LoadTameBeasts(std::string const& path, std::vector<std::string>& warnings)
{
    std::ifstream in(path);
    if (!in)
    {
        warnings.push_back("tame beasts file '" + path + "' cannot be read; hunters do not tame");
        return {};
    }
    return ParseTameBeasts(in, warnings);
}

NearbyUnit const* PickTameTarget(BotSnapshot const& bot, std::span<NearbyUnit const> nearby, TameBeasts const& beasts, float maxYards)
{
    NearbyUnit const* best = nullptr;
    std::uint64_t bestScore = std::numeric_limits<std::uint64_t>::max();
    for (NearbyUnit const& unit : nearby)
    {
        if (unit.health == 0 || forever_module::UnitRefType(unit.guid) != 8)
            continue; // a creature that is alive
        auto const known = beasts.ByEntry.find(forever_module::UnitRefEntry(unit.guid));
        if (known == beasts.ByEntry.end())
            continue;
        std::uint32_t const level = unit.level != 0 ? unit.level : known->second.LevelMax;
        if (bot.level != 0 && level > bot.level)
            continue; // the core refuses a beast above the hunter's level
        float const dx = unit.pos_x - bot.pos_x;
        float const dy = unit.pos_y - bot.pos_y;
        float const dist = std::sqrt(dx * dx + dy * dy);
        if (dist > maxYards)
            continue;
        std::uint64_t const gap = bot.level > level ? bot.level - level : 0;
        std::uint64_t const score = (gap << 40) | (std::uint64_t(unit.is_hostile ? 1 : 0) << 36) | (std::uint64_t(FamilyRank(known->second.Family)) << 32) |
                                    std::uint64_t(dist * 10.0f);
        if (score < bestScore)
        {
            bestScore = score;
            best = &unit;
        }
    }
    return best;
}

} // namespace tame
