// SPDX-License-Identifier: GPL-2.0-or-later
// Guild milestone chat (config GuildMilestone, off by default): when a player member of a guild gains a level and the guild has bot members, one random bot of
// the guild says a line in guild chat, with chance GuildMilestoneChancePct. A second bot never answers the same event. Lines: data/guild_milestones.txt
// ({level}, {name} not used). Pure logic, tests/guild_milestone_test.cpp.
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace guild_milestone {

struct Settings
{
    bool Enabled = false;                 // GuildMilestone
    std::uint32_t ChancePct = 30;         // GuildMilestoneChancePct
};

class Speaker
{
public:
    // The bot that speaks for this event, or 0. `members` are the bots of the guild (never the leveller, who is a player). An event (guild, member, level) is
    // answered at most once, whatever the outcome of the chance.
    std::uint64_t Pick(Settings const& settings, std::uint64_t guild, std::uint64_t member, std::uint32_t level, std::vector<std::uint64_t> const& members,
                       std::uint64_t seed);

private:
    std::set<std::string> _seen;
};

}  // namespace guild_milestone
