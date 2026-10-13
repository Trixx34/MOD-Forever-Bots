// Unit tests for the class rotation rules and facts (combat_rotation.h). No core needed.
#include "combat_rotation.h"

#include <cmath>
#include <cstdio>

using rotation::Cond;
using rotation::Facts;
using rotation::Rule;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

void Conditions()
{
    Facts f;
    f.TargetHpPct = 15;
    CHECK(rotation::Allowed({ Cond::TargetHpBelow, 20 }, f));
    CHECK(!rotation::Allowed({ Cond::TargetHpAbove, 20 }, f));
    CHECK(rotation::Allowed({ Cond::Always, 0 }, f));

    f.SelfPowerPct = 30;
    CHECK(rotation::Allowed({ Cond::SelfPowerAbove, 30 }, f));  // at the threshold counts as above
    CHECK(!rotation::Allowed({ Cond::SelfPowerBelow, 20 }, f));

    f.EnemiesOnBot = 2;
    CHECK(rotation::Allowed({ Cond::EnemiesAtLeast, 2 }, f));
    CHECK(!rotation::Allowed({ Cond::EnemiesAtLeast, 3 }, f));

    f.FightMs = 7999;
    CHECK(rotation::Allowed({ Cond::Opener, 8 }, f));
    f.FightMs = 8000;
    CHECK(!rotation::Allowed({ Cond::Opener, 8 }, f));

    f.TargetElite = false;
    f.TargetLevelDiff = 1;
    CHECK(!rotation::Allowed({ Cond::TargetStrong, 2 }, f));
    f.TargetLevelDiff = 2;
    CHECK(rotation::Allowed({ Cond::TargetStrong, 2 }, f));
    f.TargetElite = true;
    f.TargetLevelDiff = 0;
    CHECK(rotation::Allowed({ Cond::TargetStrong, 2 }, f));
}

void Rows()
{
    auto const warrior = rotation::RowsForClass(1);
    CHECK(!warrior.empty());
    CHECK(warrior.front().Spell == 5308);     // Execute first
    CHECK(rotation::RowsForClass(6).empty()); // no rotation for a class id without rows
    for (auto const& row : warrior)
        CHECK(row.Spell != 0);
}

void FactsFromSnapshot()
{
    forever_module::BotSnapshot bot;
    bot.health = 500;
    bot.max_health = 1000;
    bot.power = 25;
    bot.max_power = 100;
    bot.level = 10;
    bot.pos_x = 0.0f;
    bot.pos_y = 0.0f;

    forever_module::NearbyUnit target;
    target.health = 30;
    target.max_health = 200;
    target.level = 12;
    target.elite = 1;
    target.casting = 1;
    target.is_hostile = true;
    target.pos_x = 3.0f;

    forever_module::NearbyUnit other;
    other.health = 10;
    other.max_health = 10;
    other.is_hostile = true;
    other.pos_x = 4.0f;
    forever_module::NearbyUnit far;
    far.health = 10;
    far.max_health = 10;
    far.is_hostile = true;
    far.pos_x = 20.0f;

    forever_module::NearbyUnit const units[] = { target, other, far };
    Facts const f = rotation::FactsFor(bot, units, &target, 4000);
    CHECK(f.SelfHpPct == 50);
    CHECK(f.SelfPowerPct == 25);
    CHECK(f.TargetHpPct == 15);
    CHECK(f.TargetElite && f.TargetCasting);
    CHECK(f.TargetLevelDiff == 2);
    CHECK(f.EnemiesOnBot == 2); // the target and the unit 4 yards away; the one 20 yards away is not counted
    CHECK(f.FightMs == 4000);
    CHECK(std::abs(rotation::DistanceYards(bot, target) - 3.0f) < 0.001f);

    // A bot with no maximum reads as full, and a missing target leaves the target facts at their defaults.
    forever_module::BotSnapshot empty;
    Facts const g = rotation::FactsFor(empty, {}, nullptr, 0);
    CHECK(g.SelfHpPct == 100 && g.SelfPowerPct == 100 && g.TargetHpPct == 100 && g.EnemiesOnBot == 0);
}

} // namespace

void EveryClassHasRows()
{
    // The classes with rotations: all nine except the ones the module does not spawn yet.
    for (std::uint32_t cls : { 1u, 2u, 3u, 4u, 5u, 7u, 8u, 9u, 11u })
        CHECK(!rotation::RowsForClass(cls).empty());
    CHECK(rotation::RowsForClass(6).empty()); // unknown here: no rotation, the bot melees
}

void RangedClassesApproachToRange()
{
    CHECK(rotation::ApproachYards(8) == 25.0f);  // mage
    CHECK(rotation::ApproachYards(9) == 25.0f);  // warlock
    CHECK(rotation::ApproachYards(3) == 25.0f);  // hunter
    CHECK(rotation::ApproachYards(1) == 5.0f);   // warrior
    CHECK(rotation::ApproachYards(4) == 5.0f);   // rogue
    CHECK(rotation::ApproachYards(2) == 5.0f);   // paladin
}

int main()
{
    EveryClassHasRows();
    RangedClassesApproachToRange();
    Conditions();
    Rows();
    FactsFromSnapshot();

    if (failures == 0)
        std::printf("rotation_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
