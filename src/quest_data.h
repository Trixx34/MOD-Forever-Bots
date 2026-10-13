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
