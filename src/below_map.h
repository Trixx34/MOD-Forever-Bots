// SPDX-License-Identifier: GPL-2.0-or-later
// Below-map recovery (2026-11-18, doc/level-safe.md; config BelowMapRecover, default off): a bot that fell through the ground stands far below the places around
// it and every walk it is given is refused for no route (the 2026-11-18 sim: 30k of 101k no_route events were walks under 100 yards; a bot at the Ironforge
// auction house at z=19 with the auctioneers at z=505, one near the Skyborne auctioneer at z=9 with the ground at about 666). Two signs, either one:
//   - z_below: the nearest known place of its map (off_map::KnownWorld: quest givers and anchors, grind spots, hubs) within NearYards lies more than DropYards
//     above the bot, for BelowForMs without a break;
//   - no_route_storm: StormCount no-route answers for short walks (6 to TinyYards yards) within StormWindowMs.
// The bot is then teleported to the nearest known place (the same kTeleport as the off-map recovery), at most once per CooldownMs, with an event line.
// Not dead, in combat (the core refuses a teleport then), on a flight or transport, or held by the policy (a scenario, a dungeon route).
//
// Pure logic, unit-tested in tests/level_safe_test.cpp.
#pragma once

#include <cstdint>
#include <vector>

namespace below_map
{

struct Options
{
    bool Enabled = false;                 // off here and in the module settings (BelowMapRecover = 0): the sim first
    float DropYards = 60.0f;              // the nearest known place lies this much higher than the bot
    float NearYards = 60.0f;              // ... and at most this far away (2D)
    std::uint64_t BelowForMs = 20000;     // ... for this long without a break
    std::uint32_t StormCount = 8;         // this many short no-route answers ...
    std::uint64_t StormWindowMs = 300000; // ... within this long
    float TinyYards = 100.0f;             // a short walk: under this many yards to its destination (and 6 or more: a shorter one is an arrival)
    std::uint64_t CooldownMs = 600000;    // no second try for the same bot before this
};

struct Facts
{
    bool Alive = true;
    bool InCombat = false;
    bool Travelling = false;
    bool Held = false;
    bool HasGround = false;   // a known place lies within NearYards
    float Z = 0.0f;           // the bot's height
    float GroundZ = 0.0f;     // that place's height
};

struct Watch
{
    std::uint64_t BelowSinceMs = 0;        // when the bot was first seen below in this spell (0 = not below)
    std::uint64_t NextTryMs = 0;
    std::vector<std::uint64_t> NoRoutes;   // when the recent short no-route answers came (bounded)
};

enum class Verdict : std::uint8_t
{
    kFine,       // nothing wrong
    kWaiting,    // below the ground, not long enough yet
    kBlocked,    // a sign, but dead, fighting, travelling or held
    kCooldown,   // a sign, and the cooldown runs
    kSendBelow,  // teleport now: z_below
    kSendStorm,  // teleport now: no_route_storm
};

// A no-route answer for a walk that had `toDestYards` left.
void NoteNoRoute(Options const& options, Watch& watch, float toDestYards, std::uint64_t nowMs);
Verdict Check(Options const& options, Watch& watch, Facts const& facts, std::uint64_t nowMs);
char const* VerdictName(Verdict verdict);

} // namespace below_map
