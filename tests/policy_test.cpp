// Unit tests for BotPolicy, on hand-built snapshots. No core needed.
#include "bot_policy.h"

#include <cstdio>
#include <string>

using forever_module::ActionType;
using forever_module::BotAction;
using forever_module::BotActionStatus;
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

// A hostile at `x` yards with the given health percent (max_health is set, so the percent is real).
NearbyUnit Hostile(std::uint64_t counter, float x, std::uint32_t healthPct)
{
    NearbyUnit unit = Unit(8, counter, x, true, healthPct);
    unit.max_health = 100;
    return unit;
}

// A warrior fighting one hostile, with the rotation on.
BotPolicy WarriorPolicy()
{
    PolicyOptions options;
    options.ClassId = 1;
    return BotPolicy(options);
}

bool HasCast(std::vector<BotAction> const& actions, std::uint32_t spell)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kCastSpell && action.spell_id == spell)
            return true;
    return false;
}

void WarriorExecutesALowTarget()
{
    BotPolicy policy = WarriorPolicy();
    auto actions = policy.Decide(View(1, 0, { Hostile(2, 3.0f, 15) }), 1000);
    CHECK(actions.size() == 2);
    CHECK(HasCast(actions, 5308));
    if (HasCast(actions, 5308))
    {
        for (BotAction const& action : actions)
            if (action.type == ActionType::kCastSpell)
                CHECK(action.target_guid == MakeUnitRef(8, 1, 2));
    }
}

void GlobalCooldownSpacesCasts()
{
    BotPolicy policy = WarriorPolicy();
    CHECK(HasCast(policy.Decide(View(1, 0, { Hostile(2, 3.0f, 15) }), 1000), 5308));
    // Before the global cooldown ends: no cast, and the attack is unchanged.
    CHECK(policy.Decide(View(2, 0, { Hostile(2, 3.0f, 15) }), 1200).empty());
    // After it: Execute again.
    CHECK(HasCast(policy.Decide(View(3, 0, { Hostile(2, 3.0f, 15) }), 2600), 5308));
}

void RefusedCastFallsThrough()
{
    BotPolicy policy = WarriorPolicy();
    CHECK(HasCast(policy.Decide(View(1, 0, { Hostile(2, 3.0f, 15) }), 1000), 5308));

    // The core refused Execute as out of range: the policy waits a second and uses the next row that is allowed (Rend).
    BotSnapshot refused;
    refused.guid = 0;
    NearbyUnit unit = Hostile(2, 3.0f, 15);
    WorldTickSnapshotView view = View(2, 0, { unit });
    view.bots[0].last_action_sequence = 1;
    view.bots[0].last_action_type = uint8_t(ActionType::kCastSpell);
    view.bots[0].last_action_spell = 5308;
    view.bots[0].last_action_status = BotActionStatus::kOutOfRange;
    auto actions = policy.Decide(view, 2600);
    CHECK(!HasCast(actions, 5308));
    CHECK(HasCast(actions, 772)); // Rend
}

void ChargeWhileOpening()
{
    BotPolicy policy = WarriorPolicy();
    // A target 12 yards away at the start of the fight: approach, and Charge.
    auto actions = policy.Decide(View(1, 0, { Hostile(2, 12.0f, 100) }), 1000);
    CHECK(actions.size() == 2);
    CHECK(actions.size() >= 1 && actions[0].type == ActionType::kFollow);
    CHECK(HasCast(actions, 100));
}

void ApproachesADistantTarget()
{
    BotPolicy policy = WarriorPolicy();
    // Well past the opener: no Charge; the bot walks in and nothing else is in melee reach.
    CHECK(HasCast(policy.Decide(View(1, 0, { Hostile(2, 12.0f, 100) }), 1000), 100));
    auto later = policy.Decide(View(2, 0, { Hostile(2, 12.0f, 100) }), 10000);
    CHECK(later.empty()); // the follow is unchanged and no row is allowed at 12 yards
}

