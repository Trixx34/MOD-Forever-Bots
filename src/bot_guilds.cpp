// SPDX-License-Identifier: GPL-2.0-or-later
// Bot guilds (see bot_guilds.h).
#include "bot_guilds.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

namespace bot_guilds {

namespace {

constexpr std::uint32_t kMinSize = 2;
constexpr std::uint32_t kMaxSize = 40;

// The faction a guild's bots are of: 1 Alliance, 2 Horde, 3 both (mixed, never changed).
int SideOf(std::vector<Bot const*> const& members)
{
    bool alliance = false, horde = false;
    for (Bot const* bot : members)
        (bot->Alliance ? alliance : horde) = true;
    return alliance && horde ? 3 : (alliance ? 1 : 2);
}

}  // namespace

std::uint32_t EffectiveGuildSize(Settings const& settings)
{
    return std::clamp(settings.GuildSize, kMinSize, kMaxSize);
}

Plan PlanGuilds(Settings const& settings, std::vector<Bot> const& pool)
{
    Plan plan;
    std::uint32_t const size = EffectiveGuildSize(settings);

    // The bots already in a guild, by guild, and the unplaced bots. A guid listed twice counts once.
    std::map<std::uint64_t, std::vector<Bot const*>> byGuild;
    std::set<std::uint64_t> seen;
    std::vector<Bot const*> unplaced;
    for (Bot const& bot : pool)
    {
        if (bot.Guid == 0 || !seen.insert(bot.Guid).second)
            continue;
        if (bot.Guild == 0)
            unplaced.push_back(&bot);
        else
            byGuild[bot.Guild].push_back(&bot);
    }

    // Guilds with bots of both factions are not touched. Guilds per faction count for the names of the new ones.
    std::map<std::uint64_t, int> sideOfGuild;   // guild -> 1, 2 or 3
    std::uint32_t guildsPerSide[3] = { 0, 0, 0 };   // index 0 Alliance, 1 Horde (the 3 is mixed, not used)
    std::map<std::uint64_t, std::uint32_t> fill;    // members per guild, for the guilds that take joins
    for (auto const& [guild, members] : byGuild)
    {
        int side = SideOf(members);
        sideOfGuild[guild] = side;
        if (side == 3)
        {
            plan.MixedGuilds.push_back(guild);
            continue;
        }
        guildsPerSide[side == 1 ? 0 : 1] += 1;
        fill[guild] = static_cast<std::uint32_t>(members.size());
    }

    for (int side = 1; side <= 2; ++side)
    {
        bool const alliance = side == 1;
        std::vector<Bot const*> todo;
        for (Bot const* bot : unplaced)
            if (bot->Alliance == alliance)
                todo.push_back(bot);
        std::sort(todo.begin(), todo.end(), [](Bot const* a, Bot const* b) { return a->Guid < b->Guid; });
        if (todo.empty())
            continue;

        // Fill the guilds of this faction that have room, lowest guild guid first.
        std::size_t next = 0;
        for (auto& [guild, count] : fill)
        {
            if (next == todo.size())
                break;
            if (sideOfGuild[guild] != side)
                continue;
            while (count < size && next < todo.size())
            {
                plan.Joins.push_back({ todo[next]->Guid, guild });
                ++next;
                ++count;
            }
        }

        // The rest make new guilds of `size` in guid order. The first of each is its master.
        std::uint32_t ordinal = guildsPerSide[side == 1 ? 0 : 1];
        while (next < todo.size())
        {
            NewGuild created;
            created.Alliance = alliance;
            ++ordinal;
            char name[32];
            std::snprintf(name, sizeof(name), "BotGuild %c%03u", alliance ? 'A' : 'H', static_cast<unsigned>(ordinal));
            created.Name = name;
            for (std::uint32_t i = 0; i < size && next < todo.size(); ++i, ++next)
                created.Members.push_back(todo[next]->Guid);
            created.Master = created.Members.front();
            plan.Created.push_back(std::move(created));
        }
    }

    std::sort(plan.MixedGuilds.begin(), plan.MixedGuilds.end());
    return plan;
}

std::vector<std::uint64_t> OnlineGuildMates(std::vector<Bot> const& pool, std::uint64_t self, std::uint32_t minLevel)
{
    std::vector<std::uint64_t> mates;
    Bot const* me = nullptr;
    for (Bot const& bot : pool)
        if (bot.Guid == self)
            me = &bot;
    if (me == nullptr || me->Guild == 0)
        return mates;
    for (Bot const& bot : pool)
        if (bot.Guid != self && bot.Guild == me->Guild && bot.Alliance == me->Alliance && bot.Online && bot.Level >= minLevel)
            mates.push_back(bot.Guid);
    std::sort(mates.begin(), mates.end());
    return mates;
}

}  // namespace bot_guilds
