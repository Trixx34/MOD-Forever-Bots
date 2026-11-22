// SPDX-License-Identifier: GPL-2.0-or-later
// No-route backoff (2026-11-20, doc/belowmap-noroute-20261120.md; config NoRouteBackoff, default off; NoRouteBackoffSec): a per-(bot, destination)
// blacklist for walks the core answered with "no route".
//
// Arm C (2026-11-19) had 91,234 no_route events in 7.9 h, about 29 per bot-hour on every bot, unchanged by the earlier fixes. Those fixes (the policy's
// NoRouteUntilMs and the pump's move_gate) remember one place per bot only: the pause (5 s, doubling) grows only while the same place is refused again
// in a row, so a bot that goes round two or three places it cannot reach (a grind spot in rock, a hub on another floor, an explore point off the navmesh)
// starts at 5 s for each of them, every time. A refused walk is not free either: the core still moves the bot (the nearest reachable point, or a straight
// 3D line through the terrain when the navmesh has no path at all), which is also what put bots "under the map" for the below-map recovery.
//
// Here every refused point walk is remembered per bot (up to MaxEntries, on its map, SameYards wide): it is not sent again before BaseMs, then 2x, 4x
// ... up to MaxMs while it keeps being refused; an applied walk to it clears it; an entry not refused for ForgetMs is dropped. The policy holds the walk
// back (no core call, so no straight-line walk and no no_route line) and ends the walk as a refusal would (WalkBlockedAtMs), so the caller picks
// something else. Not for walks to a unit, in combat, while dead, or on a dungeon route (each has its own handling).
//
// Pure logic, unit-tested in tests/belowmap_noroute_test.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace no_route_backoff
{

struct Options
{
    bool Enabled = false;                // NoRouteBackoff (default 0)
    std::uint64_t BaseMs = 60000;        // the first wait after a refusal (NoRouteBackoffSec, default 60)
    std::uint64_t MaxMs = 1800000;       // the wait doubles with every refusal of the same place up to this
    float SameYards = 12.0f;             // a destination this close (2D) to a remembered one is the same place
    std::uint64_t ForgetMs = 3600000;    // a place not refused for this long (and no longer waiting) is forgotten
    std::size_t MaxEntries = 16;         // places remembered per bot (the oldest refusal goes first)
};

struct Entry
{
    std::uint32_t Map = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    std::uint32_t Fails = 0;       // refusals of this place
    std::uint64_t UntilMs = 0;     // not sent before this
    std::uint64_t LastMs = 0;      // the last refusal
    bool HeldLogged = false;       // the no_route_backoff stage=held line for this wait is written
};

struct Book
{
    std::vector<Entry> Entries;
};

// The wait after `fails` refusals in all: BaseMs, doubled for each refusal after the first, at most MaxMs.
std::uint64_t WaitMs(Options const& options, std::uint32_t fails);
// A refusal of a walk to (x, y, z) on `map`. Returns the place's entry (its Fails and UntilMs are new), null when the backoff is off.
Entry const* NoteRefused(Options const& options, Book& book, std::uint32_t map, float x, float y, float z, std::uint64_t nowMs);
// The entry that holds back a walk to (x, y) on `map` now, null when none does (or the backoff is off).
Entry* Blocked(Options const& options, Book& book, std::uint32_t map, float x, float y, std::uint64_t nowMs);
// A walk to (x, y) on `map` was applied: the place is reachable, it is forgotten.
void NoteReached(Options const& options, Book& book, std::uint32_t map, float x, float y);

} // namespace no_route_backoff
