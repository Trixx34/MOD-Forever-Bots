// SPDX-License-Identifier: GPL-2.0-or-later
// Grind spots (leveling 1 to 60): places where creatures of a given level range live, so an idle bot walks to the ones that suit its level instead
// of waiting at its start. Read from a text file; the file is made from the world database (tools/make_grind_spots.py, doc 20). Pure data and
// parsing, unit-tested in tests/grind_test.cpp.
//
// File format, one record per line, '#' starts a comment:
//   spot <map> <x> <y> <z> <minLevel> <maxLevel> <side> [weight]
//     side: 0 both factions, 1 Alliance, 2 Horde. weight: how many creatures the spot was made from (default 1), used to prefer richer spots.
#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

struct GrindSpot
{
    std::uint32_t Map = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    std::uint32_t MinLevel = 1;
    std::uint32_t MaxLevel = 60;
    std::uint8_t Side = 0;        // 0 both, 1 Alliance, 2 Horde
    std::uint32_t Weight = 1;
};

struct GrindData
{
    std::vector<GrindSpot> Spots;
};

GrindData ParseGrindData(std::istream& in, std::vector<std::string>& warnings);
GrindData LoadGrindData(std::string const& path, std::vector<std::string>& warnings);

// The spots a bot of `level` and `side` (1 Alliance, 2 Horde, 0 unknown: both) can use on `map`, as indexes into data.Spots, nearest to (x, y) first.
// Spots whose level range holds the level come first-class; with none, the spots with the highest top level below the bot's level (an over-leveled
// bot grinds the best there is). `skip` lists spots left out (the ones just visited) unless that would leave nothing.
std::vector<std::size_t> GrindSpotsFor(GrindData const& data, std::uint32_t map, std::uint32_t level, std::uint8_t side, float x, float y,
                                       std::vector<std::size_t> const& skip = {});

// Alliance (1) or Horde (2) from a race id, 0 for an unknown race.
std::uint8_t SideOfRace(std::uint32_t race);
