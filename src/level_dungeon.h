// SPDX-License-Identifier: GPL-2.0-or-later
// Level dungeons: which dungeon a levelling bot party is due to run, and the LFG planning. Pure logic, no core, unit-tested in tests/level_dungeon_test.cpp;
// BotPolicy (bot_policy.cpp) drives it and reuses the "tank lead" route walk for the run itself.
//
// Schedule: a party runs a dungeon at the HIGH end of its level range. The party's level (its lowest member) must have reached the dungeon's max level (and not be more
// than kGraceLevels above it: a party that levelled past a dungeon without being able to go does not catch up on it). A dungeon is run at most `maxRuns` times by one party
// (config LevelDungeonRuns), at most once per level: a second attempt, after a first that did not clear it, waits for the next level. A cleared dungeon is done.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace level_dungeon {

// A dungeon as the schedule sees it (made from the dungeon file: id, name, level range, instance map).
struct Dungeon
{
    std::string Id;
    std::string Name;
    std::uint32_t MinLevel = 1;
    std::uint32_t MaxLevel = 60;
    std::uint32_t MapId = 0;
};

constexpr std::uint32_t kGraceLevels = 2;     // a dungeon is still due this many levels above its max level
constexpr std::uint32_t kMinRuns = 1;
constexpr std::uint32_t kMaxRunsLimit = 5;
constexpr std::uint32_t kPartyMax = 5;        // LFG fills a group up to this many

std::uint32_t ClampRuns(std::uint32_t runs);  // 1..5

// What one party has done, by dungeon id.
struct Book
{
    struct Record
    {
        std::uint32_t Runs = 0;        // attempts started
        std::uint32_t LastLevel = 0;   // the party level at the last attempt
        bool Cleared = false;
    };
    std::map<std::string, Record> Dungeons;
};

// The index in `dungeons` of the dungeon this party is due to run, or -1. The one with the lowest max level goes first (then the lowest id).
int Due(std::vector<Dungeon> const& dungeons, std::uint32_t partyLevel, Book const& book, std::uint32_t maxRuns);
void NoteStarted(Book& book, std::string const& id, std::uint32_t partyLevel);
void NoteCleared(Book& book, std::string const& id);

// LFG: the dungeon a player named, by key ("rfc") or by name ("ragefire", "Ragefire Chasm"), case-insensitive. The key wins over a name word; a word must be at least
// 3 letters to match a name word. Returns -1 when none or when the word fits several names.
int FindDungeon(std::vector<Dungeon> const& dungeons, std::string_view text);
// "dm, rfc, wc" - the keys, for the whisper to a player who named an unknown dungeon.
std::string KeyList(std::vector<Dungeon> const& dungeons);

enum class Role : std::uint8_t { Tank, Healer, Dps };
char const* RoleName(Role role);
// The role a class id fills for LFG (tank = warrior; healer = priest, paladin, shaman, druid; dps = hunter, rogue, mage, warlock).
Role RoleOfClass(std::uint32_t classId);

// The roles to bring so a group reaches kPartyMax: `size` members now (the speaker included), `tanks` and `healers` of them already there. A tank, then a healer, if
// missing, then damage dealers for the rest.
std::vector<Role> MissingRoles(std::uint32_t size, std::uint32_t tanks, std::uint32_t healers);
// "tank, dps" (the roles of the list, a count after a role that repeats: "dps x2").
std::string RoleList(std::vector<Role> const& roles);

} // namespace level_dungeon
