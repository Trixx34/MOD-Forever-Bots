// SPDX-License-Identifier: GPL-2.0-or-later
// Riding training (data/riding_ranks.txt): the riding ranks a bot can learn from a stable master, and the next one due. Off by default
// (RidingTraining = 1 turns it on). Pure logic: no core, no clock, no file access. The module reads the file and passes its text to
// ParseRanks (tests/riding_training_test.cpp).
//
// A rank is due when the bot has reached its level, its riding skill is below what the rank grants and it has the copper for it.
// Ranks are tried in file order, so the cheapest rank comes first in the file.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace riding {

struct Rank
{
    std::uint32_t Spell = 0;        // the rank's spell id (taught by the stable master)
    std::uint32_t MinLevel = 0;     // the bot's level it can be learned at
    std::uint32_t Skill = 0;        // the riding skill value the rank grants (75, 150, 225, 300)
    std::uint64_t CostCopper = 0;
    std::string Name;
};

// Parses the table: one rank per line, "spell|minLevel|skill|copper|name". Blank lines and lines starting with '#' are skipped. A malformed
// line is left out and described in `errors` ("line N: ..."), so one bad row does not drop the table.
std::vector<Rank> ParseRanks(std::string_view text, std::vector<std::string>& errors);

// The first rank the bot should learn now, or nullopt when none is due.
std::optional<std::size_t> NextRank(std::vector<Rank> const& ranks, std::uint32_t level, std::uint32_t ridingSkill, std::uint64_t copper);

} // namespace riding
