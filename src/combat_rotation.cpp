// Ported from the forever branch's BotRotation.cpp (pure part) and BotCombat.cpp's warrior rows; see combat_rotation.h.
#include "combat_rotation.h"

#include <algorithm>
#include <cmath>

using forever_module::BotSnapshot;
using forever_module::NearbyUnit;

namespace rotation {

namespace {

constexpr std::uint32_t kWarriorClass = 1;

// Warrior rotation, in priority order. Spell ids are rank 1; the core casts the highest rank the bot knows.
constexpr Row kWarriorRows[] = {
    { 5308, { Cond::TargetHpBelow, 20 }, true, 0.0f, 5.0f },    // Execute
    { 2687, { Cond::SelfPowerBelow, 20 }, false, 0.0f, 0.0f },  // Bloodrage
    { 100, { Cond::Opener, 8 }, true, 8.0f, 25.0f },            // Charge: opening, from 8 to 25 yards
    { 6343, { Cond::EnemiesAtLeast, 2 }, false, 0.0f, 0.0f },   // Thunder Clap (area, around the bot)
    { 1160, { Cond::EnemiesAtLeast, 3 }, false, 0.0f, 0.0f },   // Demoralizing Shout (area)
    { 7386, { Cond::TargetStrong, 2 }, true, 0.0f, 5.0f },      // Sunder Armor on a strong target
    { 772, { Cond::Always, 0 }, true, 0.0f, 5.0f },             // Rend (no aura check yet: refreshes the bleed)
    { 845, { Cond::EnemiesAtLeast, 2 }, true, 0.0f, 5.0f },     // Cleave
    { 78, { Cond::SelfPowerAbove, 40 }, true, 0.0f, 5.0f },     // Heroic Strike with a rage reserve
};

bool IsLiveHostile(NearbyUnit const& unit)
{
    return unit.is_hostile && unit.health > 0;
}

} // namespace

bool Allowed(Rule const& rule, Facts const& f)
{
    switch (rule.Condition)
    {
        case Cond::Always:         return true;
        case Cond::TargetHpAbove:  return f.TargetHpPct > rule.Param;
        case Cond::TargetHpBelow:  return f.TargetHpPct < rule.Param;
        case Cond::SelfHpBelow:    return f.SelfHpPct < rule.Param;
        case Cond::SelfPowerBelow: return f.SelfPowerPct < rule.Param;
        case Cond::SelfPowerAbove: return f.SelfPowerPct >= rule.Param;
        case Cond::EnemiesAtLeast: return f.EnemiesOnBot >= std::uint32_t(std::max(rule.Param, 1));
        case Cond::TargetCasting:  return f.TargetCasting;
        case Cond::TargetFleeing:  return f.TargetFleeing;
        case Cond::Opener:         return f.FightMs < std::uint64_t(std::max(rule.Param, 0)) * 1000u;
        case Cond::TargetStrong:   return f.TargetElite || f.TargetLevelDiff >= std::max(rule.Param, 1);
    }
    return false;
}

std::span<Row const> RowsForClass(std::uint32_t classId)
{
    if (classId == kWarriorClass)
        return kWarriorRows;
    return {};
}

float DistanceYards(BotSnapshot const& bot, NearbyUnit const& unit)
{
    float const dx = unit.pos_x - bot.pos_x;
    float const dy = unit.pos_y - bot.pos_y;
    return std::sqrt(dx * dx + dy * dy);
}

Facts FactsFor(BotSnapshot const& bot, std::span<NearbyUnit const> nearby, NearbyUnit const* target, std::uint64_t fightMs)
{
    Facts facts;
    facts.FightMs = fightMs;
    facts.SelfHpPct = bot.max_health ? std::int32_t(std::uint64_t(bot.health) * 100 / bot.max_health) : 100;
    facts.SelfPowerPct = bot.max_power ? std::int32_t(std::uint64_t(bot.power) * 100 / bot.max_power) : 100;

    if (target)
    {
        facts.TargetHpPct = target->max_health ? std::int32_t(std::uint64_t(target->health) * 100 / target->max_health) : 100;
        facts.TargetCasting = target->casting != 0;
        facts.TargetElite = target->elite != 0;
        facts.TargetLevelDiff = std::int32_t(target->level) - std::int32_t(bot.level);
    }

    // Hostile units close to the bot: an approximation of the ones attacking it, for area rules.
    for (NearbyUnit const& unit : nearby)
        if (IsLiveHostile(unit) && DistanceYards(bot, unit) <= 8.0f)
            ++facts.EnemiesOnBot;
    if (target && facts.EnemiesOnBot == 0)
        facts.EnemiesOnBot = 1;

    return facts;
}

} // namespace rotation
