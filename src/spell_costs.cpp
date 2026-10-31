// SPDX-License-Identifier: GPL-2.0-or-later
#include "spell_costs.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace spell_cost
{

namespace
{

constexpr int kMaxRankSteps = 40; // a rank chain is a dozen long at most; the limit only guards a file with a loop in it

bool ToUnsigned(std::string const& text, std::uint32_t& out)
{
    char* end = nullptr;
    unsigned long const value = std::strtoul(text.c_str(), &end, 10);
    if (text.empty() || end == text.c_str() || *end != '\0')
        return false;
    out = static_cast<std::uint32_t>(value);
    return true;
}

bool ToSigned(std::string const& text, std::int32_t& out)
{
    char* end = nullptr;
    long const value = std::strtol(text.c_str(), &end, 10);
    if (text.empty() || end == text.c_str() || *end != '\0')
        return false;
    out = static_cast<std::int32_t>(value);
    return true;
}

} // namespace

SpellCostData ParseSpellCosts(std::istream& in, std::vector<std::string>& warnings)
{
    SpellCostData data;
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(in, line))
    {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;

        std::vector<std::string> fields;
        std::istringstream split(line);
        for (std::string field; std::getline(split, field, '\t');)
            fields.push_back(field);
        // The name (the last field) is only for people; the seven numbers are what is read.
        if (fields.size() < 7)
        {
            warnings.push_back("spell costs line " + std::to_string(lineNumber) + ": expected 7 or more tab separated fields");
            continue;
        }

        std::uint32_t id = 0, next = 0;
        Entry entry;
        char* end = nullptr;
        entry.Percent = std::strtof(fields[5].c_str(), &end);
        if (!ToUnsigned(fields[0], id) || id == 0 || !ToUnsigned(fields[1], entry.Level) || !ToSigned(fields[2], entry.PowerType) ||
            !ToSigned(fields[3], entry.Cost) || !ToSigned(fields[4], entry.PerLevel) || end == fields[5].c_str() || !ToUnsigned(fields[6], next))
        {
            warnings.push_back("spell costs line " + std::to_string(lineNumber) + ": a number is not valid");
            continue;
        }
        entry.NextRank = next;
        // A spell can have more than one power line; the first (the main resource) is the one kept.
        data.BySpell.try_emplace(id, entry);
    }
    return data;
}

SpellCostData LoadSpellCosts(std::string const& path, std::vector<std::string>& warnings)
{
    std::ifstream in(path);
    if (!in)
    {
        warnings.push_back("cannot read the spell costs from " + path + " (bots will try casts without checking the cost)");
        return {};
    }
    return ParseSpellCosts(in, warnings);
}

std::optional<Payment> SpellCostData::CostAtLevel(std::uint32_t firstRank, std::uint32_t level) const
{
    auto const first = BySpell.find(firstRank);
    if (first == BySpell.end())
        return std::nullopt;

    Entry best = first->second;
    std::uint32_t id = best.NextRank;
    for (int step = 0; id != 0 && step < kMaxRankSteps; ++step)
    {
        auto const next = BySpell.find(id);
        if (next == BySpell.end() || (level != 0 && next->second.Level > level))
            break;
        best = next->second;
        id = best.NextRank;
    }

    if (best.PerLevel != 0 || best.Percent != 0.0f || best.Cost <= 0)
        return std::nullopt; // not a flat cost: unknown, so the bot just tries it
    return Payment{ best.PowerType, static_cast<std::uint32_t>(best.Cost) };
}

} // namespace spell_cost
