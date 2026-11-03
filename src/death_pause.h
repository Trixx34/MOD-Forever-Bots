// SPDX-License-Identifier: GPL-2.0-or-later
// Death realism (config DeathPause, off by default): a dead bot waits a random DeathPauseMinSec..DeathPauseMaxSec after the death before it sends
// release spirit, as a player would take a moment. The wait is per bot and counts from the death. Nothing else of the corpse run changes.
// Pure logic, tests/death_pause_test.cpp.
#pragma once

#include <cstdint>

namespace death_pause {

struct Settings
{
    bool Enabled = false;                 // DeathPause
    std::uint32_t MinSec = 10;            // DeathPauseMinSec
    std::uint32_t MaxSec = 30;            // DeathPauseMaxSec
};

// The pause of one death, in ms, within [MinSec, MaxSec]; the same for the same (seed, bot, death time).
std::uint64_t PauseMs(Settings const& settings, std::uint64_t seed, std::uint64_t bot, std::uint64_t deathMs);

// True when the release may go out: the feature is off, or the pause since the death has passed.
bool ReleaseAllowed(Settings const& settings, std::uint64_t seed, std::uint64_t bot, std::uint64_t deathMs, std::uint64_t nowMs);

}  // namespace death_pause
