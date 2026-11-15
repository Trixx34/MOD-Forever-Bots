// SPDX-License-Identifier: GPL-2.0-or-later
// Decision throttling for a bot that only walks (WalkDecideEvery). A solo bot that is out of combat, has nothing hostile near it, is not in a group, a
// dungeon, on a transport or on an order, and is walking to a place the policy sent it to, will not decide anything new before it arrives: the policy
// would send no action (the same move is never sent twice). Its whole turn (some 40 steps) is skipped on all but every Nth tick. A new result from the
// core, a hostile nearby, combat, a group invite, death or the end of the walk (moving = 0) ends the calm at once: the next tick is a full turn.
#pragma once

#include <cstdint>
#include <span>

#include "forever_module/tier2_layout.h"

namespace calm_walk
{

constexpr std::uint32_t kMaxEvery = 10;

// What the policy knows about the bot that is not in the snapshot.
struct Facts
{
    bool WalkOrdered = false;     // the policy's last decision was a kMoveTo (BotState::Last)
    bool OnOrder = false;         // an errand, a visit, tank lead, a route, a staging hold, a dungeon map: anything the policy drives with its own timers
    bool HasLootOrFight = false;  // fought units are waiting to be looted
};

// True when the bot is in the calm state. The snapshot says whether it moves and what is around it; `nearby` is its nearby units.
inline bool IsCalm(forever_module::BotSnapshot const& bot, std::span<forever_module::NearbyUnit const> nearby, Facts const& facts)
{
    if (!facts.WalkOrdered || facts.OnOrder || facts.HasLootOrFight)
        return false;
    if (bot.life_state != 0 || bot.moving == 0 || bot.in_combat != 0 || bot.threat_target_guid != 0 || bot.casting_spell != 0)
        return false;
    if (bot.group_leader_guid != 0 || bot.group_invite_from != 0 || bot.resurrect_from != 0 || bot.group_role != 0)
        return false;
    if (bot.in_flight != 0 || bot.movement_flags != 0 || bot.transport_guid != 0)
        return false;
    if (bot.ground_effect_count != 0 || bot.telegraph_count != 0 || bot.pet_state == 2)
        return false;
    for (forever_module::NearbyUnit const& unit : nearby)
        if (unit.is_hostile && unit.health != 0)
            return false;
    return true;
}

// The per-bot counter. `calmBefore` is the state the last full turn left (IsCalm at its end); `sequence` the last action sequence that turn had seen.
struct Gate
{
    bool CalmBefore = false;
    std::uint32_t Sequence = 0;
    std::uint32_t Skipped = 0;

    // Called at the start of a turn: true when the turn may be skipped (every - 1 skips, then a full turn).
    bool Skip(std::uint32_t every, bool calmNow, std::uint32_t sequenceNow)
    {
        if (every <= 1 || !CalmBefore || !calmNow || sequenceNow != Sequence)
        {
            Skipped = 0;
            return false;
        }
        if (Skipped + 1 >= every)
        {
            Skipped = 0;
            return false;
        }
        ++Skipped;
        return true;
    }

    // Called when a full turn ended.
    void Ran(bool calm, std::uint32_t sequence)
    {
        CalmBefore = calm;
        Sequence = sequence;
        Skipped = 0;
    }
};

} // namespace calm_walk
