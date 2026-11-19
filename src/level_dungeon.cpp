// SPDX-License-Identifier: GPL-2.0-or-later
#include "level_dungeon.h"

#include <algorithm>
#include <cctype>

namespace level_dungeon {

namespace {

std::string Lower(std::string_view text)
{
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

std::vector<std::string> NameWords(std::string_view text)
{
    std::vector<std::string> words;
    std::string current;
    for (char c : text)
    {
        if (std::isalnum(static_cast<unsigned char>(c)))
            current.push_back(char(std::tolower(static_cast<unsigned char>(c))));
        else if (!current.empty())
            words.push_back(std::move(current)), current.clear();
    }
    if (!current.empty())
        words.push_back(std::move(current));
    return words;
}

} // namespace

std::uint32_t ClampRuns(std::uint32_t runs)
{
    return std::clamp(runs, kMinRuns, kMaxRunsLimit);
}

std::uint32_t FirstDueLevel(Dungeon const& dungeon, std::uint32_t earlyLevels)
{
    std::uint32_t const early = dungeon.MaxLevel > earlyLevels ? dungeon.MaxLevel - earlyLevels : 0;
    return std::max(early, dungeon.MinLevel);
}

DueResult DueWhy(std::vector<Dungeon> const& dungeons, std::uint32_t partyLevel, Book const& book, std::uint32_t maxRuns, DueRules const& rules)
{
    maxRuns = ClampRuns(maxRuns);
    DueResult result;
    int best = -1;
    for (std::size_t i = 0; i < dungeons.size(); ++i)
    {
        Dungeon const& dungeon = dungeons[i];
        if (partyLevel < FirstDueLevel(dungeon, rules.EarlyLevels) || partyLevel > dungeon.MaxLevel + kGraceLevels)
            continue;
        if (dungeon.Side != 0 && dungeon.Side != rules.PartySide)
        {
            result.Why = std::max(result.Why, Skip::WrongSide);
            continue;
        }
        auto const seen = book.Dungeons.find(dungeon.Id);
        if (seen != book.Dungeons.end())
        {
            Book::Record const& record = seen->second;
            if (record.Cleared)
            {
                result.Why = std::max(result.Why, Skip::Cleared);
                continue;
            }
            if (record.Runs >= maxRuns)
            {
                result.Why = std::max(result.Why, Skip::RunsUsed);
                continue;
            }
            if (partyLevel <= record.LastLevel)
            {
                result.Why = std::max(result.Why, Skip::SameLevel);
                continue;
            }
        }
        if (best < 0 || dungeon.MaxLevel < dungeons[std::size_t(best)].MaxLevel ||
            (dungeon.MaxLevel == dungeons[std::size_t(best)].MaxLevel && dungeon.Id < dungeons[std::size_t(best)].Id))
            best = int(i);
    }
    if (best >= 0)
    {
        result.Index = best;
        result.Why = Skip::None;
        result.Early = partyLevel < dungeons[std::size_t(best)].MaxLevel;
    }
    return result;
}

int Due(std::vector<Dungeon> const& dungeons, std::uint32_t partyLevel, Book const& book, std::uint32_t maxRuns, DueRules const& rules)
{
    return DueWhy(dungeons, partyLevel, book, maxRuns, rules).Index;
}

int Due(std::vector<Dungeon> const& dungeons, std::uint32_t partyLevel, Book const& book, std::uint32_t maxRuns)
{
    return Due(dungeons, partyLevel, book, maxRuns, DueRules{});
}

char const* SkipName(Skip why)
{
    switch (why)
    {
        case Skip::None: return "none";
        case Skip::NoDungeon: return "no_dungeon";
        case Skip::WrongSide: return "wrong_side";
        case Skip::Cleared: return "cleared";
        case Skip::RunsUsed: return "runs_used";
        case Skip::SameLevel: return "same_level";
    }
    return "none";
}

std::map<std::string, std::uint8_t> ParseSides(std::string_view text, std::vector<std::string>& warnings)
{
    std::map<std::string, std::uint8_t> sides;
    std::string item;
    auto const take = [&](std::string const& raw)
    {
        std::string entry;
        for (char c : raw)
            if (!std::isspace(static_cast<unsigned char>(c)))
                entry.push_back(char(std::tolower(static_cast<unsigned char>(c))));
        if (entry.empty())
            return;
        std::size_t const colon = entry.find(':');
        std::string const id = colon == std::string::npos ? std::string() : entry.substr(0, colon);
        std::string const side = colon == std::string::npos ? std::string() : entry.substr(colon + 1);
        std::uint8_t value = 0;
        bool ok = !id.empty();
        if (side == "alliance" || side == "a" || side == "1")
            value = 1;
        else if (side == "horde" || side == "h" || side == "2")
            value = 2;
        else if (side == "any" || side == "both" || side == "0")
            value = 0;
        else
            ok = false;
        if (!ok)
        {
            warnings.push_back("LevelDungeonSides: '" + raw + "' is not '<dungeon id>:alliance|horde|any'; skipped");
            return;
        }
        sides[id] = value;
    };
    for (char c : text)
    {
        if (c == ',')
            take(item), item.clear();
        else
            item.push_back(c);
    }
    take(item);
    return sides;
}

std::uint64_t RunCapMs(std::uint32_t durationSec, bool fromFile, std::uint64_t fallbackMs)
{
    return fromFile && durationSec > 0 ? std::uint64_t(durationSec) * 1000 : fallbackMs;
}

bool HasTankAndHealer(std::vector<Role> const& roles)
{
    return std::find(roles.begin(), roles.end(), Role::Tank) != roles.end() && std::find(roles.begin(), roles.end(), Role::Healer) != roles.end();
}

void NoteStarted(Book& book, std::string const& id, std::uint32_t partyLevel)
{
    Book::Record& record = book.Dungeons[id];
    ++record.Runs;
    record.LastLevel = partyLevel;
}

void NoteCleared(Book& book, std::string const& id)
{
    book.Dungeons[id].Cleared = true;
}

int FindDungeon(std::vector<Dungeon> const& dungeons, std::string_view text)
{
    std::string const wanted = Lower(text);
    std::vector<std::string> const words = NameWords(wanted);
    if (words.empty())
        return -1;
    for (std::size_t i = 0; i < dungeons.size(); ++i)
        if (Lower(dungeons[i].Id) == wanted)
            return int(i);
    int found = -1;
    for (std::size_t i = 0; i < dungeons.size(); ++i)
    {
        std::vector<std::string> const name = NameWords(dungeons[i].Name);
        bool match = false;
        for (std::string const& word : words)
            if (word.size() >= 3 && std::find(name.begin(), name.end(), word) != name.end())
                match = true;
        if (!match)
            continue;
        if (found >= 0)
            return -1; // ambiguous
        found = int(i);
    }
    return found;
}

std::string KeyList(std::vector<Dungeon> const& dungeons)
{
    std::vector<std::string> keys;
    for (Dungeon const& dungeon : dungeons)
        keys.push_back(dungeon.Id);
    std::sort(keys.begin(), keys.end());
    std::string out;
    for (std::string const& key : keys)
        out += (out.empty() ? "" : ", ") + key;
    return out;
}

char const* RoleName(Role role)
{
    switch (role)
    {
        case Role::Tank: return "tank";
        case Role::Healer: return "healer";
        case Role::Dps: return "dps";
    }
    return "dps";
}

Role RoleOfClass(std::uint32_t classId)
{
    switch (classId)
    {
        case 1: return Role::Tank;                                   // warrior
        case 2: case 5: case 7: case 11: return Role::Healer;       // paladin, priest, shaman, druid
        default: return Role::Dps;                                   // hunter, rogue, mage, warlock
    }
}

std::vector<Role> MissingRoles(std::uint32_t size, std::uint32_t tanks, std::uint32_t healers)
{
    std::vector<Role> roles;
    std::uint32_t room = size >= kPartyMax ? 0 : kPartyMax - size;
    if (room > 0 && tanks == 0)
        roles.push_back(Role::Tank), --room;
    if (room > 0 && healers == 0)
        roles.push_back(Role::Healer), --room;
    while (room-- > 0)
        roles.push_back(Role::Dps);
    return roles;
}

std::string RoleList(std::vector<Role> const& roles)
{
    std::string out;
    for (Role role : {Role::Tank, Role::Healer, Role::Dps})
    {
        auto const count = std::count(roles.begin(), roles.end(), role);
        if (count == 0)
            continue;
        out += (out.empty() ? "" : ", ") + std::string(RoleName(role));
        if (count > 1)
            out += " x" + std::to_string(count);
    }
    return out;
}

} // namespace level_dungeon
