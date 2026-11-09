// SPDX-License-Identifier: GPL-2.0-or-later
// Bot guilds: which guild each bot of the pool is in, by guild purpose. Pure logic, tests/bot_guilds_test.cpp. No core needed.
//
// Three purposes, one faction per guild:
//   Leveling  bots below LevelCap. A bot leaves at LevelCap or when its want changes. Not capped in size. An empty leveling guild is dissolved.
//   Utility   dungeons, professions, auction house. Long-term: a bot stays until its want changes. Not capped in size.
//   Raiding   a bot joins only at RaidMinLevel or more and when RaidQualified. Capped at RaidSize bots. It stays until its want changes.
//
// The pool split: a bot with no Want set gets its purpose from its guid and the shares (GuildsLevelingShare, RaidingShare, UtilityShare; 60/20/20).
// A bot whose Want is set (by the policy) keeps it. GuildsEnabled is off by default: then PlanMoves plans nothing.
//
// PlanMoves is the one function that decides membership. It returns one Move per bot that changes, with the guild it leaves (From) and the guild it joins
// (To, or a new guild by index). A bot appears once, so it is never in two guilds at once. Running PlanMoves on the result of the moves gives no moves.
// No guild mixes factions: a guild whose bots are of both factions is reported and left alone, and nobody joins it.
//
// The pool is the bots that are online: the planner only sees what the caller passes, and offline bots are not covered yet.
//
// GuildMates is the roster query: the bots of a purpose (and faction, online, level, raid-ready) that a bot can group with.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bot_guilds {

enum class Purpose : std::uint8_t { Leveling = 0, Utility = 1, Raiding = 2 };

struct Bot;

struct Settings
{
    bool Enabled = false;               // GuildsEnabled
    std::uint32_t LevelCap = 60;        // GuildsLevelCap: a leveling bot at this level leaves
    std::uint32_t RaidMinLevel = 60;    // GuildsRaidMinLevel: a raiding bot must be at least this
    std::uint32_t LevelingShare = 60;   // GuildsLevelingShare (percent of the pool)
    std::uint32_t RaidingShare = 20;    // GuildsRaidingShare
    std::uint32_t UtilityShare = 20;    // GuildsUtilityShare
    std::uint32_t RaidSize = 40;        // GuildsRaidSize: bots per raiding guild (2 to 40)
    // The raiding check beyond the level (attunement, gear). Empty: the level alone qualifies.
    std::function<bool(Bot const&)> RaidQualified;
};

// Reads the Guilds* keys from the config map. A bad value is clamped and reported in warnings; shares that do not add up to 100 are replaced by 60/20/20.
Settings LoadSettings(std::unordered_map<std::string, std::string> const& config, std::vector<std::string>& warnings);

// A guild the caller knows (a bot guild). Guid is its guild id in the core, Alliance its one faction.
struct Guild
{
    std::uint64_t Guid = 0;
    Purpose Kind = Purpose::Leveling;
    bool Alliance = false;
};

// One bot of the pool. Guild is its current guild (0 none). Want is the purpose the policy sets (nullopt: the pool share decides).
struct Bot
{
    std::uint64_t Guid = 0;
    std::uint64_t Guild = 0;
    bool Alliance = false;
    bool Online = false;
    std::uint32_t Level = 1;
    std::optional<Purpose> Want;
};

// A bot changes guild: From is the guild it leaves (0 none), To the guild it joins (0 none), or NewGuild is the index in Plan::Created it joins (-1 none).
struct Move
{
    std::uint64_t Bot = 0;
    std::uint64_t From = 0;
    std::uint64_t To = 0;
    int NewGuild = -1;
};

struct NewGuild
{
    std::string Name;                    // "BotLvl A001", "BotUtil H002", "BotRaid A001": tag, faction and ordinal among the guilds of that kind and faction
    Purpose Kind = Purpose::Leveling;
    bool Alliance = false;
    std::uint64_t Master = 0;            // the lowest guid of the members
    std::vector<std::uint64_t> Members;  // in guid order, the master first
};

struct Plan
{
    std::vector<Move> Moves;             // in guid order
    std::vector<NewGuild> Created;
    std::vector<std::uint64_t> Dissolved;    // empty leveling guilds: disband them
    std::vector<std::uint64_t> Mixed;        // guilds with both factions: left alone
};

// The raiding guild size actually used (clamped to 2 to 40).
std::uint32_t EffectiveRaidSize(Settings const& settings);

// The purpose a bot with no Want gets from its guid and the shares.
Purpose ShareOf(Settings const& settings, std::uint64_t guid);

// The purpose the bot wants: its Want if set, else its share.
Purpose WantOf(Settings const& settings, Bot const& bot);

// Whether a bot may join a raiding guild: level at least RaidMinLevel, and RaidQualified if set.
bool RaidQualified(Settings const& settings, Bot const& bot);

// The moves for the pool. `guilds` are the bot guilds known to the caller; a bot in a guild not listed is outside this plan and is not moved.
// Enabled off: an empty plan.
Plan PlanMoves(Settings const& settings, std::vector<Guild> const& guilds, std::vector<Bot> const& pool);

// The roster query. Candidates are the bots in a guild of Kind and the faction of self, not self. SameGuildOnly: only the guild of self.
struct MateQuery
{
    Purpose Kind = Purpose::Utility;
    std::uint32_t MinLevel = 60;
    bool OnlineOnly = true;
    bool SameGuildOnly = false;
    bool RaidReadyOnly = false;          // only the bots RaidQualified accepts
};

// Sorted by guid.
std::vector<std::uint64_t> GuildMates(Settings const& settings, std::vector<Guild> const& guilds, std::vector<Bot> const& pool, std::uint64_t self,
                                      MateQuery const& query);

}  // namespace bot_guilds
