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

char const* TagOf(Purpose kind)
{
    switch (kind)
    {
        case Purpose::Leveling: return "BotLvl";
        case Purpose::Utility: return "BotUtil";
        case Purpose::Raiding: return "BotRaid";
    }
    return "BotGuild";
}

// A bot that changes guild: the guild it leaves (0 none) and the purpose it joins (nullopt: it leaves and joins nothing).
struct Pending
{
    Bot const* Member;
    std::uint64_t From;
    std::optional<Purpose> Target;
};

}  // namespace

std::uint32_t EffectiveGuildSize(Settings const& settings)
{
    return std::clamp(settings.GuildSize, kMinSize, kMaxSize);
}

bool RaidQualified(Settings const& settings, Bot const& bot)
{
    if (bot.Level < settings.LevelCap)
        return false;
    return !settings.RaidQualified || settings.RaidQualified(bot);
}

Plan PlanMoves(Settings const& settings, std::vector<Guild> const& guilds, std::vector<Bot> const& pool)
{
    Plan plan;
    std::uint32_t const size = EffectiveGuildSize(settings);

    // Bots by guid (the first listed wins), and the known guilds by guid.
    std::map<std::uint64_t, Bot const*> bots;
    for (Bot const& bot : pool)
        if (bot.Guid != 0)
            bots.emplace(bot.Guid, &bot);
    std::map<std::uint64_t, Guild const*> known;
    for (Guild const& guild : guilds)
        if (guild.Guid != 0)
            known.emplace(guild.Guid, &guild);

    // The factions in each known guild (bit 1 Alliance, bit 2 Horde). A guild with both is mixed: left alone, and nobody joins it.
    std::map<std::uint64_t, int> sides;
    std::map<std::uint64_t, std::uint32_t> count;   // bots in each known guild, as the plan goes
    for (auto const& [guid, bot] : bots)
    {
        if (bot->Guild == 0 || known.count(bot->Guild) == 0)
            continue;
        sides[bot->Guild] |= bot->Alliance ? 1 : 2;
        count[bot->Guild] += 1;
    }
    std::set<std::uint64_t> mixed;
    for (auto const& [guild, bits] : sides)
        if (bits == 3)
        {
            mixed.insert(guild);
            plan.Mixed.push_back(guild);
        }

    // Pass A: who stays, who leaves, and where each leaver or unguilded bot should go.
    std::vector<Pending> pending;
    for (auto const& [guid, bot] : bots)
    {
        if (bot->Guild != 0 && (known.count(bot->Guild) == 0 || mixed.count(bot->Guild)))
            continue;                                   // a guild this plan does not manage
        std::optional<Purpose> current;
        if (bot->Guild != 0)
            current = known.at(bot->Guild)->Kind;

        bool const stays = current && bot->Want && *bot->Want == *current &&
                           !(*current == Purpose::Leveling && bot->Level >= settings.LevelCap);
        if (stays)
            continue;

        std::optional<Purpose> target;
        if (bot->Want)
        {
            switch (*bot->Want)
            {
                case Purpose::Leveling: if (bot->Level < settings.LevelCap) target = Purpose::Leveling; break;
                case Purpose::Utility: target = Purpose::Utility; break;
                case Purpose::Raiding: if (RaidQualified(settings, *bot)) target = Purpose::Raiding; break;
            }
        }
        if (!current && !target)
            continue;
        if (current)
            count[bot->Guild] -= 1;
        pending.push_back({ bot, bot->Guild, target });
    }

    // Pass B: the placements. A guild with room of the purpose and faction takes the bot (lowest guid first); otherwise the bot starts or fills a new one.
    std::map<std::pair<Purpose, bool>, int> openNew;      // (purpose, faction) -> index of the new guild still taking members
    std::map<std::pair<Purpose, bool>, std::uint32_t> madeNew;
    std::map<std::pair<Purpose, bool>, std::uint32_t> existingOfKind;
    for (auto const& [guid, guild] : known)
        existingOfKind[{ guild->Kind, guild->Alliance }] += 1;

    std::map<std::uint64_t, Move> moves;
    for (Pending const& p : pending)
    {
        Move move;
        move.Bot = p.Member->Guid;
        move.From = p.From;
        if (p.Target)
        {
            std::uint64_t guildGuid = 0;
            for (auto const& [guid, guild] : known)
            {
                if (guild->Kind == *p.Target && guild->Alliance == p.Member->Alliance && mixed.count(guid) == 0 && count[guid] < size)
                {
                    guildGuid = guid;
                    break;
                }
            }
            if (guildGuid != 0)
            {
                move.To = guildGuid;
                count[guildGuid] += 1;
            }
            else
            {
                std::pair<Purpose, bool> key{ *p.Target, p.Member->Alliance };
                auto open = openNew.find(key);
                if (open == openNew.end() || plan.Created[open->second].Members.size() >= size)
                {
                    NewGuild created;
                    created.Kind = *p.Target;
                    created.Alliance = p.Member->Alliance;
                    std::uint32_t ordinal = existingOfKind[key] + (++madeNew[key]);
                    char name[48];
                    std::snprintf(name, sizeof(name), "%s %c%03u", TagOf(*p.Target), created.Alliance ? 'A' : 'H', static_cast<unsigned>(ordinal));
                    created.Name = name;
                    plan.Created.push_back(created);
                    openNew[key] = static_cast<int>(plan.Created.size()) - 1;
                }
                NewGuild& group = plan.Created[openNew[key]];
                group.Members.push_back(p.Member->Guid);
                group.Master = group.Members.front();
                move.NewGuild = openNew[key];
            }
        }
        moves[move.Bot] = move;
    }

    for (Pending const& p : pending)
        plan.Moves.push_back(moves.at(p.Member->Guid));

    // Empty leveling guilds are dissolved; utility and raiding guilds are kept.
    for (auto const& [guid, guild] : known)
        if (guild->Kind == Purpose::Leveling && mixed.count(guid) == 0 && count[guid] == 0)
            plan.Dissolved.push_back(guid);

    return plan;
}

std::vector<std::uint64_t> GuildMates(Settings const& settings, std::vector<Guild> const& guilds, std::vector<Bot> const& pool, std::uint64_t self,
                                      MateQuery const& query)
{
    std::vector<std::uint64_t> mates;
    Bot const* me = nullptr;
    for (Bot const& bot : pool)
        if (bot.Guid == self)
        {
            me = &bot;
            break;
        }
    if (me == nullptr)
        return mates;

    std::map<std::uint64_t, Guild const*> known;
    for (Guild const& guild : guilds)
        if (guild.Guid != 0)
            known.emplace(guild.Guid, &guild);

    std::set<std::uint64_t> found;
    for (Bot const& bot : pool)
    {
        if (bot.Guid == 0 || bot.Guid == self || bot.Guild == 0)
            continue;
        if (query.OnlineOnly && !bot.Online)
            continue;
        if (bot.Level < query.MinLevel || bot.Alliance != me->Alliance)
            continue;
        auto guild = known.find(bot.Guild);
        if (guild == known.end() || guild->second->Kind != query.Kind)
            continue;
        if (query.SameGuildOnly && (me->Guild == 0 || bot.Guild != me->Guild))
            continue;
        if (query.RaidReadyOnly && !RaidQualified(settings, bot))
            continue;
        found.insert(bot.Guid);
    }
    mates.assign(found.begin(), found.end());
    return mates;
}

}  // namespace bot_guilds
