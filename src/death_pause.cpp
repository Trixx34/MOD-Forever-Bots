// SPDX-License-Identifier: GPL-2.0-or-later
#include "death_pause.h"

#include "bot_random.h"

namespace death_pause {

std::uint64_t PauseMs(Settings const& settings, std::uint64_t seed, std::uint64_t bot, std::uint64_t deathMs)
{
    std::uint64_t const lo = std::uint64_t(settings.MinSec) * 1000;
    std::uint64_t hi = std::uint64_t(settings.MaxSec) * 1000;
    if (hi < lo)
        hi = lo;
    return lo + bot_random::Draw(seed, bot, 0xB1, deathMs) % (hi - lo + 1);
}

bool ReleaseAllowed(Settings const& settings, std::uint64_t seed, std::uint64_t bot, std::uint64_t deathMs, std::uint64_t nowMs)
{
    return !settings.Enabled || nowMs - deathMs >= PauseMs(settings, seed, bot, deathMs);
}

}  // namespace death_pause
