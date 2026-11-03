// SPDX-License-Identifier: GPL-2.0-or-later
#include "guild_milestone.h"

#include "bot_random.h"

namespace guild_milestone {

std::uint64_t Speaker::Pick(Settings const& settings, std::uint64_t guild, std::uint64_t member, std::uint32_t level, std::vector<std::uint64_t> const& members,
                            std::uint64_t seed)
{
    if (!settings.Enabled || members.empty())
        return 0;
    std::string const key = std::to_string(guild) + ":" + std::to_string(member) + ":" + std::to_string(level);
    if (!_seen.insert(key).second)
        return 0;
    if (_seen.size() > 2048)
        _seen.clear();
    std::uint64_t const event = guild * 1000003ull + member * 131ull + level;
    if (bot_random::Below(bot_random::Draw(seed, member, 0xE1, event), 100) >= settings.ChancePct)
        return 0;
    return members[bot_random::Below(bot_random::Draw(seed, member, 0xE2, event), std::uint32_t(members.size()))];
}

}  // namespace guild_milestone
