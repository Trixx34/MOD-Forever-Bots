// SPDX-License-Identifier: GPL-2.0-or-later
// Below-map recovery (2026-11-18, doc/level-safe.md; config BelowMapRecover, default off): a bot that fell through the ground stands far below the places around
// it and every walk it is given is refused for no route (the 2026-11-18 sim: 30k of 101k no_route events were walks under 100 yards; a bot at the Ironforge
// auction house at z=19 with the auctioneers at z=505, one near the Skyborne auctioneer at z=9 with the ground at about 666). Two signs, either one:
//   - z_below: every known place of its map (off_map::KnownWorld: quest givers and anchors, grind spots, hubs) within NearYards lies more than DropYards
//     above the bot (the lowest of them is taken as the ground), for BelowForMs and at least BelowChecks checks in a row;
//   - no_route_storm: StormCount no-route answers for short walks (6 to TinyYards yards) within StormWindowMs.
// The bot is then teleported to that lowest place (z_below) or the nearest known place of its side (storm), at most once per CooldownMs, with an event line.
// Not dead, in combat (the core refuses a teleport then), on a flight, or held by the policy (a scenario, a dungeon route): kBlocked.
//
// Arm C (2026-11-19, 400 bots, 7.9 h): 5,148 recoveries on 376 of 400 bots (max 33 per bot), too many to be real falls. The ground was the single nearest
// known place within 60 yards, at any height: a quest giver on a tower or a tree platform (map 1 pos 10329,848,1133 against 1326), a grind spot on a hill or
// mesa above a valley, a place on another floor of a city. That is not the ground under the bot (the module has no height query of its own). Now the lowest
// near place is the ground, and a sign is not acted on (a kSkip verdict: event below_map_skip, counted in the hourly line) when:
//   - on_transport: the bot stands on a ship, zeppelin or elevator (its z is the deck's);
//   - swimming: in water below a cliff with places on top;
//   - on_path: the core applied a walk (kMoveTo needs a full navmesh route from where the bot stands) within RoutedRecentMs and the bot has not dropped more
//     than RoutedDropYards since: it stands on walkable ground. A falling bot (movement_flags bit 2) is not excused this way;
//   - near_ground (storm only): a known place within NearYards lies within NearGroundYards of the bot's height (map 2991 pos 4126,1953,976 with ground 980
//     was a storm recovery: short walks refused beside a place at the bot's own height are a navmesh gap, not a fall).
// A skip starts the sign over (the drop must persist again, the storm's answers are dropped).
//
// Pure logic, unit-tested in tests/level_safe_test.cpp and tests/arm_c_fixes_test.cpp.
#pragma once

#include <cstdint>
#include <vector>

namespace below_map
{

struct Options
{
    bool Enabled = false;                 // off here and in the module settings (BelowMapRecover = 0): the sim first
    float DropYards = 60.0f;              // every known place near the bot lies this much higher than the bot
    float NearYards = 60.0f;              // ... of those within this (2D)
    std::uint64_t BelowForMs = 20000;     // ... for this long without a break
    std::uint32_t BelowChecks = 5;        // ... and at least this many checks in a row (the policy checks every CheckEveryMs)
    std::uint64_t CheckEveryMs = 2000;    // the policy looks at a bot this often (the near places are a scan of the map's list)
    std::uint32_t StormCount = 8;         // this many short no-route answers ...
    std::uint64_t StormWindowMs = 300000; // ... within this long
    float TinyYards = 100.0f;             // a short walk: under this many yards to its destination (and 6 or more: a shorter one is an arrival)
    std::uint64_t CooldownMs = 600000;    // no second try for the same bot before this
    std::uint64_t RoutedRecentMs = 60000; // on_path: a walk the core applied this recently ...
    float RoutedDropYards = 20.0f;        // ... and the bot no more than this below where it stood then
    float NearGroundYards = 15.0f;        // near_ground: a known place within NearYards this close to the bot's height (storm only)
    std::uint64_t SkipLogMs = 600000;     // a below_map_skip line (and count) at most once per bot this often
};

struct Facts
{
    bool Alive = true;
    bool InCombat = false;
    bool Travelling = false;  // on a taxi flight
    bool OnTransport = false; // on a ship, zeppelin or elevator (transport_guid, or movement_flags bit 3)
    bool Swimming = false;    // movement_flags bit 0
    bool Falling = false;     // movement_flags bit 2
    bool Held = false;
    bool HasGround = false;   // a known place lies within NearYards
    float Z = 0.0f;           // the bot's height
    float GroundZ = 0.0f;     // the lowest of the known places within NearYards
    float NearestDz = 1e9f;   // the smallest |place z - bot z| of those places
};

struct Watch
{
    std::uint64_t BelowSinceMs = 0;        // when the bot was first seen below in this spell (0 = not below)
    std::uint32_t BelowChecks = 0;         // checks in a row that saw it below
    std::uint64_t NextTryMs = 0;
    std::uint64_t NextCheckMs = 0;         // the policy's own pace (CheckEveryMs)
    std::uint64_t SkipLoggedMs = 0;        // the last below_map_skip line
    bool SkipLogged = false;               // SkipLoggedMs is live
    bool LastSkipBelow = false;            // the sign the last kSkip verdict was about: z_below (true) or the storm
    bool Routed = false;                   // a walk was applied (RoutedMs, RoutedZ are live)
    std::uint64_t RoutedMs = 0;
    float RoutedZ = 0.0f;                  // the bot's height when it was
    std::vector<std::uint64_t> NoRoutes;   // when the recent short no-route answers came (bounded)
};

enum class Verdict : std::uint8_t
{
    kFine,            // nothing wrong
    kWaiting,         // below the ground, not long enough yet
    kBlocked,         // a sign, but dead, fighting, flying or held
    kCooldown,        // a sign, and the cooldown runs
    kSendBelow,       // teleport now: z_below
    kSendStorm,       // teleport now: no_route_storm
    kSkipTransport,   // a sign, not acted on: on a transport
    kSkipSwimming,    // ... swimming
    kSkipOnPath,      // ... a walk was applied recently and the bot did not drop since
    kSkipNearGround,  // ... (storm) a known place at the bot's own height
};

bool IsSkip(Verdict verdict);
bool IsSend(Verdict verdict);
// A no-route answer for a walk that had `toDestYards` left.
void NoteNoRoute(Options const& options, Watch& watch, float toDestYards, std::uint64_t nowMs);
// The core applied a walk for the bot standing at height `z` (on_path).
void NoteRouted(Options const& options, Watch& watch, float z, std::uint64_t nowMs);
Verdict Check(Options const& options, Watch& watch, Facts const& facts, std::uint64_t nowMs);
// "z_below" / "no_route_storm" for a send, the skip reason ("on_transport", "swimming", "on_path", "near_ground") for a skip, else the verdict's name.
char const* VerdictName(Verdict verdict);
// A below_map_skip line (and count) is due for this skip: at most once per SkipLogMs per bot. Marks it written.
bool SkipLogDue(Options const& options, Watch& watch, std::uint64_t nowMs);

} // namespace below_map
