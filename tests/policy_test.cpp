// Unit tests for BotPolicy, on hand-built snapshots. No core needed.
#include "bot_policy.h"

#include <cstdio>
#include <string>

using forever_module::ActionType;
using forever_module::BotAction;
using forever_module::BotSnapshot;
using forever_module::MakeUnitRef;
using forever_module::NearbyUnit;
using forever_module::WorldTickSnapshotView;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

constexpr std::uint64_t kBot = 42;

// One bot at the origin, with the given nearby units. Each unit sits at (x, 0).
WorldTickSnapshotView View(std::uint64_t tick, std::uint64_t threat, std::vector<NearbyUnit> nearby)
{
    WorldTickSnapshotView view;
    view.tick_number = tick;
    view.max_nearby_units = 8;
    BotSnapshot bot;
    bot.guid = kBot;
    bot.health = 1000;
    bot.max_health = 1000;
    bot.threat_target_guid = threat;
    bot.nearby_count = static_cast<std::uint32_t>(nearby.size());
    view.bots.push_back(bot);
    view.nearby.resize(view.max_nearby_units);
    for (std::size_t i = 0; i < nearby.size(); ++i)
        view.nearby[i] = nearby[i];
    return view;
}

NearbyUnit Unit(std::uint8_t type, std::uint64_t counter, float x, bool hostile, std::uint32_t health = 100)
{
    NearbyUnit unit;
    unit.guid = MakeUnitRef(type, 1, counter);
    unit.pos_x = x;
    unit.health = health;
    unit.is_hostile = hostile;
    return unit;
}

void AttacksClosestHostile()
{
    BotPolicy policy;
    auto actions = policy.Decide(View(1, 0, { Unit(8, 1, 30.0f, true), Unit(8, 2, 5.0f, true), Unit(2, 3, 1.0f, false) }));
    CHECK(actions.size() == 1);
    if (actions.size() == 1)
    {
        CHECK(actions[0].bot_guid == kBot);
        CHECK(actions[0].type == ActionType::kAttackTarget);
        CHECK(actions[0].target_guid == MakeUnitRef(8, 1, 2)); // the hostile at 5 yards, not the one at 30
    }
}

void KeepsCurrentThreatTarget()
{
    BotPolicy policy;
    std::uint64_t const threat = MakeUnitRef(8, 1, 9);
    auto actions = policy.Decide(View(1, threat, { Unit(8, 2, 5.0f, true) }));
    CHECK(actions.size() == 1 && actions[0].target_guid == threat);
}

void FollowsPlayerWhenNoEnemy()
{
    BotPolicy policy;
    auto actions = policy.Decide(View(1, 0, { Unit(2, 7, 12.0f, false), Unit(8, 3, 4.0f, false) }));
    CHECK(actions.size() == 1);
    if (actions.size() == 1)
    {
        CHECK(actions[0].type == ActionType::kFollow);
        CHECK(actions[0].target_guid == MakeUnitRef(2, 1, 7));
    }
}

void StopsWhenNothingNearby()
{
    BotPolicy policy;
    auto actions = policy.Decide(View(1, 0, {}));
    CHECK(actions.size() == 1 && actions[0].type == ActionType::kStop);
}

void DeadUnitsAreNotAttacked()
{
    BotPolicy policy;
    auto actions = policy.Decide(View(1, 0, { Unit(8, 2, 5.0f, true, 0) }));
    CHECK(actions.size() == 1 && actions[0].type == ActionType::kStop);
}

// A bot's health and max health, with the bot's own threat target. Builds on View().
WorldTickSnapshotView Hurt(std::uint32_t health, std::vector<NearbyUnit> nearby, std::uint64_t threat = 0)
{
    WorldTickSnapshotView view = View(1, threat, std::move(nearby));
    view.bots[0].health = health;
    return view;
}

