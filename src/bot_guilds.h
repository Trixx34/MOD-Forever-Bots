// SPDX-License-Identifier: GPL-2.0-or-later
// Bot guilds: the grouping of the bot pool into guilds, one faction per guild, for later group quests. Pure logic, tests/bot_guilds_test.cpp. No core needed.
//
// PlanGuilds decides who joins which guild. It is idempotent: a bot that already has a guild is never moved, a guild that has room takes the unplaced bots of
// its faction first, and only then are new guilds made (GuildSize bots each, the last may be smaller). The guild master of a new guild is its lowest guid bot.
// Creating the guilds in the core is not done here (see the design note): the plan only says what to create and whom to invite.
//
// GuildGuidOfMate / OnlineGuildMates is the roster query a group-quest feature asks: "who are my guild-mates that are online and at the level?".
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bot_guilds {

struct Settings
{
    std::uint32_t GuildSize = 10;   // bots per new guild (clamped to 2 to 40)
};

// One bot of the pool. guild is the guild guid the bot is in (0 none). alliance is its faction. online and level are for the roster query.
struct Bot
{
    std::uint64_t Guid = 0;
    std::uint64_t Guild = 0;
    bool Alliance = false;
    bool Online = false;
    std::uint32_t Level = 1;
};

struct Join
{
    std::uint64_t Bot = 0;
    std::uint64_t Guild = 0;        // an existing guild with room
};

struct NewGuild
{
    std::string Name;               // "BotGuild A007": faction letter and the ordinal, unique among the bot guilds of that faction
    bool Alliance = false;
    std::uint64_t Master = 0;       // the lowest guid of the members
    std::vector<std::uint64_t> Members;   // in guid order, the master first
};

struct Plan
{
    std::vector<Join> Joins;
    std::vector<NewGuild> Created;
    std::vector<std::uint64_t> MixedGuilds;   // guilds whose bots are of both factions: left as they are, reported
};

// The guild size actually used (clamped).
std::uint32_t EffectiveGuildSize(Settings const& settings);

// The plan for the pool. Bots of the same faction only share a guild. Running it again on the result (the joins applied, the new guilds given the guids the
// caller created) gives an empty plan.
Plan PlanGuilds(Settings const& settings, std::vector<Bot> const& pool);

// The guild-mates of `self` that are online and at least minLevel: same non-zero guild, not self, same faction, sorted by guid.
std::vector<std::uint64_t> OnlineGuildMates(std::vector<Bot> const& pool, std::uint64_t self, std::uint32_t minLevel = 60);

}  // namespace bot_guilds
