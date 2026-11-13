// SPDX-License-Identifier: GPL-2.0-or-later
// Bot guilds (see bot_guilds.h).
#include "bot_guilds.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <limits>
#include <map>
#include <set>
#include <system_error>

namespace bot_guilds {

namespace {

constexpr std::uint32_t kMinRaidSize = 2;
constexpr std::uint32_t kMaxRaidSize = 40;
constexpr std::uint32_t kUncapped = std::numeric_limits<std::uint32_t>::max();

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

std::uint32_t CapacityOf(Settings const& settings, Purpose kind)
{
    return kind == Purpose::Raiding ? EffectiveRaidSize(settings) : kUncapped;
}

// Reads one integer key: a value out of range is clamped and reported; a value that is not a number is reported and ignored.
void ReadKey(std::unordered_map<std::string, std::string> const& config, char const* key, std::uint32_t min, std::uint32_t max, std::uint32_t& value,
             std::vector<std::string>& warnings)
{
    auto const it = config.find(key);
    if (it == config.end())
        return;
    std::uint64_t parsed = 0;
    auto const& text = it->second;
    auto const [end, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (ec != std::errc() || end != text.data() + text.size())
    {
        warnings.push_back(std::string(key) + " is not a number; the default is kept");
        return;
    }
    if (parsed < min || parsed > max)
    {
        warnings.push_back(std::string(key) + " is out of range and is clamped");
        parsed = std::clamp<std::uint64_t>(parsed, min, max);
    }
    value = static_cast<std::uint32_t>(parsed);
}

// A bot that changes guild: the guild it leaves (0 none) and the purpose it joins (nullopt: it leaves and joins nothing).
struct Pending
{
    Bot const* Member;
    std::uint64_t From;
    std::optional<Purpose> Target;
};

}  // namespace

Settings LoadSettings(std::unordered_map<std::string, std::string> const& config, std::vector<std::string>& warnings)
{
    Settings settings;
    auto const enabled = config.find("GuildsEnabled");
    if (enabled != config.end())
        settings.Enabled = enabled->second == "1" || enabled->second == "true";
    auto const backoff = config.find("GuildsRefusalBackoff");
    if (backoff != config.end())
        settings.RefusalBackoff = backoff->second == "1" || backoff->second == "true";
    ReadKey(config, "GuildsLevelCap", 1, 255, settings.LevelCap, warnings);
    ReadKey(config, "GuildsRaidMinLevel", 1, 255, settings.RaidMinLevel, warnings);
    ReadKey(config, "GuildsLevelingShare", 0, 100, settings.LevelingShare, warnings);
    ReadKey(config, "GuildsRaidingShare", 0, 100, settings.RaidingShare, warnings);
    ReadKey(config, "GuildsUtilityShare", 0, 100, settings.UtilityShare, warnings);
    ReadKey(config, "GuildsRaidSize", kMinRaidSize, kMaxRaidSize, settings.RaidSize, warnings);
    if (settings.LevelingShare + settings.RaidingShare + settings.UtilityShare != 100)
    {
        warnings.push_back("Bot.Guilds shares do not add up to 100; 60/20/20 is used");
        settings.LevelingShare = 60;
        settings.RaidingShare = 20;
        settings.UtilityShare = 20;
    }
    return settings;
}

std::uint32_t EffectiveRaidSize(Settings const& settings)
{
    return std::clamp(settings.RaidSize, kMinRaidSize, kMaxRaidSize);
}

Purpose ShareOf(Settings const& settings, std::uint64_t guid)
{
    std::uint64_t const total = static_cast<std::uint64_t>(settings.LevelingShare) + settings.RaidingShare + settings.UtilityShare;
    if (total == 0)
        return Purpose::Leveling;
    std::uint64_t const r = guid % total;
    if (r < settings.LevelingShare)
        return Purpose::Leveling;
    if (r < static_cast<std::uint64_t>(settings.LevelingShare) + settings.RaidingShare)
        return Purpose::Raiding;
    return Purpose::Utility;
}

Purpose WantOf(Settings const& settings, Bot const& bot)
{
    return bot.Want ? *bot.Want : ShareOf(settings, bot.Guid);
}

bool RaidQualified(Settings const& settings, Bot const& bot)
{
    if (bot.Level < settings.RaidMinLevel)
        return false;
    return !settings.RaidQualified || settings.RaidQualified(bot);
}

Plan PlanMoves(Settings const& settings, std::vector<Guild> const& guilds, std::vector<Bot> const& pool)
{
    Plan plan;
    if (!settings.Enabled)
        return plan;

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

        Purpose const want = WantOf(settings, *bot);
        bool const stays = current && want == *current && !(want == Purpose::Leveling && bot->Level >= settings.LevelCap);
        if (stays)
            continue;

        std::optional<Purpose> target;
        switch (want)
        {
            case Purpose::Leveling: if (bot->Level < settings.LevelCap) target = Purpose::Leveling; break;
            case Purpose::Utility: target = Purpose::Utility; break;
            case Purpose::Raiding: if (RaidQualified(settings, *bot)) target = Purpose::Raiding; break;
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
            std::uint32_t const capacity = CapacityOf(settings, *p.Target);
            std::uint64_t guildGuid = 0;
            for (auto const& [guid, guild] : known)
            {
                if (guild->Kind == *p.Target && guild->Alliance == p.Member->Alliance && mixed.count(guid) == 0 && count[guid] < capacity)
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
                if (open == openNew.end() || plan.Created[open->second].Members.size() >= capacity)
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
