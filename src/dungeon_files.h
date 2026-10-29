// Per-dungeon strategy files (doc/dungeon-strategy.md): one JSON file per dungeon, data/dungeons/<map id>-<name>.json, holding the entrance, the route, the events
// (doors, levers) and a block per boss. This library reads them, checks them, and lets a folder of your own override the shipped ones by map id. No SDK
// dependency; the pure checks are tested in tests/dungeon_files_test.cpp.
//
// Where dungeons come from, by priority for one map:
//   1. DungeonDir       your own folder of JSON files (absolute path): a file for a map REPLACES the shipped one
//   2. DungeonDataDir   the shipped JSON files (default: the folder "dungeons" beside DungeonFile, so data/dungeons for data/dungeons.txt)
//   3. DungeonFile      the old text file (data/dungeons.txt) and DungeonEventsFile (data/dungeon_events.txt): a dungeon is read from them only when no JSON
//                       file has its map
// A JSON file with a problem is not used: the problem is logged with the file name, and the next place down the list supplies that map.
#pragma once

#include "dungeon_data.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

constexpr int kDungeonSchema = 1;

// Parses one JSON dungeon file. On a problem returns false with the reason in `error` (the dungeon is not usable). Things that are only odd (an unknown
// key) are lines in `warnings`, and the file is used. `fileMap` is the map id in the file's name (36 for 36-the-deadmines.json): when given, the file's own
// map must be the same. `source` goes into the messages and into Dungeon::Source.
// Contract note: internally this unwinds via a private exception (dungeon_files.cpp's FileError), caught before returning here, so a failure is always
// reported through the bool/`error` return (never an exception escaping this function) like every other parser in the codebase.
bool ParseDungeonJson(std::string_view text, std::string const& source, std::optional<std::uint32_t> fileMap, Dungeon& out, std::string& error,
                      std::vector<std::string>& warnings);

// Reads every <map id>-<name>.json of a folder, in file-name order. A bad file is a warning and is left out. A missing folder is a warning only when
// `required`. Two files for one map: the first is kept.
std::vector<Dungeon> LoadDungeonDir(std::string const& dir, bool required, std::vector<std::string>& warnings);

struct DungeonSources
{
    std::string File;        // DungeonFile: the old text file
    std::string EventsFile;  // DungeonEventsFile: the old events file
    std::string DataDir;     // DungeonDataDir: the shipped JSON folder ("" = beside File, in "dungeons")
    std::string OverrideDir; // DungeonDir: your own JSON folder
};

DungeonSources SourcesFromConfig(std::unordered_map<std::string, std::string> const& config);

// The folder of the shipped JSON files: DataDir, else "dungeons" beside File, else "". The second form is not an error when it does not exist.
std::string ShippedDir(DungeonSources const& sources);

struct DungeonSet
{
    std::vector<Dungeon> Dungeons;                 // one per map; each has its own events
    std::vector<dungeon_events::Event> Events;     // every event in force (all dungeons' events, plus those of old-file maps)
    std::vector<std::string> Log;                  // what came from where, one line each, for the module log
};

// The dungeons in force, from the places above. Problems are lines in `warnings`.
DungeonSet LoadDungeonSet(DungeonSources const& sources, std::vector<std::string>& warnings);