void RetreatsBelowThirtyPercent()
{
    BotPolicy policy;
    // 250 of 1000 is below 30%: a hostile is near and it is fighting, but it follows the player instead.
    auto actions = policy.Decide(Hurt(250, { Unit(8, 2, 5.0f, true), Unit(2, 7, 12.0f, false) }, MakeUnitRef(8, 1, 9)));
    CHECK(actions.size() == 1);
    if (actions.size() == 1)
    {
        CHECK(actions[0].type == ActionType::kFollow);
        CHECK(actions[0].target_guid == MakeUnitRef(2, 1, 7));
    }
}

void RetreatWithNoPlayerStops()
{
    BotPolicy policy;
    auto actions = policy.Decide(Hurt(100, { Unit(8, 2, 5.0f, true) }));
    CHECK(actions.size() == 1 && actions[0].type == ActionType::kStop);
}

void ThirtyPercentIsNotLow()
{
    BotPolicy policy;
    auto actions = policy.Decide(Hurt(300, { Unit(8, 2, 5.0f, true) }));
    CHECK(actions.size() == 1 && actions[0].type == ActionType::kAttackTarget);
}

void DeadBotGetsNoOrders()
{
    BotPolicy policy;
    CHECK(policy.Decide(Hurt(0, { Unit(8, 2, 5.0f, true) })).empty());
}

void OnlyChangesArePublished()
{
    BotPolicy policy;
    CHECK(policy.Decide(View(1, 0, { Unit(8, 2, 5.0f, true) })).size() == 1);
    CHECK(policy.Decide(View(2, 0, { Unit(8, 2, 5.0f, true) })).empty()); // same decision, next tick
    CHECK(policy.Decide(View(3, 0, { Unit(8, 2, 5.0f, true) })).empty()); // unchanged, no action
    CHECK(policy.Decide(View(4, 0, {})).size() == 1);                 // now there is nothing to fight
}

} // namespace

// Test casts: a bot fighting a hostile gets the test spell at its target on its own timer, after the attack decision.
void TestCastOnItsOwnTimer()
{
    PolicyOptions options;
    options.TestSpell = 585;
    options.CastEveryTicks = 20;
    BotPolicy policy(options);

    auto first = policy.Decide(View(1, 0, { Unit(8, 2, 5.0f, true) }));
    CHECK(first.size() == 2);
    if (first.size() == 2)
    {
        CHECK(first[0].type == ActionType::kAttackTarget);
        CHECK(first[1].type == ActionType::kCastSpell && first[1].spell_id == 585);
        CHECK(first[1].target_guid == MakeUnitRef(8, 1, 2));
    }

    // Same fight, before the interval: nothing new.
    CHECK(policy.Decide(View(10, 0, { Unit(8, 2, 5.0f, true) })).empty());

    // After the interval: only the cast, since the attack is unchanged.
    auto later = policy.Decide(View(21, 0, { Unit(8, 2, 5.0f, true) }));
    CHECK(later.size() == 1 && later[0].type == ActionType::kCastSpell);
}

void NoTestCastWhenOff()
{
    BotPolicy policy;
    auto first = policy.Decide(View(1, 0, { Unit(8, 2, 5.0f, true) }));
    CHECK(first.size() == 1 && first[0].type == ActionType::kAttackTarget);
    CHECK(policy.Decide(View(100, 0, { Unit(8, 2, 5.0f, true) })).empty());
}

void StatusNamesAreReadable()
{
    CHECK(std::string(ActionStatusName(forever_module::BotActionStatus::kOutOfRange)) == "out of range");
    CHECK(std::string(ActionStatusName(forever_module::BotActionStatus::kOnCooldown)) == "on cooldown");
}

int main()
{
    TestCastOnItsOwnTimer();
    NoTestCastWhenOff();
    StatusNamesAreReadable();
    AttacksClosestHostile();
    KeepsCurrentThreatTarget();
    FollowsPlayerWhenNoEnemy();
    StopsWhenNothingNearby();
    DeadUnitsAreNotAttacked();
    RetreatsBelowThirtyPercent();
    RetreatWithNoPlayerStops();
    ThirtyPercentIsNotLow();
    DeadBotGetsNoOrders();
    OnlyChangesArePublished();

    if (failures == 0)
        std::printf("policy_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
