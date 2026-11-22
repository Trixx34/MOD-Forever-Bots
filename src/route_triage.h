// SPDX-License-Identifier: GPL-2.0-or-later
// Route-failure triage: why a move was refused for "no route", from what the bot was doing when it was. The no_route journal line carries the
// cause (cause=...), so the refusals can be split by it without re-reading the policy. Pure logic, unit-tested in tests/route_triage_test.cpp.
#pragma once

#include <string_view>

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

// The part of the policy that chose the refused walk (2026-11-20, the no_route line's caller= field), from its move reason (BotTurn::moveWhy, the
// move_why line's why=): grind, explore, hub, quest, errand, vendor, goto, event, combat, dungeon, death, other. An empty or unknown reason is other.
inline char const* Caller(char const* why)
{
    if (!why || !*why)
        return "other";
    std::string_view const w(why);
    struct Group
    {
        std::string_view Why;
        char const* Caller;
    };
    static constexpr Group kGroups[] = {
        { "state.GrindTarget", "grind" }, { "state.ExploreTarget", "explore" }, { "hub", "hub" }, { "list[state.NextHub % list.size()]", "hub" },
        { "quest_walk", "quest" }, { "quest_hunt", "quest" }, { "order.Dest", "errand" }, { "vendor", "vendor" }, { "rep_vendor", "vendor" },
        { "repair_wait", "vendor" }, { "gather_walk", "errand" }, { "point_walk", "errand" }, { "state.GotoPoint", "goto" }, { "visit.Dest", "event" },
        { "event_move", "event" }, { "event_escort", "event" }, { "death_area", "death" }, { "return_to_instance", "dungeon" }, { "*waypoint", "dungeon" },
        { "flee", "combat" }, { "kite", "combat" }, { "band_kite", "combat" }, { "heal_los", "combat" }, { "pull_rest", "combat" },
        { "pull_hold", "combat" }, { "pull_unreachable", "combat" }, { "fight_height_avoid", "combat" }, { "approach_target", "combat" },
        { "tank_spot", "dungeon" }, { "tank_spot_hold", "dungeon" }, { "boss_flank", "dungeon" }, { "warder_hold", "dungeon" },
        { "regroup_rally", "dungeon" }, { "healer_fetch", "dungeon" }, { "raid_rule_move", "dungeon" }, { "dead_walk", "death" },
        { "fight_risk_retreat", "combat" }, { "telegraph_dodge", "combat" }, { "cast_reposition", "combat" }, { "los_reposition", "combat" },
        { "ground_avoid", "combat" },
    };
    for (Group const& group : kGroups)
        if (group.Why == w)
            return group.Caller;
    return "other";
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
