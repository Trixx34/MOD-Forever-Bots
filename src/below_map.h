// SPDX-License-Identifier: GPL-2.0-or-later
// Below-map recovery (2026-11-18, doc/level-safe.md; config BelowMapRecover, default off): a bot that fell through the ground stands far below the places around
// it and every walk it is given is refused for no route (the 2026-11-18 sim: 30k of 101k no_route events were walks under 100 yards; a bot at the Ironforge
// auction house at z=19 with the auctioneers at z=505, one near the Skyborne auctioneer at z=9 with the ground at about 666). Two signs, either one:
//   - z_below: every known place of its map (off_map::KnownWorld: quest givers and anchors, grind spots, hubs) within NearYards lies more than DropYards
//     above the bot (the closest floor to the bot is taken as the ground), for BelowForMs and at least BelowChecks checks in a row;
//   - no_route_storm: StormCount no-route answers for short walks (6 to TinyYards yards) within StormWindowMs.
// The bot is then teleported to that floor (z_below) or the nearest known place of its side (storm), at most once per CooldownMs, with an event line.
// Not dead, in combat (the core refuses a teleport then), on a flight, or held by the policy (a scenario, a dungeon route): kBlocked.
//
// Arm C (2026-11-19, 400 bots, 7.9 h): 5,148 recoveries on 376 of 400 bots (max 33 per bot), too many to be real falls. The ground was the single nearest
// known place within 60 yards, at any height: a quest giver on a tower or a tree platform (map 1 pos 10329,848,1133 against 1326), a grind spot on a hill or
// mesa above a valley, a place on another floor of a city. That is not the ground under the bot (the module has no height query of its own). Then the lowest
// near place became the ground, and a sign is not acted on (a kSkip verdict: event below_map_skip, counted in the hourly line) when:
//   - on_transport: the bot stands on a ship, zeppelin or elevator (its z is the deck's);
//   - swimming: in water below a cliff with places on top;
//   - on_path: the core applied a walk (kMoveTo needs a full navmesh route from where the bot stands) within RoutedRecentMs and the bot has not dropped more
//     than RoutedDropYards since: it stands on walkable ground. A falling bot (movement_flags bit 2) is not excused this way;
//   - near_ground (storm only): a known place within NearYards lies within NearGroundYards of the bot's height (map 2991 pos 4126,1953,976 with ground 980
//     was a storm recovery: short walks refused beside a place at the bot's own height are a navmesh gap, not a fall).
// A skip starts the sign over (the drop must persist again, the storm's answers are dropped).
//
// 2026-11-20 (doc/belowmap-noroute-20261120.md): the 11-20 strata run still had 2,655 recoveries in one arm. There is still no height query in the module or
// the snapshot: the "ground" is only the known places within NearYards. Two more causes were found in the code:
//   - the core walks a bot in a straight 3D line when the navmesh has no path (MoveSplineInit::MoveTo on PATHFIND_NOPATH, after ModuleBotActions' kMoveTo
//     answered kNoRoute): a bot crossing a hill or a valley that way is under the terrain for a while, and a z_below sign fires on a bot that is walking.
//     Such a walk ends at its destination's height, so a moving bot recovers by itself;
//   - the storm counted the same refused destination again and again (the no_route retry of one point), and every refused explore walk (a point at the
//     bot's own height, 50 to 100 yards away), none of which says anything about the bot's height.
// Now (keys BelowMapChecks, BelowMapMinPlaces, BelowMapRequireStill, BelowMapStormRiseYards, BelowMapStormDistinct; each defaults to the safe side):
//   - the ground is the closest floor to the bot's height among the near places (FindGround: the lowest one when all lie above, never the highest), and
//     the event carries the floors seen (distinct heights in FloorBandYards bands), the lowest and the highest place;
//   - z_below needs MinPlaces near places (one giver on a tower is no floor), BelowChecks checks in a row (default 10, about 20 s), and the bot standing
//     still: a moving bot (RequireStill), a flying one and a falling one (for FallGraceMs; a longer fall is a fall into the void and counts) is skipped
//     with reason=moving|flying|falling|few_places, and the sign starts over;
//   - the storm counts a refused short walk only when its destination lies StormRiseYards or more above the bot (a bot under the ground walking up to
//     the places it fell from) and, with StormDistinct, each destination once (StormSameYards).
//
// Pure logic, unit-tested in tests/level_safe_test.cpp, tests/arm_c_fixes_test.cpp and tests/belowmap_noroute_test.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace below_map
{

struct Options
{
    bool Enabled = false;                 // off here and in the module settings (BelowMapRecover = 0): the sim first
    float DropYards = 60.0f;              // the closest floor near the bot lies this much higher than the bot
    float NearYards = 60.0f;              // ... of the known places within this (2D)
    std::uint64_t BelowForMs = 20000;     // ... for this long without a break
    std::uint32_t BelowChecks = 10;       // ... and at least this many checks in a row (the policy checks every CheckEveryMs; BelowMapChecks)
    std::uint64_t CheckEveryMs = 2000;    // the policy looks at a bot this often (the near places are a scan of the map's list)
    std::uint32_t MinPlaces = 2;          // z_below only with at least this many known places within NearYards (BelowMapMinPlaces)
    bool RequireStill = true;             // z_below only for a bot that is not moving (BelowMapRequireStill)
    std::uint64_t FallGraceMs = 30000;    // a falling bot is skipped this long; a longer fall is a fall into the void and counts
    float FloorBandYards = 10.0f;         // places within this of each other in height are one floor (the floors= field)
    std::uint32_t StormCount = 8;         // this many short no-route answers ...
    std::uint64_t StormWindowMs = 300000; // ... within this long
    float StormRiseYards = 30.0f;         // ... for destinations at least this much above the bot (0 = any; BelowMapStormRiseYards)
    bool StormDistinct = true;            // ... each destination counted once (BelowMapStormDistinct)
    float StormSameYards = 12.0f;         // a destination this close (2D) to a counted one is the same one
    float TinyYards = 100.0f;             // a short walk: under this many yards to its destination (and 6 or more: a shorter one is an arrival)
    std::uint64_t CooldownMs = 600000;    // no second try for the same bot before this
    std::uint64_t RoutedRecentMs = 60000; // on_path: a walk the core applied this recently ...
    float RoutedDropYards = 20.0f;        // ... and the bot no more than this below where it stood then
    float NearGroundYards = 15.0f;        // near_ground: a known place within NearYards this close to the bot's height (storm only)
    std::uint64_t SkipLogMs = 600000;     // a below_map_skip line (and count) at most once per bot this often
};

// What the known places within NearYards say about the ground under a bot at height z (FindGround).
struct Ground
{
    bool HasGround = false;    // at least one place
    std::size_t Index = 0;     // which of the heights given is the ground
    float GroundZ = 0.0f;      // the closest floor to the bot's height: the place with the smallest |z - bot z| (the lower one on a tie)
    float NearestDz = 1e9f;    // |GroundZ - bot z|
    float LowZ = 0.0f;         // the lowest place
    float HighZ = 0.0f;        // the highest place
    std::uint32_t Places = 0;  // how many places
    std::uint32_t Floors = 0;  // distinct heights among them (FloorBandYards bands)
};
Ground FindGround(std::vector<float> const& placeZs, float z, float floorBandYards);

struct Facts
{
    bool Alive = true;
    bool InCombat = false;
    bool Travelling = false;  // on a taxi flight
    bool OnTransport = false; // on a ship, zeppelin or elevator (transport_guid, or movement_flags bit 3)
    bool Swimming = false;    // movement_flags bit 0
    bool Flying = false;      // movement_flags bit 1
    bool Falling = false;     // movement_flags bit 2
    bool Moving = false;      // the snapshot's moving
    bool Held = false;
    bool HasGround = false;   // a known place lies within NearYards
    float Z = 0.0f;           // the bot's height
    float GroundZ = 0.0f;     // the closest floor of the known places within NearYards (FindGround)
    float NearestDz = 1e9f;   // the smallest |place z - bot z| of those places
    std::uint32_t Places = 0; // how many known places lie within NearYards (FindGround)
    std::uint32_t Floors = 0; // distinct heights among them
};

// A short no-route answer that counts for the storm.
struct NoRoute
{
    std::uint64_t AtMs = 0;
    float X = 0.0f;
    float Y = 0.0f;
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
    bool Falling = false;                  // the bot was falling at the last check (FallingSinceMs is live)
    std::uint64_t FallingSinceMs = 0;      // when the bot was first seen falling in this fall
    std::uint32_t LastChecks = 0;          // the checks in a row behind the last verdict (the checks= field)
    std::vector<NoRoute> NoRoutes;         // the recent short no-route answers that count for the storm (bounded)
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
    kSkipMoving,      // ... (z_below) the bot is walking (a straight-line walk through the terrain ends at its destination)
    kSkipFalling,     // ... (z_below) falling, for FallGraceMs
    kSkipFlying,      // ... (z_below) flying
    kSkipFewPlaces,   // ... (z_below) fewer than MinPlaces known places near: no floor to judge by
};

bool IsSkip(Verdict verdict);
bool IsSend(Verdict verdict);
// A no-route answer for a walk that had `toDestYards` left, to (destX, destY, destZ), for a bot at height botZ.
void NoteNoRoute(Options const& options, Watch& watch, float toDestYards, float destX, float destY, float destZ, float botZ, std::uint64_t nowMs);
// The core applied a walk for the bot standing at height `z` (on_path).
void NoteRouted(Options const& options, Watch& watch, float z, std::uint64_t nowMs);
Verdict Check(Options const& options, Watch& watch, Facts const& facts, std::uint64_t nowMs);
// "z_below" / "no_route_storm" for a send, the skip reason ("on_transport", "swimming", "on_path", "near_ground", "moving", "falling", "flying",
// "few_places") for a skip, else the verdict's name.
char const* VerdictName(Verdict verdict);
// A below_map_skip line (and count) is due for this skip: at most once per SkipLogMs per bot. Marks it written.
bool SkipLogDue(Options const& options, Watch& watch, std::uint64_t nowMs);

} // namespace below_map
