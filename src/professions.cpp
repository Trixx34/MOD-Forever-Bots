// SPDX-License-Identifier: GPL-2.0-or-later
#include "professions.h"

#include "gather_nodes.h"
#include "text_split.h"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace professions
{

std::uint32_t SkillCap(std::uint32_t level, std::uint32_t perLevel, std::uint32_t professionMax)
{
    std::uint32_t cap = perLevel == 0 ? 0xFFFFFFFFu : level * perLevel;
    if (professionMax != 0)
        cap = std::min(cap, professionMax);
    return cap;
}

bool BelowCap(std::uint32_t value, std::uint32_t level, std::uint32_t perLevel, std::uint32_t professionMax)
{
    if (level == 0)
        return true;
    return value < SkillCap(level, perLevel, professionMax);
}

std::vector<SkillUp> SkillUps(std::uint16_t const prevSkill[2], std::uint16_t const prevValue[2], std::uint16_t const skill[2], std::uint16_t const value[2])
{
    std::vector<SkillUp> ups;
    for (int slot = 0; slot < 2; ++slot)
        if (skill[slot] != 0 && skill[slot] == prevSkill[slot] && prevValue[slot] != 0 && value[slot] > prevValue[slot])
            ups.push_back(SkillUp{ skill[slot], prevValue[slot], value[slot] });
    return ups;
}

std::string SkillUpLine(std::uint64_t bot, SkillUp const& up)
{
    return "profession_skillup: bot=" + std::to_string(bot) + " skill=" + std::to_string(up.Skill) + " from=" + std::to_string(up.From) + " to=" +
           std::to_string(up.To);
}

std::vector<Recipe> ParseRecipeTable(std::string_view text, std::vector<std::string>& errors)
{
    std::vector<Recipe> recipes;
    std::size_t lineNumber = 0;
    for (std::string_view line : text_split::Split(text, '\n'))
    {
        ++lineNumber;
        if (auto const hash = line.find('#'); hash != std::string_view::npos)
            line = line.substr(0, hash);
        line = text_split::TrimBlanks(line);
        if (line.empty())
            continue;
        auto const fail = [&](char const* why) { errors.push_back("line " + std::to_string(lineNumber) + ": " + why); };
        std::vector<std::string_view> fields;
        for (std::string_view field : text_split::Split(line, ' '))
            if (!field.empty())
                fields.push_back(field);
        if (fields.size() != 6)
        {
            fail("expected skill spell minSkill trivialLow trivialHigh reagent:count,...");
            continue;
        }
        Recipe recipe;
        if (!text_split::ParseUint32(fields[0], recipe.Skill) || !text_split::ParseUint32(fields[1], recipe.Spell) ||
            !text_split::ParseUint32(fields[2], recipe.MinSkill) || !text_split::ParseUint32(fields[3], recipe.TrivialLow) ||
            !text_split::ParseUint32(fields[4], recipe.TrivialHigh) || recipe.Skill == 0 || recipe.Spell == 0 || recipe.TrivialHigh == 0)
        {
            fail("bad skill, spell or skill numbers");
            continue;
        }
        bool ok = true;
        for (std::string_view pair : text_split::Split(fields[5], ','))
        {
            auto const colon = pair.find(':');
            std::uint32_t item = 0, count = 0;
            if (colon == std::string_view::npos || !text_split::ParseUint32(pair.substr(0, colon), item) ||
                !text_split::ParseUint32(pair.substr(colon + 1), count) || item == 0 || count == 0)
            {
                ok = false;
                break;
            }
            recipe.Reagents.emplace_back(item, count);
        }
        if (!ok)
        {
            fail("bad reagent list (item:count,...)");
            continue;
        }
        recipes.push_back(std::move(recipe));
    }
    return recipes;
}

std::optional<std::size_t> PlanRecipe(std::vector<Recipe> const& recipes, std::uint32_t skill, std::uint32_t value,
                                      std::function<bool(std::uint32_t spell)> const& skip)
{
    std::optional<std::size_t> best;
    auto const key = [&](std::size_t i) { return std::make_tuple(recipes[i].TrivialLow, recipes[i].MinSkill, recipes[i].Spell); };
    for (std::size_t i = 0; i < recipes.size(); ++i)
    {
        Recipe const& recipe = recipes[i];
        if (recipe.Skill != skill || value < recipe.MinSkill || value >= recipe.TrivialHigh)
            continue;
        if (skip && skip(recipe.Spell))
            continue;
        if (!best || key(i) < key(*best))
            best = i;
    }
    return best;
}

void EmptyNodes::Mark(std::size_t node, std::uint64_t untilMs)
{
    if (_until.size() >= kMaxKept && !_until.count(node))
    {
        auto oldest = _until.begin();
        for (auto it = _until.begin(); it != _until.end(); ++it)
            if (it->second < oldest->second)
                oldest = it;
        _until.erase(oldest);
    }
    _until[node] = untilMs;
}

bool EmptyNodes::Empty(std::size_t node, std::uint64_t nowMs) const
{
    auto const it = _until.find(node);
    return it != _until.end() && nowMs < it->second;
}

std::optional<std::size_t> NearestSafeNode(std::vector<gather::NodePoint> const& nodes, std::uint32_t skill, std::uint32_t map, float x, float y,
                                           gather::NodeFilter const& filter, float maxYards, std::function<bool(std::size_t)> const& unsafe,
                                           std::uint32_t& skipped)
{
    skipped = 0;
    std::optional<std::size_t> best;
    float bestDistance = 0.0f;
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        gather::NodePoint const& node = nodes[i];
        if (node.Skill != skill || node.Map != map)
            continue;
        if (node.RequiredSkill > filter.SkillValue || (filter.Level != 0xFFFFFFFFu && node.AreaLevel > filter.Level + filter.LevelSlack) ||
            (filter.Side != 0 && node.Side != 0 && node.Side != filter.Side))
            continue;
        float const distance = std::hypot(node.X - x, node.Y - y);
        if (distance > maxYards || (best && distance >= bestDistance))
            continue;
        if (unsafe && unsafe(i))
        {
            ++skipped;
            continue;
        }
        best = i;
        bestDistance = distance;
    }
    return best;
}

} // namespace professions
