// SPDX-License-Identifier: GPL-2.0-or-later
// City greeting (config CityGreet, off by default): an idle bot in a capital that sees a player within 12 yards now and then says a line to the player in
// /say; a bot standing at the auction house (the auctioneer within 10 yards) uses the auction line pool instead. Pure logic, unit-tested in
// tests/city_greet_test.cpp. Limits: one bot per player per 10 minutes, CityGreetZoneMax lines per map per minute.
#pragma once

#include "realism.h"

#include <cstdint>
#include <map>

namespace city_greet {

struct Settings
{
    bool Enabled = false;                 // CityGreet
    std::uint32_t ChancePct = 5;          // CityGreetChancePct
    std::uint32_t ZoneMax = 2;            // CityGreetZoneMax: lines a minute over all bots of one map
    float RangeYards = 12.0f;
    std::uint64_t PlayerCooldownMs = 10ull * 60 * 1000;
};

struct Facts
{
    bool Idle = false;
    bool InCapital = false;
    float Distance = 1e9f;                // to the player
};

class Greeter
{
public:
    // True when this bot may greet this player now. Records the use of the limits only when it returns true. `draw` receives a random number for the line.
    bool Try(Settings const& settings, Facts const& facts, std::uint64_t seed, std::uint64_t bot, std::uint64_t player, std::uint32_t map, std::uint64_t nowMs,
             std::uint32_t& draw);

private:
    std::map<std::uint64_t, std::uint64_t> _playerMs;
    realism::ZoneRate _zone;
    std::uint32_t _counter = 0;
};

}  // namespace city_greet
