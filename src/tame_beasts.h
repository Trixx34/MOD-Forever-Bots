// SPDX-License-Identifier: GPL-2.0-or-later
// The beasts a hunter may tame (Tame Beast), read from data/tame_beasts.txt (tools/make_tame_beasts.py), and the choice of the one to tame. Pure logic over
// Tier 2 snapshot types, unit-tested in tests/tame_test.cpp.
//
// File format, one record per line, '#' starts a comment:
//   beast <entry> <family> <level min> <level max>
// The core lets a hunter tame a creature of a tameable family whose level is at most the hunter's, when the hunter has no pet and knows Call Pet; the cast is a 20 second
// channel, and the pet is created when it ends (src/server/scripts/Spells/spell_hunter.cpp in the core).
#pragma once

#include "forever_module/tier2_session.h"

#include <cstdint>
#include <iosfwd>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace tame {

struct Beast
{
    std::uint32_t Family = 0;
    std::uint32_t LevelMin = 0;
    std::uint32_t LevelMax = 0;
};

struct TameBeasts
{
    std::map<std::uint32_t, Beast> ByEntry;
};

TameBeasts ParseTameBeasts(std::istream& in, std::vector<std::string>& warnings);
TameBeasts LoadTameBeasts(std::string const& path, std::vector<std::string>& warnings);

// The creature to tame, or nullptr: a live creature of the list within maxYards of the bot, of a level at most the bot's (the unit's own level when the snapshot has it,
// else the list's highest level for the entry). The pet takes the beast's level, so the closest level wins; then a neutral creature over a hostile one (a hostile one
// attacks the hunter during the 20 second channel), then the families hunters like (cat, wolf, boar, bear), then the closest.
forever_module::NearbyUnit const* PickTameTarget(forever_module::BotSnapshot const& bot, std::span<forever_module::NearbyUnit const> nearby,
                                                 TameBeasts const& beasts, float maxYards);

} // namespace tame
