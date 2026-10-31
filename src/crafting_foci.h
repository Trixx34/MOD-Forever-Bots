// SPDX-License-Identifier: GPL-2.0-or-later
// Anvils, forges and cooking fires in the capitals (data/crafting_foci.txt): where a bot must stand to craft a recipe that asks for a spell focus.
// Pure logic: no core, no clock, no file access. The module reads the file and passes its text to ParseFociTable (tests/crafting_foci_test.cpp).
//
// A recipe's spell focus (Spell.RequiresSpellFocus) is met when a gameobject of type 8 with that focus id stands within its radius of the crafter. In this
// client: 1 anvil (Blacksmithing, Engineering), 3 forge (smelting, Mining), 4 cooking fire (Cooking). The core already enforces it (cast result 123);
// the tools of the old trades (hammer, pick, knife, rod) are not in this server's data (docs/98-profession-tools.md), so nothing here asks for them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace crafting {

constexpr std::uint32_t kFocusAnvil = 1;
constexpr std::uint32_t kFocusForge = 3;
constexpr std::uint32_t kFocusCookingFire = 4;

struct FocusPoint
{
    std::string City;           // as the errand tables spell it ("Stormwind", "ThunderBluff", ...)
    std::uint32_t Map = 0;
    std::uint32_t Focus = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    std::uint32_t Radius = 0;   // yards the crafter must stand within
    std::uint32_t Entry = 0;    // the gameobject template
    std::string Name;
};

// The focus a profession's recipes mostly need, 0 for a skill whose recipes need none (Tailoring, Enchanting, Alchemy, First Aid, ...):
// Blacksmithing and Engineering 1, Mining (smelting) 3, Cooking 4. Skill line ids as in BotProfessions.
std::uint32_t FocusOfSkill(std::uint32_t skill);

// Parses the table: one object per line, "city|map|focus|x y z|radius|entry|name". Blank lines and lines starting with '#' are skipped. A malformed line is
// left out and described in `errors` ("line N: ..."), so one bad row does not drop the table.
std::vector<FocusPoint> ParseFociTable(std::string_view text, std::vector<std::string>& errors);

// The nearest object of a focus on the bot's map (flat distance), or nullopt when the map has none. Returns the index into `points`.
std::optional<std::size_t> NearestFocus(std::vector<FocusPoint> const& points, std::uint32_t focus, std::uint32_t map, float x, float y);

// Whether the bot stands within the object's radius (flat distance, the radius the server checks) and so can craft at it.
bool InReach(FocusPoint const& point, std::uint32_t map, float x, float y);

} // namespace crafting
