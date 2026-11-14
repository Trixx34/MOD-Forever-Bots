// SPDX-License-Identifier: GPL-2.0-or-later
// Bot guilds (see bot_guilds.h).
#include "bot_guilds.h"

#include <algorithm>
#include <cctype>
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
        case Purpose::Pvp: return "BotPvp";
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

bool ReadFlag(std::unordered_map<std::string, std::string> const& config, char const* key, bool& value)
{
    auto const it = config.find(key);
    if (it == config.end())
        return false;
    value = it->second == "1" || it->second == "true";
    return true;
}

// SplitMix64: a well-mixed 64-bit value from a seed (the name draw and the pvp share).
std::uint64_t Mix(std::uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

std::optional<NameStyle> StyleFromName(std::string const& text)
{
    if (text == "lore") return NameStyle::Lore;
    if (text == "funny") return NameStyle::Funny;
    if (text == "normal") return NameStyle::Normal;
    if (text == "reallife") return NameStyle::RealLife;
    return std::nullopt;
}

bool ValidGuildName(std::string const& name)
{
    if (name.size() < 2 || name.size() > 24 || !std::isalpha(static_cast<unsigned char>(name[0])))
        return false;
    for (char const c : name)
        if (!std::isalpha(static_cast<unsigned char>(c)) && c != ' ' && c != '\'' && c != '-')
            return false;
    return true;
}

std::vector<std::string> Split(std::string const& text, char separator)
{
    std::vector<std::string> parts;
    std::string current;
    for (char const c : text)
    {
        if (c == separator)
        {
            parts.push_back(current);
            current.clear();
        }
        else
            current += c;
    }
    parts.push_back(current);
    return parts;
}

}  // namespace

StyleWeights DefaultStyleWeights(Purpose kind)
{
    // Indexed lore, funny, normal, reallife.
    switch (kind)
    {
        case Purpose::Raiding: return StyleWeights{ 3, 1, 3, 1 };
        case Purpose::Pvp: return StyleWeights{ 2, 0, 3, 2 };
        case Purpose::Leveling:
        case Purpose::Utility: break;
    }
    return StyleWeights{ 1, 1, 1, 1 };
}

