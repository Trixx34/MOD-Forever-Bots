// SPDX-License-Identifier: GPL-2.0-or-later
// BotPolicy: profession progression (Professions, default off; professions.h, doc/professions.md). The skill-up lines, the skill cap, the safe node walk and the
// skinning ask after a loot. The craft plan is in ReadResult (kSkillUp, kCraft) and TurnCraftGather.
#include "bot_policy_internal.h"

#include <cmath>

void BotPolicy::ProfessionSkillUps(BotState& state, BotSnapshot const& bot)
{
    std::uint16_t const skill[2] = { bot.profession_skill[0], bot.profession_skill[1] };
    std::uint16_t const value[2] = { bot.profession_value[0], bot.profession_value[1] };
    if (Opt().OnEvent)
        for (professions::SkillUp const& up : professions::SkillUps(state.ProfSeenSkill, state.ProfSeenValue, skill, value))
            Opt().OnEvent(professions::SkillUpLine(bot.guid, up));
    for (int slot = 0; slot < 2; ++slot)
    {
        state.ProfSeenSkill[slot] = skill[slot];
        state.ProfSeenValue[slot] = value[slot];
    }
}

bool BotPolicy::ProfessionBelowCap(BotState& state, BotSnapshot const& bot, std::uint32_t line)
{
    for (int slot = 0; slot < 2; ++slot)
    {
        if (bot.profession_skill[slot] != line)
            continue;
        professions::Options const& o = Opt().Professions;
        if (professions::BelowCap(bot.profession_value[slot], bot.level, o.SkillCapPerLevel, bot.profession_max[slot]))
            return true;
        std::uint32_t const cap = professions::SkillCap(bot.level, o.SkillCapPerLevel, bot.profession_max[slot]);
        if (state.ProfCapLogged[slot] != cap && Opt().OnEvent)
            Opt().OnEvent("profession_cap: bot=" + std::to_string(bot.guid) + " skill=" + std::to_string(line) + " value=" +
                          std::to_string(bot.profession_value[slot]) + " cap=" + std::to_string(cap) + " level=" + std::to_string(bot.level));
        state.ProfCapLogged[slot] = cap;
        return false;
    }
    return true; // not a primary (Cooking, First Aid): no cap of this module
}

bool BotPolicy::ProfessionNodeUnsafe(BotSnapshot const& bot, std::size_t node, std::uint64_t nowMs) const
{
    if (_emptyNodes.Empty(node, nowMs))
        return true;
    gather::NodePoint const& point = (*Opt().GatherNodes)[node];
    // The level-safe check (level_safe.h): the node's place, and the straight way to it from where the bot stands, not above its level + MarginLevels.
    if (!_levelMap.Empty() && bot.level != 0)
    {
        std::uint32_t const margin = Opt().LevelSafe.MarginLevels;
        if (_levelMap.TooHigh(point.Map, point.X, point.Y, bot.level, margin) ||
            _levelMap.FirstTooHigh(bot.map_id, bot.pos_x, bot.pos_y, point.X, point.Y, bot.level, margin, Opt().LevelSafe.StepYards, 0.0f))
            return true;
    }
    // The shared danger map (danger_map.h; empty unless DangerMap or SafeRevive is on): the node's cell, and the way there.
    if (_dangerMap.Dangerous(point.Map, point.X, point.Y, bot.level, nowMs) ||
        _dangerMap.FirstDangerous(bot.map_id, bot.pos_x, bot.pos_y, point.X, point.Y, bot.level, nowMs, 0.0f))
        return true;
    return false;
}

bool BotPolicy::ProfessionNodeWalk(BotState& state, BotSnapshot const& bot, std::size_t index, std::uint64_t nowMs)
{
    professions::Options const& o = Opt().Professions;
    // A grouped bot follows its party: no detour for a node.
    if (bot.group_leader_guid != 0 || !ProfessionBelowCap(state, bot, kGatherLines[index]))
        return false;
    gather::NodeFilter filter;
    if (bot.profession_skill[0] == kGatherLines[index])
        filter.SkillValue = bot.profession_value[0];
    else if (bot.profession_skill[1] == kGatherLines[index])
        filter.SkillValue = bot.profession_value[1];
    filter.Level = bot.level;
    filter.Side = Opt().SideOf ? Opt().SideOf(bot.guid) : std::uint8_t(0);
    std::uint32_t skipped = 0;
    std::optional<std::size_t> const node =
        professions::NearestSafeNode(*Opt().GatherNodes, kGatherLines[index], bot.map_id, bot.pos_x, bot.pos_y, filter, o.NodeMaxYards,
                                     [&](std::size_t i) { return ProfessionNodeUnsafe(bot, i, nowMs); }, skipped);
    if (!node)
        return false;
    gather::NodePoint const& point = (*Opt().GatherNodes)[*node];
    state.GatherWalking = true;
    state.GatherWalkLine = std::uint32_t(index);
    state.GatherWalkIndex = std::uint32_t(*node);
    state.GatherWalkStartMs = nowMs;
    state.GatherRetryAtMs[index] = nowMs + kGatherWalkTimeoutMs; // held while the walk runs; its end sets the time
    if (Opt().OnEvent)
        Opt().OnEvent("profession_node_walk: bot=" + std::to_string(bot.guid) + " skill=" + std::to_string(kGatherLines[index]) + " node=" +
                      std::to_string(point.Entry) + " yards=" + std::to_string(int(std::hypot(point.X - bot.pos_x, point.Y - bot.pos_y))) +
                      " skipped=" + std::to_string(skipped));
    return true;
}

void BotPolicy::ProfessionSkinAfterLoot(BotState& state, BotSnapshot const& bot, std::uint32_t corpseLevel, std::uint64_t nowMs)
{
    professions::Options const& o = Opt().Professions;
    if (!o.Enabled || !o.SkinAfterLoot || !HasLine(bot, professions::kSkinning) || !ProfessionBelowCap(state, bot, professions::kSkinning))
        return;
    std::size_t index = 0;
    while (index < kGatherLineCount && kGatherLines[index] != professions::kSkinning)
        ++index;
    if (index == kGatherLineCount)
        return;
    // The loot is taken first (this tick's kLoot); the core skins only a looted corpse. The node back-off of this line does not hold the ask.
    state.GatherRetryAtMs[index] = nowMs + kSkinAfterLootMs;
    state.GatherFails[index] = 0;
    if (Opt().OnEvent)
        Opt().OnEvent("profession_skin: bot=" + std::to_string(bot.guid) + " corpse_level=" + std::to_string(corpseLevel));
}
