// SPDX-License-Identifier: GPL-2.0-or-later
// Gathering nodes in the world (data/gather_nodes.txt): the herbs, ore veins and pools a gathering skill harvests. The gather walk (bot_policy)
// uses the nearest node of the bot's skill on its map when none is in sight, as crafting_foci does for anvils and forges.
// Pure logic: no core, no clock, no file access. The module reads the file and passes its text to ParseNodeTable (tests/gather_nodes_test.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gather {

constexpr std::uint32_t kSkillHerbalism = 182;
constexpr std::uint32_t kSkillMining = 186;
constexpr std::uint32_t kSkillSkinning = 393;
constexpr std::uint32_t kSkillFishing = 356;

struct NodePoint
{
    std::uint32_t Skill = 0;    // the gathering skill line that harvests it (Herbalism, Mining, Skinning, Fishing)
    std::uint32_t Map = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    std::uint32_t Entry = 0;    // the gameobject template
};

// Parses the table: one node per line, "skill map x y z entry". Blank lines and lines starting with '#' are skipped. A malformed line is left out and
// described in `errors` ("line N: ..."), so one bad row does not drop the table.
std::vector<NodePoint> ParseNodeTable(std::string_view text, std::vector<std::string>& errors);

// The nearest node of a skill on the bot's map (flat distance), or nullopt when the map has none. Returns the index into `nodes`.
std::optional<std::size_t> NearestNode(std::vector<NodePoint> const& nodes, std::uint32_t skill, std::uint32_t map, float x, float y);

} // namespace gather