void OverPullIsSkipped()
{
    // Four hostiles within 3 to 6 yards: a group of four is more than the bot pulls. No target, so the bot only stops.
    BotPolicy policy = WarriorPolicy();
    auto actions = policy.Decide(View(1, 0, { Hostile(2, 3.0f, 100), Hostile(3, 4.0f, 100), Hostile(4, 5.0f, 100), Hostile(5, 6.0f, 100) }), 1000);
    CHECK(actions.size() == 1 && actions[0].type == ActionType::kStop);
    for (BotAction const& action : actions)
        CHECK(action.type != ActionType::kAttackTarget && action.type != ActionType::kCastSpell);
}

void SmallGroupIsPulled()
{
    // Three hostiles together is the most the bot pulls: it attacks the closest.
    BotPolicy policy = WarriorPolicy();
    auto actions = policy.Decide(View(1, 0, { Hostile(2, 3.0f, 100), Hostile(3, 4.0f, 100), Hostile(4, 5.0f, 100) }), 1000);
    bool attacked = false;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kAttackTarget && action.target_guid == MakeUnitRef(8, 1, 2))
            attacked = true;
    CHECK(attacked);
}

void ThreatTargetIsFoughtInAGroup()
{
    // A group of four, but one of them is already attacking the bot: it is fought, even though a pull would be refused.
    BotPolicy policy = WarriorPolicy();
    auto actions = policy.Decide(View(1, MakeUnitRef(8, 1, 5), { Hostile(2, 3.0f, 100), Hostile(3, 4.0f, 100), Hostile(4, 5.0f, 100), Hostile(5, 6.0f, 100) }), 1000);
    bool followedAttacker = false;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kFollow && action.target_guid == MakeUnitRef(8, 1, 5))
            followedAttacker = true;
    CHECK(followedAttacker); // the attacker is 6 yards away: the bot walks to it
}

void CooldownSkipsTheSpell()
{
    // Execute is on cooldown in the snapshot, so the target at 15% health gets Rend instead.
    BotPolicy policy = WarriorPolicy();
    WorldTickSnapshotView view = View(1, 0, { Hostile(2, 3.0f, 15) });
    view.bots[0].spell_cooldowns[0].spell_id = 5308;
    view.bots[0].spell_cooldowns[0].remaining_ms = 5000;
    auto actions = policy.Decide(view, 1000);
    CHECK(!HasCast(actions, 5308));
    CHECK(HasCast(actions, 772));
}

void RendIsNotRecastWhileActive()
{
    BotPolicy policy = WarriorPolicy();
    CHECK(HasCast(policy.Decide(View(1, 0, { Hostile(2, 3.0f, 100) }), 1000), 772));

    // Rend was applied at 2600 ms, so it lasts until 23600 ms. Until then the bot uses the next allowed row (Heroic Strike).
    WorldTickSnapshotView applied = View(2, 0, { Hostile(2, 3.0f, 100) });
    applied.bots[0].last_action_sequence = 1;
    applied.bots[0].last_action_type = uint8_t(ActionType::kCastSpell);
    applied.bots[0].last_action_spell = 772;
    applied.bots[0].last_action_status = BotActionStatus::kApplied;
    auto actions = policy.Decide(applied, 2600);
    CHECK(!HasCast(actions, 772));
    CHECK(HasCast(actions, 78));

    // After it wears off, Rend is cast again.
    CHECK(HasCast(policy.Decide(View(3, 0, { Hostile(2, 3.0f, 100) }), 24000), 772));
}

int main()
{
    OverPullIsSkipped();
    SmallGroupIsPulled();
    ThreatTargetIsFoughtInAGroup();
    CooldownSkipsTheSpell();
    RendIsNotRecastWhileActive();
    WarriorExecutesALowTarget();
    GlobalCooldownSpacesCasts();
    RefusedCastFallsThrough();
    ChargeWhileOpening();
    ApproachesADistantTarget();
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
