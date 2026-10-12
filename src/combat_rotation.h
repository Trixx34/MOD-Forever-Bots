// Class rotations of the bots (milestone M2). The rule evaluator is ported from the forever branch's BotRotation
// (src/server/game/Bots/BotRotation.{h,cpp}, pure part, GPL-2.0-or-later as in TrinityCore). The warrior rows are the
// forever spell table's warrior rows, rank 1 ids; the core casts the highest rank the bot knows.
//
// Pure logic over Tier 2 snapshot types: no SDK state, unit-tested in tests/rotation_test.cpp.
#pragma once

#include "forever_module/tier2_session.h"

#include <cstdint>
#include <span>

namespace rotation {

enum class Cond : std::uint8_t
{
    Always,
    TargetHpAbove,     // Param: percent
    TargetHpBelow,     // Param: percent
    SelfHpBelow,       // Param: percent
    SelfPowerBelow,    // Param: percent of the main resource
    SelfPowerAbove,    // Param: percent
    EnemiesAtLeast,    // Param: hostile units close to the bot
    TargetCasting,
    TargetFleeing,
    Opener,            // Param: seconds from the start of the fight
    TargetStrong,      // the target is elite, or Param or more levels above the bot
};

// What a rule reads. Built from a snapshot by FactsFor().
struct Facts
{
    std::int32_t SelfHpPct = 100;
    std::int32_t SelfPowerPct = 100;
    std::int32_t TargetHpPct = 100;
    std::uint32_t EnemiesOnBot = 0;
    bool TargetCasting = false;
    bool TargetFleeing = false;   // not in the snapshot yet, so always false
    bool TargetElite = false;
    std::int32_t TargetLevelDiff = 0;
    std::uint64_t FightMs = 0;
};

struct Rule
{
    Cond Condition = Cond::Always;
    std::int32_t Param = 0;
};

bool Allowed(Rule const& rule, Facts const& facts);

// One spell of a rotation. OnTarget false casts on the bot itself. For OnTarget rows the target must be between MinRange
// and MaxRange yards (MaxRange 0 means no limit).
struct Row
{
    std::uint32_t Spell;
    Rule When;
    bool OnTarget;
    float MinRange;
    float MaxRange;
};

// Rows in priority order, for a class id (1 = warrior). Empty for classes without rows yet.
std::span<Row const> RowsForClass(std::uint32_t classId);

// Distance in yards on the ground between the bot and a unit in the snapshot.
float DistanceYards(forever_module::BotSnapshot const& bot, forever_module::NearbyUnit const& unit);

// Facts for the rules: health and power percentages, the target's state and level, hostiles close to the bot.
// target may be null (the target is not in the snapshot's view).
rotation::Facts FactsFor(forever_module::BotSnapshot const& bot, std::span<forever_module::NearbyUnit const> nearby,
                         forever_module::NearbyUnit const* target, std::uint64_t fightMs);

} // namespace rotation
