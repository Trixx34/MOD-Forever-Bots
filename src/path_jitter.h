// SPDX-License-Identifier: GPL-2.0-or-later
// Path jitter (config PathJitter, off by default): an out-of-combat walking move target is offset by up to PathJitterYards in a random direction, so bots
// do not walk the same line. Never within 2 yards of a point the route data marks as not walkable, never across a required route waypoint, and never in
// a dungeon (the caller passes InDungeon). The offset depends only on the seed, the bot and the target, so it does not change from tick to tick.
// Pure logic, tests/path_jitter_test.cpp (fixed seed).
#pragma once

#include <cstdint>
#include <vector>

namespace path_jitter {

struct Settings
{
    bool Enabled = false;                 // PathJitter
    float Yards = 1.0f;                   // PathJitterYards
    float KeepClearYards = 2.0f;
};

struct Pt
{
    float X = 0.0f, Y = 0.0f;
};

struct Facts
{
    bool InCombat = false;
    bool HasTarget = false;
    bool EnemyNear = false;
    bool InDungeon = false;
    bool Walking = false;                 // a plain walking move (not a flight, a follow, a cast position or a kite)
    std::vector<Pt> const* Unwalkable = nullptr;   // points the route data knows are not walkable
    std::vector<Pt> const* Required = nullptr;     // waypoints a dungeon route requires
    Pt From;                              // where the bot stands
};

// The target moved by the jitter, or the target itself when it must not be moved.
Pt Apply(Settings const& settings, Facts const& facts, Pt target, std::uint64_t seed, std::uint64_t bot);

}  // namespace path_jitter
