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

// The rest action in `actions`: its spell_id (1 sit, 0 stand) and its position in the list, or -1 if there is none.
int RestAt(std::vector<BotAction> const& actions, std::uint32_t& spellId)
{
    for (std::size_t i = 0; i < actions.size(); ++i)
    {
        if (actions[i].type == ActionType::kRest)
        {
            spellId = actions[i].spell_id;
            return static_cast<int>(i);
        }
    }
    return -1;
}

void RestsWhenHurtAndIdle()
{
    BotPolicy policy;
    std::uint32_t spell = 99;
    CHECK(RestAt(policy.Decide(Hurt(600, {}), 1000), spell) >= 0);
    CHECK(spell == 1); // sit down
}

void NoRestAtFullHealthOrWithAPlayerNear()
{
    BotPolicy policy;
    std::uint32_t spell = 99;
    CHECK(RestAt(policy.Decide(View(1, 0, {}), 1000), spell) < 0); // full health

    BotPolicy withPlayer;
    CHECK(RestAt(withPlayer.Decide(Hurt(600, { Unit(2, 7, 4.0f, false, 100) }), 1000), spell) < 0); // a player to follow
}

void StandsBeforeAFight()
{
    BotPolicy policy;
    std::uint32_t spell = 99;
    CHECK(RestAt(policy.Decide(Hurt(600, {}), 1000), spell) >= 0 && spell == 1);

    // A hostile attacks the bot: the stand goes out before the attack in the same action set.
    auto actions = policy.Decide(Hurt(600, { Hostile(2, 3.0f, 100) }, MakeUnitRef(8, 1, 2)), 2000);
    int const stand = RestAt(actions, spell);
    CHECK(stand >= 0 && spell == 0);
    int attack = -1;
    for (std::size_t i = 0; i < actions.size(); ++i)
        if (actions[i].type == ActionType::kAttackTarget)
            attack = static_cast<int>(i);
    CHECK(attack >= 0 && stand < attack);
}

void RefusedSitIsRetriedLater()
{
    BotPolicy policy;
    std::uint32_t spell = 99;
    CHECK(RestAt(policy.Decide(Hurt(600, {}), 1000), spell) >= 0 && spell == 1);

    // The core refused the sit (in combat): the bot is not sitting, and it does not try again for a second.
    WorldTickSnapshotView refused = Hurt(600, {});
    refused.bots[0].last_action_sequence = 1;
    refused.bots[0].last_action_type = uint8_t(ActionType::kRest);
    refused.bots[0].last_action_status = BotActionStatus::kRefused;
    CHECK(RestAt(policy.Decide(refused, 2000), spell) < 0);
    CHECK(RestAt(policy.Decide(Hurt(600, {}, 0), 2500), spell) < 0);
    CHECK(RestAt(policy.Decide(Hurt(600, {}, 0), 3500), spell) >= 0 && spell == 1);
}

// Whether `actions` holds a kVendor with this spell_id, or a kTrain.
bool HasVendor(std::vector<BotAction> const& actions, std::uint32_t services)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kVendor && action.spell_id == services)
            return true;
    return false;
}

bool HasTrain(std::vector<BotAction> const& actions)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kTrain)
            return true;
    return false;
}

void ServicesWhenIdleOnTimerAndOnLevelUp()
{
    PolicyOptions options;
    options.ServiceEveryMs = 600000;
    options.TrainEveryMs = 600000;
    BotPolicy policy(options);

    WorldTickSnapshotView first = View(1, 0, {});
    first.bots[0].level = 10;
    auto actions = policy.Decide(first, 1000);
    CHECK(HasVendor(actions, 7)); // sell grey, repair, equip
    CHECK(HasTrain(actions));     // first check

    // Nothing again until the timers run out, and no level has changed.
    WorldTickSnapshotView soon = View(2, 0, {});
    soon.bots[0].level = 10;
    CHECK(!HasVendor(policy.Decide(soon, 2000), 7));
    CHECK(!HasTrain(policy.Decide(soon, 2000)));

    // A level-up checks the trainer at once, even though the timer has not run out.
    WorldTickSnapshotView levelled = View(3, 0, {});
    levelled.bots[0].level = 11;
    CHECK(HasTrain(policy.Decide(levelled, 3000)));

    // The vendor visit repeats on its own timer.
    WorldTickSnapshotView later = View(4, 0, {});
    later.bots[0].level = 11;
    CHECK(HasVendor(policy.Decide(later, 601000), 7));
}

