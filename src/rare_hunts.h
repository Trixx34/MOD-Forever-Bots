// SPDX-License-Identifier: GPL-2.0-or-later
// Rare hunts (RareHunts = 1, off by default): a list of rare creatures (data/rare_hunts.txt), each with the place it spawns and the levels
// it suits. A bot in range of its level walks to the nearest one and fights it. The kill target goes through the same fight filter the
// hunt quests use (the policy fights only the listed entries; see HuntEntries). Pure logic: no core, no clock, no file access.
// tests/rare_hunts_test.cpp covers the rules.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rare_hunts {

// How far a bot's level may be from a hunt's levels and still take it (below the minimum or above the maximum by this much).
constexpr std::uint32_t kLevelSlack = 3;

struct Hunt
{
    std::uint32_t Entry = 0;        // the creature template
    std::string Name;
    std::uint32_t Map = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    std::uint32_t MinLevel = 0;
    std::uint32_t MaxLevel = 0;
    float Radius = 0.0f;            // yards around the spawn where the bot searches
};

// Parses the table: one hunt per line, "entry|name|map|x|y|z|minLevel|maxLevel|radius". Blank lines and lines starting with '#' are skipped.
// A malformed line is left out and described in `errors` ("line N: ..."), so one bad row does not drop the table.
std::vector<Hunt> ParseHunts(std::string_view text, std::vector<std::string>& errors);

// The nearest hunt on the bot's map that suits its level (within kLevelSlack of the hunt's levels) and is not in `done` (entries the bot
// already killed or gave up). Returns the index into `hunts`, or nullopt.
std::optional<std::size_t> ChooseHunt(std::vector<Hunt> const& hunts, std::uint32_t map, float x, float y, std::uint32_t level,
                                      std::vector<std::uint32_t> const& done);

// The creature entries of every hunt: the fight filter's allowed targets while a hunt is on.
std::vector<std::uint32_t> HuntEntries(std::vector<Hunt> const& hunts);

} // namespace rare_hunts
