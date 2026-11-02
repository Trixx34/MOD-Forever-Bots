// SPDX-License-Identifier: GPL-2.0-or-later
// Route-failure triage: why a move was refused for "no route", from what the bot was doing when it was. The no_route journal line carries the
// cause (cause=...), so the refusals can be split by it without re-reading the policy. Pure logic, unit-tested in tests/route_triage_test.cpp.
#pragma once

namespace route_triage
{

enum class NoRouteCause
{
    kArrived, // the bot already stands at the destination (a path of no length is refused, which is an arrival, not a missing route)
    kCombat,  // the bot was in combat (its move was a chase or a retreat)
    kToUnit,  // the last move walked to a unit (a creature or a player), which may have moved away or be unreachable
    kToPoint, // the last move walked to a fixed point (a grind spot, a hub, a vendor)
    kNoMove,  // no move of the bot's is on record (the refusal is for a move the module did not track)
};

// A bot within this many yards of its last move's destination has arrived.
constexpr float kArrivedYards = 6.0f;

// haveMove: whether a last move is on record. toUnit: whether that move targeted a unit. distanceToDest: the bot's distance to that move's
// destination, in yards (ignored without a move).
inline NoRouteCause Classify(bool inCombat, bool haveMove, bool toUnit, float distanceToDest)
{
    if (haveMove && distanceToDest < kArrivedYards)
        return NoRouteCause::kArrived;
    if (inCombat)
        return NoRouteCause::kCombat;
    if (!haveMove)
        return NoRouteCause::kNoMove;
    return toUnit ? NoRouteCause::kToUnit : NoRouteCause::kToPoint;
}

inline char const* Name(NoRouteCause cause)
{
    switch (cause)
    {
        case NoRouteCause::kArrived: return "arrived";
        case NoRouteCause::kCombat: return "combat";
        case NoRouteCause::kToUnit: return "to_unit";
        case NoRouteCause::kToPoint: return "to_point";
        case NoRouteCause::kNoMove: return "no_move";
    }
    return "unknown";
}

} // namespace route_triage