void NoServicesInAFight()
{
    BotPolicy policy;
    WorldTickSnapshotView fighting = View(1, MakeUnitRef(8, 1, 2), { Hostile(2, 3.0f, 100) });
    fighting.bots[0].level = 10;
    auto actions = policy.Decide(fighting, 1000);
    CHECK(!HasVendor(actions, 7));
    CHECK(!HasTrain(actions));
}

void HostilePlayersAreNotAttacked()
{
    // Another bot of the other faction, hostile and alive, three yards away: the bot does not attack it.
    BotPolicy policy;
    NearbyUnit rival = Unit(2, 7, 3.0f, true, 100); // player (type 2), hostile
    rival.max_health = 100;
    auto actions = policy.Decide(View(1, 0, { rival }), 1000);
    for (BotAction const& action : actions)
        CHECK(action.type != ActionType::kAttackTarget);

    // A creature at the same range is attacked.
    BotPolicy creature;
    auto mob = creature.Decide(View(1, 0, { Hostile(2, 3.0f, 100) }), 1000);
    bool attacked = false;
    for (BotAction const& action : mob)
        attacked = attacked || action.type == ActionType::kAttackTarget;
    CHECK(attacked);
}

// A policy for one class with its rotation on, and no per-bot class table.
BotPolicy ClassPolicy(std::uint32_t classId)
{
    PolicyOptions options;
    options.ClassId = classId;
    return BotPolicy(options);
}

// The guid of the cast with this spell, or 0 if there is none.
std::uint64_t CastTargetOf(std::vector<BotAction> const& actions, std::uint32_t spell)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kCastSpell && action.spell_id == spell)
            return action.target_guid;
    return 0;
}

void MageCastsFromRange()
{
    // A mage at 12 yards casts Fireball and does not walk into melee.
    auto actions = ClassPolicy(8).Decide(View(1, 0, { Hostile(2, 12.0f, 100) }), 1000);
    CHECK(HasCast(actions, 133));
    for (BotAction const& action : actions)
        CHECK(action.type != ActionType::kFollow);

    // At 30 yards it approaches first.
    auto far = ClassPolicy(8).Decide(View(1, 0, { Hostile(2, 30.0f, 100) }), 1000);
    bool approached = false;
    for (BotAction const& action : far)
        approached = approached || (action.type == ActionType::kFollow && action.target_guid == MakeUnitRef(8, 1, 2));
    CHECK(approached);
}

void WarlockDotIsNotRecastWhileRunning()
{
    BotPolicy policy = ClassPolicy(9);
    CHECK(HasCast(policy.Decide(View(1, 0, { Hostile(2, 12.0f, 100) }), 1000), 172)); // Corruption

    // Corruption landed at 2600 ms and runs 18 s: the next cast is Immolate, not Corruption.
    WorldTickSnapshotView landed = View(2, 0, { Hostile(2, 12.0f, 100) });
    landed.bots[0].last_action_sequence = 1;
    landed.bots[0].last_action_type = uint8_t(ActionType::kCastSpell);
    landed.bots[0].last_action_spell = 172;
    landed.bots[0].last_action_status = BotActionStatus::kApplied;
    auto actions = policy.Decide(landed, 2600);
    CHECK(!HasCast(actions, 172));
    CHECK(HasCast(actions, 707));
}

void PriestShieldsItselfWhenHurt()
{
    // Hurt to 40%, with a mob at 12 yards: Power Word: Shield goes on the bot itself (target 0).
    auto actions = ClassPolicy(5).Decide(Hurt(400, { Hostile(2, 12.0f, 100) }, MakeUnitRef(8, 1, 2)), 1000);
    CHECK(HasCast(actions, 17));
    CHECK(CastTargetOf(actions, 17) == 0);
}

void RogueKicksACastingTarget()
{
    NearbyUnit caster = Hostile(2, 3.0f, 100);
    caster.casting = 1;
    auto actions = ClassPolicy(4).Decide(View(1, 0, { caster }), 1000);
    CHECK(HasCast(actions, 1766)); // Kick
}

int main()
{
    MageCastsFromRange();
    WarlockDotIsNotRecastWhileRunning();
    PriestShieldsItselfWhenHurt();
    RogueKicksACastingTarget();
    HostilePlayersAreNotAttacked();
    ServicesWhenIdleOnTimerAndOnLevelUp();
    NoServicesInAFight();
    RestsWhenHurtAndIdle();
    NoRestAtFullHealthOrWithAPlayerNear();
    StandsBeforeAFight();
    RefusedSitIsRetriedLater();
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
