// SPDX-License-Identifier: GPL-2.0-or-later
// Level dungeons: which dungeon a levelling bot party is due to run, and the LFG planning. Pure logic, no core, unit-tested in tests/level_dungeon_test.cpp;
// BotPolicy (bot_policy.cpp) drives it and reuses the "tank lead" route walk for the run itself.
//
// Schedule: a party runs a dungeon at the HIGH end of its level range. The party's level (its lowest member) must have reached the dungeon's max level (and not be more
// than kGraceLevels above it: a party that levelled past a dungeon without being able to go does not catch up on it). A dungeon is run at most `maxRuns` times by one party
// (config LevelDungeonRuns), at most once per level: a second attempt, after a first that did not clear it, waits for the next level. A cleared dungeon is done.
// Optional rules (DueRules, all off by default = the schedule above): LevelDungeonEarlyLevels lets a party go up to N levels below the max level (never below the
// min level); LevelDungeonSides gives a dungeon a side so a party of the other side is never sent there.
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
    std::uint8_t Side = 0;            // 0 = both sides, 1 = Alliance only, 2 = Horde only (LevelDungeonSides)
    std::uint32_t DurationSec = 0;    // the dungeon file's run.duration_sec; 0 = not known
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

// Optional schedule rules. Defaults keep Due() above unchanged.
struct DueRules
{
    std::uint32_t EarlyLevels = 0;   // LevelDungeonEarlyLevels: due from (max level - this), never below the min level; 0 = only from the max level
    std::uint8_t PartySide = 0;      // the party's side (1 Alliance, 2 Horde; 0 unknown): a dungeon with a side is due only for that side
};
// Why no dungeon is due (the log reason of level_dungeon_skip). Ordered by how close the party came: the highest one met is reported.
enum class Skip : std::uint8_t { None, NoDungeon, WrongSide, Cleared, RunsUsed, SameLevel };
char const* SkipName(Skip why);   // "none", "no_dungeon", "wrong_side", "cleared", "runs_used", "same_level"
struct DueResult
{
    int Index = -1;                  // as Due()
    Skip Why = Skip::NoDungeon;      // Skip::None when Index >= 0
    bool Early = false;              // due below the dungeon's max level (only with EarlyLevels)
};
DueResult DueWhy(std::vector<Dungeon> const& dungeons, std::uint32_t partyLevel, Book const& book, std::uint32_t maxRuns, DueRules const& rules);
int Due(std::vector<Dungeon> const& dungeons, std::uint32_t partyLevel, Book const& book, std::uint32_t maxRuns, DueRules const& rules);
// The lowest party level at which `dungeon` is due: its max level minus `earlyLevels`, never below its min level.
std::uint32_t FirstDueLevel(Dungeon const& dungeon, std::uint32_t earlyLevels);

// LevelDungeonSides: "rfc:horde,hot:alliance,dm:1" -> dungeon id (lower case) -> side (1 Alliance, 2 Horde, 0 both). A side is alliance/a/1, horde/h/2 or any/both/0
// (case-insensitive). A bad item is skipped with a line in `warnings`.
std::map<std::string, std::uint8_t> ParseSides(std::string_view text, std::vector<std::string>& warnings);

// The run time cap of a level dungeon: the dungeon file's duration_sec when `fromFile` (LevelDungeonFileDuration) and the file gives one, else `fallbackMs`.
std::uint64_t RunCapMs(std::uint32_t durationSec, bool fromFile, std::uint64_t fallbackMs);
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
// LevelDungeonNeedRoles: whether a party with these roles has at least one tank and one healer.
bool HasTankAndHealer(std::vector<Role> const& roles);
// "tank, dps" (the roles of the list, a count after a role that repeats: "dps x2").
std::string RoleList(std::vector<Role> const& roles);

} // namespace level_dungeon
