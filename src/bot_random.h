// SPDX-License-Identifier: GPL-2.0-or-later
// Per-bot randomness for the policy. Every bot has a bit of it: which bots form parties and how big the parties are, how long a bot idles before it
// walks, which grind spot it picks and where exactly it stands there. Without it all the bots follow the same rules in the same order and move as one
// crowd (the parties were cut from the guid order, every bot walked to the nearest spot after exactly 30 s).
//
// The values are pseudo-random but not random: they are mixed from the run's seed (RandomSeed), the bot, what the value is for (a salt) and a counter,
// so a run with the same seed behaves the same way and the tests can check it. Nothing here keeps state.
#pragma once

#include <cstdint>

namespace bot_random
{

// What a draw is for. Each use has its own salt, so two uses on one bot are not tied together.
enum Salt : std::uint32_t
{
    kSaltParty = 1,        // does this bot take part in the parties
    kSaltPartySize = 2,    // how big a party is
    kSaltIdle = 3,         // how long a bot idles before it walks
    kSaltGrindPick = 4,    // which of the best grind spots it takes
    kSaltJitter = 5,       // where around a spot it stands
    kSaltRest = 6,         // the health at which it sits down to rest
};

// SplitMix64: a well mixed 64 bit value from one 64 bit input.
inline std::uint64_t Mix(std::uint64_t value)
{
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

inline std::uint64_t Draw(std::uint64_t seed, std::uint64_t bot, std::uint32_t salt, std::uint64_t counter = 0)
{
    return Mix(Mix(Mix(seed ^ 0xA5A5A5A5A5A5A5A5ull) + bot) + (std::uint64_t(salt) << 32 | (counter & 0xFFFFFFFFull)) + (counter >> 32) * 0x9E3779B1ull);
}

// A whole number below `limit` (0 when limit is 0).
inline std::uint32_t Below(std::uint64_t draw, std::uint32_t limit)
{
    return limit == 0 ? 0u : std::uint32_t(draw % limit);
}

// A number in [0, 1).
inline double Unit(std::uint64_t draw)
{
    return double(draw >> 11) * (1.0 / 9007199254740992.0);
}

// `base` varied by up to +-pct percent (pct 0 gives base back unchanged).
inline std::uint64_t Vary(std::uint64_t base, std::uint32_t pct, std::uint64_t draw)
{
    if (pct == 0 || base == 0)
        return base;
    double const factor = 1.0 + (Unit(draw) * 2.0 - 1.0) * double(pct) / 100.0;
    double const value = double(base) * factor;
    return value < 1.0 ? 1u : std::uint64_t(value);
}

} // namespace bot_random
