// SPDX-License-Identifier: GPL-2.0-or-later
// Quest data (M5): the starter quests a bot can do, per start, read from a text file. Pure data and parsing, unit-tested in
// tests/quest_test.cpp.
//
// File format, one record per line, '#' starts a comment:
//   start <name>                       a new start (a race's first zone); the records below belong to it, in the order bots do them
//   anchor <map> <x> <y> <z>           where the start is; a bot belongs to the nearest anchor on its map
//   quest <id> giver <x> <y> <z> ender <x> <y> <z>
//   hunt <quest> <position> <need> <entry>[,<entry>...]
//                                      objective number <position> (0 = the quest's first objective, in the order of the log
//                                      snapshot) needs <need> kills of, or items dropped by, these creature entries
//   area <quest> <x> <y> <z>           a place to look for the quest's targets (repeat for several)
//   reward <quest>                     a class quest: the bot completes it with its reward and does no objectives (the GM command
//                                      `.quest reward` does the same); its giver and ender are still walked to
//   startfrom <quest> item <entry>     the quest starts from a quest-starting item the bot carries (no giver NPC): the accept is kQuest operation 5 and
//                                      needs no walk, it is tried where the bot stands (the core refuses it, reason no-giver, when the bot has no such item)
//   startfrom <quest> object <entry>   the quest starts from a game object of that entry (no creature giver): the bot walks to the quest's 'giver' point,
//                                      which must then be the place of the object, and the accept is operation 5 with the object entry
//   req <quest> [minlevel <n>] [level <n>] [prev <quest>] [races <mask>] [classes <mask>]
//                                      who may do the quest (optional; for the whole-world quest files): the least level to accept it, the quest's own
//                                      level (a bot well above it skips the quest), the quest that must be handed in before it (tried later, not
//                                      refused), and the races and classes that may take it as bit masks (bit n-1 for race or class id n; 0 = any), and
//                                      'group <n>': the quest is meant for a party of n (the planner takes it only with config QuestGroups on)
#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

struct QuestPoint
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

struct QuestObjectiveDef
{
    std::uint32_t Position = 0;               // index of the objective in the quest log entry (QuestEntry::counts)
    std::uint32_t Need = 0;
    std::vector<std::uint32_t> Entries;       // creature entries that count for it
};

struct QuestDef
{
    std::uint32_t Id = 0;
    QuestPoint Giver;
    QuestPoint Ender;
    std::vector<QuestObjectiveDef> Objectives;
    std::vector<QuestPoint> Areas;
    std::uint32_t MinLevel = 0;               // 0 = no limit
    std::uint32_t QuestLevel = 0;             // 0 = unknown
    std::uint32_t PrevQuest = 0;              // 0 = none
    std::uint32_t RaceMask = 0;               // 0 = any race
    std::uint32_t ClassMask = 0;              // 0 = any class
    std::uint32_t Group = 0;                  // the quest's suggested group size (a 'group <n>' pair of the req record): 2 or more = the bot needs a party of that many (config QuestGroups)
    std::uint32_t StartItem = 0;              // the quest starts from this quest-starting item (startfrom <quest> item): operation 5, no walk
    std::uint32_t StartObject = 0;            // the quest starts from the game object of this entry (startfrom <quest> object): operation 5 at the giver point
    bool Reward = false;                      // a class quest: completed with its reward, no objectives
};

struct QuestStartDef
{
    std::string Name;
    std::uint32_t Map = 0;
    QuestPoint Anchor;
    bool HasAnchor = false;
    std::vector<QuestDef> Quests;
};

struct QuestData
{
    std::vector<QuestStartDef> Starts;
};

// Parses the text. A line that does not fit is dropped with a line in `warnings`.
QuestData ParseQuestData(std::istream& in, std::vector<std::string>& warnings);

// Reads the file at `path`; an unreadable file gives empty data and a warning.
QuestData LoadQuestData(std::string const& path, std::vector<std::string>& warnings);
