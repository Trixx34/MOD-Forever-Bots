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

int Due(std::vector<Dungeon> const& dungeons, std::uint32_t partyLevel, Book const& book, std::uint32_t maxRuns)
{
    maxRuns = ClampRuns(maxRuns);
    int best = -1;
    for (std::size_t i = 0; i < dungeons.size(); ++i)
    {
        Dungeon const& dungeon = dungeons[i];
        if (partyLevel < dungeon.MaxLevel || partyLevel > dungeon.MaxLevel + kGraceLevels || partyLevel < dungeon.MinLevel)
            continue;
        auto const seen = book.Dungeons.find(dungeon.Id);
        if (seen != book.Dungeons.end())
        {
            Book::Record const& record = seen->second;
            if (record.Cleared || record.Runs >= maxRuns || partyLevel <= record.LastLevel)
                continue;
        }
        if (best < 0 || dungeon.MaxLevel < dungeons[std::size_t(best)].MaxLevel ||
            (dungeon.MaxLevel == dungeons[std::size_t(best)].MaxLevel && dungeon.Id < dungeons[std::size_t(best)].Id))
            best = int(i);
    }
    return best;
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
