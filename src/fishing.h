// SPDX-License-Identifier: GPL-2.0-or-later
// Fishing (Fishing = 1, off by default): a bot with a fishing pole and the Fishing skill walks to the nearest water spot of its map
// (data/fishing_spots.txt) and fishes there. Pure logic: no core, no clock, no file access. The module reads the file and passes its text to
// ParseSpots (tests/fishing_test.cpp).
//
// The cast itself (the Fishing spell at a pool) needs a core action that targets a point on the water; the core's current action set has no
// such action, so this module picks spots and decides when fishing is due, and the policy hook stays behind the Fishing key. See the report
// in doc/fishing.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fishing {

constexpr std::uint32_t kSkillFishing = 356;

struct Spot
{
    std::uint32_t Map = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    float Radius = 0.0f;    // yards from the spot where the bot may fish
    std::string Name;
};

// Parses the table: one spot per line, "map|x|y|z|radius|name". Blank lines and lines starting with '#' are skipped. A malformed line is
// left out and described in `errors` ("line N: ..."), so one bad row does not drop the table.
std::vector<Spot> ParseSpots(std::string_view text, std::vector<std::string>& errors);

// The nearest spot on the bot's map (flat distance), or nullopt when the map has none. Returns the index into `spots`.
std::optional<std::size_t> NearestSpot(std::vector<Spot> const& spots, std::uint32_t map, float x, float y);

// Whether fishing is due: the bot has a pole, the Fishing skill is below the level cap (5 per level, as the client caps it) and the bot is
// at least level 5 (the lowest level a pole is useful).
bool FishingDue(std::uint32_t level, std::uint32_t fishingSkill, bool hasPole);

} // namespace fishing