Settings LoadSettings(std::unordered_map<std::string, std::string> const& config, std::vector<std::string>& warnings)
{
    Settings settings;
    auto const enabled = config.find("GuildsEnabled");
    if (enabled != config.end())
        settings.Enabled = enabled->second == "1" || enabled->second == "true";
    ReadFlag(config, "GuildsRefusalBackoff", settings.RefusalBackoff);
    ReadFlag(config, "GuildsKindByLevel", settings.KindByLevel);
    ReadFlag(config, "GuildsRemember", settings.Remember);
    ReadKey(config, "GuildsPvpShare", 0, 100, settings.PvpShare, warnings);
    // The pvp level follows the battleground level unless it is set itself (a key the battlegrounds read too; warnings for it come from there).
    if (config.count("GuildsPvpMinLevel") != 0)
        ReadKey(config, "GuildsPvpMinLevel", 1, 255, settings.PvpMinLevel, warnings);
    else
    {
        std::vector<std::string> ignored;
        ReadKey(config, "BattlegroundMinLevel", 1, 255, settings.PvpMinLevel, ignored);
    }
    if (auto const file = config.find("GuildsNameFile"); file != config.end())
        settings.NameFile = file->second;
    static char const* const kStyleKeys[kPurposeCount] = { "GuildsNameStylesLeveling", "GuildsNameStylesUtility", "GuildsNameStylesRaiding",
                                                           "GuildsNameStylesPvp" };
    for (std::size_t kind = 0; kind < kPurposeCount; ++kind)
    {
        auto const it = config.find(kStyleKeys[kind]);
        if (it != config.end() && !ParseStyleWeights(it->second, settings.NameStyles[kind]))
            warnings.push_back(std::string(kStyleKeys[kind]) + " is not \"style:weight ...\" (styles lore, funny, normal, reallife); the default is kept");
    }
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

bool InPvpShare(Settings const& settings, std::uint64_t guid)
{
    return settings.PvpShare > 0 && Mix(guid ^ 0x5076705368617265ull) % 100 < settings.PvpShare;
}

Purpose KindOf(Settings const& settings, Bot const& bot)
{
    if (bot.Level >= settings.PvpMinLevel && InPvpShare(settings, bot.Guid))
        return Purpose::Pvp;
    if (!settings.KindByLevel)
        return ShareOf(settings, bot.Guid);
    if (RaidQualified(settings, bot))
        return Purpose::Raiding;
    if (bot.Level < settings.LevelCap)
        return Purpose::Leveling;
    return Purpose::Utility;
}

Purpose WantOf(Settings const& settings, Bot const& bot)
{
    return bot.Want ? *bot.Want : KindOf(settings, bot);
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
            case Purpose::Pvp: if (bot->Level >= settings.PvpMinLevel) target = Purpose::Pvp; break;
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

    // Empty leveling guilds are dissolved; utility, raiding and pvp guilds are kept.
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

// ---- Names ----

std::vector<NameEntry> ParseNames(std::string const& text, std::vector<std::string>& warnings)
{
    std::vector<NameEntry> names;
    std::set<std::string> seen;
    std::size_t lineNo = 0;
    for (std::string line : Split(text, '\n'))
    {
        ++lineNo;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos || line[0] == '#')
            continue;
        std::vector<std::string> const fields = Split(line, '|');
        std::string const where = "guild names line " + std::to_string(lineNo);
        if (fields.size() != 4 || fields[0] != "guild")
        {
            warnings.push_back(where + ": expected guild|style|weight|name");
            continue;
        }
        std::optional<NameStyle> const style = StyleFromName(fields[1]);
        std::uint32_t weight = 0;
        auto const [end, ec] = std::from_chars(fields[2].data(), fields[2].data() + fields[2].size(), weight);
        if (!style || ec != std::errc() || end != fields[2].data() + fields[2].size() || weight < 1 || weight > 100 || !ValidGuildName(fields[3]))
        {
            warnings.push_back(where + ": bad style, weight or name");
            continue;
        }
        if (!seen.insert(NameKey(fields[3])).second)
        {
            warnings.push_back(where + ": duplicate name " + fields[3]);
            continue;
        }
        names.push_back(NameEntry{ *style, weight, fields[3] });
    }
    return names;
}

bool ParseStyleWeights(std::string const& text, StyleWeights& weights)
{
    StyleWeights parsed = weights;
    std::string spaced = text;
    std::replace(spaced.begin(), spaced.end(), ',', ' ');
    for (std::string const& word : Split(spaced, ' '))
    {
        if (word.empty())
            continue;
        auto const colon = word.find(':');
        if (colon == std::string::npos)
            return false;
        std::optional<NameStyle> const style = StyleFromName(word.substr(0, colon));
        std::string const number = word.substr(colon + 1);
        std::uint32_t value = 0;
        auto const [end, ec] = std::from_chars(number.data(), number.data() + number.size(), value);
        if (!style || number.empty() || ec != std::errc() || end != number.data() + number.size() || value > 100)
            return false;
        parsed[static_cast<std::size_t>(*style)] = value;
    }
    weights = parsed;
    return true;
}

std::string NameKey(std::string const& name)
{
    std::string key = name;
    for (char& c : key)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return key;
}

std::optional<std::string> PickName(Settings const& settings, std::vector<NameEntry> const& names, Purpose kind,
                                    std::function<bool(std::string const&)> const& taken, std::uint64_t seed)
{
    StyleWeights const& styles = settings.NameStyles[static_cast<std::size_t>(kind)];
    std::vector<std::pair<std::uint64_t, NameEntry const*>> pool;   // running total, entry
    std::uint64_t total = 0;
    for (NameEntry const& entry : names)
    {
        std::uint64_t const weight = static_cast<std::uint64_t>(entry.Weight) * styles[static_cast<std::size_t>(entry.Style)];
        if (weight == 0 || (taken && taken(NameKey(entry.Name))))
            continue;
        total += weight;
        pool.emplace_back(total, &entry);
    }
    if (total == 0)
        return std::nullopt;
    std::uint64_t const roll = Mix(seed) % total;
    auto const hit = std::upper_bound(pool.begin(), pool.end(), roll, [](std::uint64_t r, auto const& item) { return r < item.first; });
    return hit->second->Name;
}

// ---- Store rows ----

char const* KindName(Purpose kind)
{
    switch (kind)
    {
        case Purpose::Leveling: return "leveling";
        case Purpose::Utility: return "utility";
        case Purpose::Raiding: return "raiding";
        case Purpose::Pvp: return "pvp";
    }
    return "leveling";
}

std::optional<Purpose> KindFromName(std::string const& text)
{
    for (Purpose kind : { Purpose::Leveling, Purpose::Utility, Purpose::Raiding, Purpose::Pvp })
        if (text == KindName(kind))
            return kind;
    return std::nullopt;
}

std::string StoreKeyFor(std::uint64_t guild)
{
    return std::string(kStorePrefix) + std::to_string(guild);
}

bool GuildFromStoreKey(std::string const& key, std::uint64_t& guild)
{
    std::string const prefix = kStorePrefix;
    if (key.size() <= prefix.size() || key.compare(0, prefix.size(), prefix) != 0)
        return false;
    std::uint64_t parsed = 0;
    char const* begin = key.data() + prefix.size();
    char const* last = key.data() + key.size();
    auto const [end, ec] = std::from_chars(begin, last, parsed);
    if (ec != std::errc() || end != last || parsed == 0)
        return false;
    guild = parsed;
    return true;
}

std::string EncodeRecord(Guild const& guild, std::string const& name)
{
    return std::string(KindName(guild.Kind)) + (guild.Alliance ? " A " : " H ") + name;
}

bool DecodeRecord(std::string const& value, Purpose& kind, bool& alliance, std::string& name)
{
    auto const first = value.find(' ');
    if (first == std::string::npos || first + 2 > value.size())
        return false;
    std::optional<Purpose> const parsedKind = KindFromName(value.substr(0, first));
    char const side = value[first + 1];
    if (!parsedKind || (side != 'A' && side != 'H') || (first + 2 < value.size() && value[first + 2] != ' '))
        return false;
    kind = *parsedKind;
    alliance = side == 'A';
    name = first + 3 <= value.size() ? value.substr(first + 3) : std::string();
    return true;
}

}  // namespace bot_guilds
