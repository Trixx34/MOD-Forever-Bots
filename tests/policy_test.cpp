// Unit tests for BotPolicy, on hand-built snapshots. No core needed.
#include "bot_policy.h"

#include "combat_rotation.h"

#include <cstdio>
#include <memory>
#include <sstream>
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
    view.bots[0].life_state = health == 0 ? 1 : 0;
    return view;
}

void RetreatsBelowThirtyPercent()
{
    BotPolicy policy;
    // 250 of 1000 is below 30%: a hostile five yards away is what it runs from now, and it keeps running until it is clear
    // of it. Standing still was the whole reply before, and the creature killed the bot where it stood.
    auto actions = policy.Decide(Hurt(250, { Unit(8, 2, 5.0f, true), Unit(2, 7, 12.0f, false) }, MakeUnitRef(8, 1, 9)));
    CHECK(actions.size() == 1);
    if (actions.size() == 1)
        CHECK(actions[0].type == ActionType::kMoveTo);
}

void RetreatFollowsAPlayerWhenNothingNearToFlee()
{
    BotPolicy policy;
    // Nothing close by that can still kill it: it follows the player again, as before.
    auto actions = policy.Decide(Hurt(250, { Unit(2, 7, 12.0f, false) }));
    CHECK(actions.size() == 1);
    if (actions.size() == 1)
    {
        CHECK(actions[0].type == ActionType::kFollow);
        CHECK(actions[0].target_guid == MakeUnitRef(2, 1, 7));
    }
}

void RetreatWithNoPlayerStillFlees()
{
    BotPolicy policy;
    // 100 of 1000 is below 30%: no player to follow, but a hostile five yards away, so it runs.
    auto actions = policy.Decide(Hurt(100, { Unit(8, 2, 5.0f, true) }));
    CHECK(actions.size() == 1);
    if (actions.size() == 1)
        CHECK(actions[0].type == ActionType::kMoveTo);
}

void ThirtyPercentIsNotLow()
{
    BotPolicy policy;
    auto actions = policy.Decide(Hurt(300, { Unit(8, 2, 5.0f, true) }));
    CHECK(actions.size() == 1 && actions[0].type == ActionType::kAttackTarget);
}

void LoneCorpseReleasesAndDoesNotFight()
{
    BotPolicy policy;
    auto const actions = policy.Decide(Hurt(0, { Unit(8, 2, 5.0f, true) }), 1000);
    CHECK(actions.size() == 1 && actions[0].type == ActionType::kReleaseSpirit);
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
    options.WanderWhenIdle = false;       // these tests look at casts and rests: the bot stays where it is
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

// A snapshot of the bot (class 8, a mage) with the result of its last cast set.
WorldTickSnapshotView CastResultView(std::uint64_t tick, std::vector<NearbyUnit> nearby, std::uint64_t sequence, std::uint32_t spell,
                                     BotActionStatus status)
{
    WorldTickSnapshotView view = View(tick, 0, std::move(nearby));
    view.bots[0].last_action_sequence = sequence;
    view.bots[0].last_action_type = uint8_t(ActionType::kCastSpell);
    view.bots[0].last_action_spell = spell;
    view.bots[0].last_action_status = status;
    return view;
}

void NoCastWhileASpellIsInFlight()
{
    // result "bot is casting spell N": a Fireball that has left the caster is still a cast to the core, and cast_remaining_ms is 0 for it.
    BotPolicy policy = ClassPolicy(8);
    WorldTickSnapshotView view = View(1, 0, { Hostile(2, 12.0f, 100) });
    view.bots[0].casting_spell = 133;
    view.bots[0].cast_remaining_ms = 0;
    CHECK(!HasCast(policy.Decide(view, 1000), 133));
    view.bots[0].casting_spell = 0; // landed
    CHECK(HasCast(policy.Decide(view, 1100), 133));
}

void RangedRowsKeepASlackInsideTheirRange()
{
    // result 118 (out of range): the snapshot is old and the target moves, so a row is used two yards inside its range.
    CHECK(HasCast(ClassPolicy(8).Decide(View(1, 0, { Hostile(2, 27.0f, 100) }), 1000), 133));
    CHECK(!HasCast(ClassPolicy(8).Decide(View(1, 0, { Hostile(2, 29.0f, 100) }), 1000), 133));

    // The height counts: 20 yards along the ground and 20 up is 28.3 away.
    NearbyUnit ledge = Hostile(2, 20.0f, 100);
    ledge.pos_z = 20.0f;
    CHECK(!HasCast(ClassPolicy(8).Decide(View(1, 0, { ledge }), 1000), 133));

    // Fire Blast reaches 20 yards, not 30.
    CHECK(!HasCast(ClassPolicy(8).Decide(View(1, 0, { Hostile(2, 19.0f, 20) }), 1000), 2136));
    CHECK(HasCast(ClassPolicy(8).Decide(View(1, 0, { Hostile(2, 15.0f, 20) }), 1000), 2136));
}

void OutOfRangeRefusalMakesTheCasterStandCloser()
{
    BotPolicy policy = ClassPolicy(8);
    CHECK(HasCast(policy.Decide(View(1, 0, { Hostile(2, 20.0f, 100) }), 1000), 133));
    auto const refused = policy.Decide(CastResultView(2, { Hostile(2, 20.0f, 100) }, 1, 133, BotActionStatus::kOutOfRange), 2600);
    bool closer = false;
    for (BotAction const& action : refused)
        closer = closer || (action.type == ActionType::kFollow && action.target_guid == MakeUnitRef(8, 1, 2));
    CHECK(closer); // a mage stands at 25 yards; after the refusal it walks in to 15
}

void CooldownRefusalHoldsEverySpellForAMoment()
{
    // result 91 / 32: usually the global cooldown. The next row is not tried at once and fails the same way.
    BotPolicy policy = ClassPolicy(8);
    CHECK(HasCast(policy.Decide(View(1, 0, { Hostile(2, 10.0f, 20) }), 1000), 2136));
    auto const held = policy.Decide(CastResultView(2, { Hostile(2, 10.0f, 20) }, 1, 2136, BotActionStatus::kOnCooldown), 2600);
    CHECK(!HasCast(held, 133));
    CHECK(HasCast(policy.Decide(View(3, 0, { Hostile(2, 10.0f, 20) }), 3200), 133));
}

void RepeatedLineOfSightRefusalsAvoidTheTarget()
{
    // result 71: closing in is tried first; three refusals in a row and the target is dropped for a while.
    BotPolicy policy = ClassPolicy(8);
    CHECK(HasCast(policy.Decide(View(1, 0, { Hostile(2, 10.0f, 100) }), 1000), 133));
    std::uint64_t now = 4000;
    for (std::uint64_t n = 1; n <= 3; ++n, now += 3000)
        policy.Decide(CastResultView(1 + n, { Hostile(2, 10.0f, 100) }, n, 133, BotActionStatus::kNoLineOfSight), now);
    for (BotAction const& action : policy.Decide(View(9, 0, { Hostile(2, 10.0f, 100) }), now))
        CHECK(action.type != ActionType::kCastSpell && action.target_guid != MakeUnitRef(8, 1, 2));
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

void SpellsAboveTheBotsLevelAreNotCast()
{
    NearbyUnit caster = Hostile(2, 3.0f, 100);
    caster.casting = 1;
    WorldTickSnapshotView low = View(1, 0, { caster });
    low.bots[0].level = 1; // Kick is learned at level 12
    CHECK(!HasCast(ClassPolicy(4).Decide(low, 1000), 1766));
    WorldTickSnapshotView high = View(1, 0, { caster });
    high.bots[0].level = 12;
    CHECK(HasCast(ClassPolicy(4).Decide(high, 1000), 1766));
}

void SealIsNotRecastWhileUp()
{
    // The seal lasts 30 s on the bot: cast once, then not again until it has worn off.
    BotPolicy policy = ClassPolicy(2);
    auto first = policy.Decide(View(1, 0, { Hostile(2, 3.0f, 100) }), 1000);
    CHECK(HasCast(first, 21084));
    CHECK(CastTargetOf(first, 21084) == 0); // on the bot itself

    WorldTickSnapshotView applied = View(2, 0, { Hostile(2, 3.0f, 100) });
    applied.bots[0].last_action_sequence = 1;
    applied.bots[0].last_action_type = uint8_t(ActionType::kCastSpell);
    applied.bots[0].last_action_spell = 21084;
    applied.bots[0].last_action_status = BotActionStatus::kApplied;
    CHECK(!HasCast(policy.Decide(applied, 2600), 21084));
    CHECK(HasCast(policy.Decide(View(3, 0, { Hostile(2, 3.0f, 100) }), 33000), 21084));
}

void EviscerateAtFivePoints()
{
    // Five Sinister Strikes that land on one target give five combo points: the next finisher is Eviscerate.
    BotPolicy policy = ClassPolicy(4);
    CHECK(HasCast(policy.Decide(View(1, 0, { Hostile(2, 3.0f, 100) }), 1000), 1752));
    for (std::uint32_t i = 1; i <= 5; ++i)
    {
        WorldTickSnapshotView landed = View(1 + i, 0, { Hostile(2, 3.0f, 100) });
        landed.bots[0].last_action_sequence = i;
        landed.bots[0].last_action_type = uint8_t(ActionType::kCastSpell);
        landed.bots[0].last_action_spell = 1752;
        landed.bots[0].last_action_status = BotActionStatus::kApplied;
        auto actions = policy.Decide(landed, 1000 + 1600 * i);
        if (i < 5)
            CHECK(HasCast(actions, 1752) && !HasCast(actions, 2098));
        else
            CHECK(HasCast(actions, 2098));
    }
}

void SaberSlashCountsAsAComboPoint()
{
    // Saber Slash (Forever) gives one combo point like Sinister Strike: a Saber Slash and four Sinister Strikes make five on one target.
    PolicyOptions options;
    options.ClassId = 4;
    std::istringstream file("class 4\nrow 2098 combo 5 target range 0 5\nrow 424785 duration 12 always target range 0 5\nrow 1752 always target range 0 5\n");
    std::vector<std::string> warnings;
    options.Rotations = std::make_shared<rotation::RotationData>(rotation::ParseRotationData(file, warnings));
    CHECK(warnings.empty());
    BotPolicy policy(options);
    CHECK(HasCast(policy.Decide(View(1, 0, { Hostile(2, 3.0f, 100) }), 1000), 424785));
    std::uint32_t const spells[] = { 424785, 1752, 1752, 1752, 1752 };
    for (std::uint32_t i = 1; i <= 5; ++i)
    {
        WorldTickSnapshotView landed = View(1 + i, 0, { Hostile(2, 3.0f, 100) });
        landed.bots[0].last_action_sequence = i;
        landed.bots[0].last_action_type = uint8_t(ActionType::kCastSpell);
        landed.bots[0].last_action_spell = spells[i - 1];
        landed.bots[0].last_action_status = BotActionStatus::kApplied;
        auto actions = policy.Decide(landed, 1000 + 1600 * i);
        if (i < 5)
            CHECK(!HasCast(actions, 2098));
        else
            CHECK(HasCast(actions, 2098));
    }
}

void HostilePlayerDoesNotBlockRest()
{
    // A hostile player three yards away is neither followed nor a reason not to sit down.
    BotPolicy policy;
    std::uint32_t spell = 99;
    NearbyUnit rival = Unit(2, 9, 3.0f, true, 100);
    rival.max_health = 100;
    CHECK(RestAt(policy.Decide(Hurt(600, { rival }), 1000), spell) >= 0 && spell == 1);
}

void RangedBotWithoutPowerWalksIn()
{
    // A mage with 5% mana, a mob at 12 yards: it closes to melee range instead of standing at range with nothing to cast.
    WorldTickSnapshotView view = View(1, 0, { Hostile(2, 12.0f, 100) });
    view.bots[0].power = 5;
    view.bots[0].max_power = 100;
    auto actions = ClassPolicy(8).Decide(view, 1000);
    bool followed = false;
    for (BotAction const& action : actions)
        followed = followed || (action.type == ActionType::kFollow && action.target_guid == MakeUnitRef(8, 1, 2));
    CHECK(followed);
}

// Parties (M8). Bots on the origin with four nearby slots each; callers set positions and the party fields.
WorldTickSnapshotView PartyView(std::vector<std::uint64_t> const& guids)
{
    WorldTickSnapshotView view;
    view.tick_number = 1;
    view.max_nearby_units = 4;
    for (std::uint64_t guid : guids)
    {
        BotSnapshot bot;
        bot.guid = guid;
        bot.health = 1000;
        bot.max_health = 1000;
        view.bots.push_back(bot);
    }
    view.nearby.resize(guids.size() * view.max_nearby_units);
    return view;
}

// A walk longer than the mount threshold spends its first tick mounting up: kMount, then kStop instead of the walk. The walk to the
// destination follows on the next tick. The tests below are about where the bot walks, so they read the tick after the mount;
// BotMountsForALongWalk and its neighbours cover the mount itself.
std::vector<BotAction> AfterMount(BotPolicy& policy, WorldTickSnapshotView const& view, std::uint64_t nowMs)
{
    policy.Decide(view, nowMs);
    return policy.Decide(view, nowMs + 1);
}

void AddNearby(WorldTickSnapshotView& view, std::size_t botIndex, NearbyUnit const& unit)
{
    view.nearby[botIndex * view.max_nearby_units + view.bots[botIndex].nearby_count++] = unit;
}

// Bot 12 is the warrior; the other bots are priests (class 5) and so follow it.
std::uint32_t WarriorLeads(std::uint64_t guid)
{
    return guid == 12 ? 1 : 5;
}

BotPolicy PartyPolicy(std::uint32_t size, std::uint32_t count = 1)
{
    PolicyOptions options;
    options.PartySize = size;
    options.PartyCount = count;
    options.ClassOf = WarriorLeads;
    return BotPolicy(options);
}

std::uint64_t PlayerOf(std::uint64_t counter)
{
    return MakeUnitRef(2, 0, counter);
}

bool HasGroup(std::vector<BotAction> const& actions, std::uint64_t bot, std::uint32_t spell, std::uint64_t target = 0)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kGroup && action.bot_guid == bot && action.spell_id == spell &&
            (target == 0 || action.target_guid == target))
            return true;
    return false;
}

bool HasAnyGroup(std::vector<BotAction> const& actions, std::uint64_t bot)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kGroup && action.bot_guid == bot)
            return true;
    return false;
}

// An action of `type` for `bot` with `target` as its target (any target when 0).
bool HasAction(std::vector<BotAction> const& actions, std::uint64_t bot, ActionType type, std::uint64_t target = 0)
{
    for (BotAction const& action : actions)
        if (action.type == type && action.bot_guid == bot && (target == 0 || action.target_guid == target))
            return true;
    return false;
}

void PartyOffByDefault()
{
    auto const actions = PartyPolicy(0).Decide(PartyView({ 10, 11, 12, 13, 14 }), 0);
    for (BotAction const& action : actions)
        CHECK(action.type != ActionType::kGroup);
}

void LeaderInvitesItsMembers()
{
    auto const actions = PartyPolicy(5).Decide(PartyView({ 10, 11, 12, 13, 14 }), 0);
    CHECK(HasGroup(actions, 12, 1, PlayerOf(10)));
    CHECK(HasGroup(actions, 12, 1, PlayerOf(11)));
    CHECK(HasGroup(actions, 12, 1, PlayerOf(13)));
    CHECK(HasGroup(actions, 12, 1, PlayerOf(14)));
    CHECK(!HasGroup(actions, 12, 1, PlayerOf(12))); // not itself
    CHECK(!HasAnyGroup(actions, 10));                // members invite no one
}

void OnlyTheFirstPartyIsFormed()
{
    // Ten bots, two parties of five: the first party's leader (bot 12) invites its members; bots 15..19 are not
    // partied, so nobody invites them.
    std::vector<std::uint64_t> guids = { 10, 11, 12, 13, 14, 15, 16, 17, 18, 19 };
    auto const actions = PartyPolicy(5, 1).Decide(PartyView(guids), 0);
    CHECK(HasGroup(actions, 12, 1, PlayerOf(10)));
    for (std::uint64_t unpartied : { 15, 16, 17, 18, 19 })
        CHECK(!HasAnyGroup(actions, unpartied));
}

void InvitesWaitForTheRetry()
{
    BotPolicy policy = PartyPolicy(5);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    CHECK(HasGroup(policy.Decide(view, 0), 12, 1));
    CHECK(!HasGroup(policy.Decide(view, 1000), 12, 1)); // still waiting for the answer
    CHECK(HasGroup(policy.Decide(view, 16000), 12, 1)); // invited again after the retry
}

void InvitedMemberIsNotInvitedAgain()
{
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    view.bots[0].group_invite_from = PlayerOf(12); // bot 10 has the invite waiting
    CHECK(!HasGroup(PartyPolicy(5).Decide(view, 0), 12, 1, PlayerOf(10)));
}

void MemberAcceptsItsLeadersInvite()
{
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    view.bots[1].group_invite_from = PlayerOf(12);
    auto const actions = PartyPolicy(5).Decide(view, 0);
    CHECK(HasGroup(actions, 11, 2));
    CHECK(!HasAnyGroup(actions, 10));
}

void MemberLeavesAForeignParty()
{
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    view.bots[1].group_role = 1;
    view.bots[1].group_leader_guid = PlayerOf(99);
    CHECK(HasGroup(PartyPolicy(5).Decide(view, 0), 11, 3));

    // In its own party already: nothing to do.
    view.bots[1].group_leader_guid = PlayerOf(12);
    auto const actions = PartyPolicy(5).Decide(view, 0);
    CHECK(!HasGroup(actions, 11, 3));
    CHECK(!HasAnyGroup(actions, 11));
}

void MemberFollowsItsLeaderUntilClose()
{
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    view.bots[1].pos_x = 30.0f; // bot 11, far from the leader at the origin
    CHECK(HasAction(PartyPolicy(5).Decide(view, 0), 11, ActionType::kFollow, PlayerOf(12)));

    view.bots[1].pos_x = 3.0f; // close enough: it holds where it is
    CHECK(!HasAction(PartyPolicy(5).Decide(view, 0), 11, ActionType::kFollow));
}

void MemberFightsItsLeadersTarget()
{
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    NearbyUnit const wolf = Hostile(7, 2.0f, 100);
    view.bots[2].threat_target_guid = wolf.guid; // the leader is fighting the wolf
    AddNearby(view, 1, wolf);                    // bot 11 sees it
    auto const actions = PartyPolicy(5).Decide(view, 0);
    // Bot 11 is a priest, and the wolf is 2 yards away: a melee target inside kiting distance, so it backs off (kMoveTo) and casts at
    // the wolf instead of walking into it. Either way it acts on the leader's target; a melee member attacks it outright.
    bool engages = false;
    for (BotAction const& action : actions)
        if (action.bot_guid == 11 && action.target_guid == wolf.guid &&
            (action.type == ActionType::kAttackTarget || action.type == ActionType::kCastSpell))
            engages = true;
    CHECK(engages);
}

void NoPartyActionsInCombat()
{
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    view.bots[2].in_combat = 1;
    CHECK(!HasGroup(PartyPolicy(5).Decide(view, 0), 12, 1));
}

// Travel (M6). The core gives each bot its nearest vendor on its map (layout 6); services are on every 60 s. The vendor
// stands at x = 100.
void SetVendor(WorldTickSnapshotView& view, float x)
{
    for (BotSnapshot& bot : view.bots)
    {
        bot.vendor_x = x;
        bot.vendor_y = 0.0f;
        bot.vendor_z = 0.0f;
        bot.vendor_valid = 1;
    }
}

BotPolicy VendorPolicy(bool party = false)
{
    PolicyOptions options;
    options.ServiceEveryMs = 60000;
    if (party)
    {
        options.PartySize = 5;
        options.PartyCount = 1;
        options.ClassOf = WarriorLeads;
    }
    return BotPolicy(options);
}

void IdleBotWalksToItsVendor()
{
    WorldTickSnapshotView view = PartyView({ 10 });
    SetVendor(view, 100.0f);
    auto const actions = VendorPolicy().Decide(view, 0);
    bool found = false;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kMoveTo && action.bot_guid == 10)
        {
            found = true;
            CHECK(action.dest_x == 100.0f && action.dest_y == 0.0f);
        }
    CHECK(found);
    CHECK(!HasAction(actions, 10, ActionType::kVendor)); // the visit waits until the bot is in reach
}

void WornGearSendsTheBotToTheRepairman()
{
    WorldTickSnapshotView view = PartyView({ 10 });
    SetVendor(view, 100.0f);
    view.bots[0].repair_x = -200.0f;
    view.bots[0].repair_valid = 1;
    view.bots[0].durability_pct = 20;
    BotPolicy policy = VendorPolicy();
    auto const actions = AfterMount(policy, view, 0);
    bool found = false;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kMoveTo && action.bot_guid == 10)
        {
            found = true;
            CHECK(action.dest_x == -200.0f); // the repairman, not the vendor at x = 100
        }
    CHECK(found);
}

void GoodGearGoesToTheNearestVendor()
{
    WorldTickSnapshotView view = PartyView({ 10 });
    SetVendor(view, 100.0f);
    view.bots[0].repair_x = -200.0f;
    view.bots[0].repair_valid = 1;
    view.bots[0].durability_pct = 90;
    auto const actions = VendorPolicy().Decide(view, 0);
    for (BotAction const& action : actions)
        if (action.type == ActionType::kMoveTo && action.bot_guid == 10)
            CHECK(action.dest_x == 100.0f);
}

void VendorVisitWhenInReach()
{
    WorldTickSnapshotView view = PartyView({ 10 });
    SetVendor(view, 100.0f);
    view.bots[0].pos_x = 98.0f; // two yards from the vendor
    auto const actions = VendorPolicy().Decide(view, 0);
    CHECK(HasAction(actions, 10, ActionType::kVendor));
    CHECK(!HasAction(actions, 10, ActionType::kMoveTo));
}

void TravelIsDroppedAfterTheTimeout()
{
    BotPolicy policy = VendorPolicy();
    WorldTickSnapshotView view = PartyView({ 10 });
    SetVendor(view, 100.0f);
    CHECK(HasAction(policy.Decide(view, 0), 10, ActionType::kMoveTo));
    auto const stuck = policy.Decide(view, 200000); // still far away after the timeout
    for (BotAction const& action : stuck)                // the walk to the vendor is over (the bot may take a short walk of its own)
        CHECK(!(action.bot_guid == 10 && action.type == ActionType::kMoveTo && action.dest_x == 100.0f));
    CHECK(!HasAction(stuck, 10, ActionType::kVendor));
}

void WalkingBotDoesNotSit()
{
    WorldTickSnapshotView view = PartyView({ 10 });
    SetVendor(view, 100.0f);
    view.bots[0].health = 500; // hurt enough to sit, if it were idle
    auto const actions = VendorPolicy().Decide(view, 0);
    CHECK(HasAction(actions, 10, ActionType::kMoveTo));
    for (BotAction const& action : actions)
        CHECK(!(action.type == ActionType::kRest && action.spell_id == 1));
}

void NoVendorOnTheMapMeansNoWalk()
{
    // The map has no vendor: the visit is tried where the bot stands (the core refuses it if no NPC is in range).
    auto const actions = VendorPolicy().Decide(PartyView({ 10 }), 0);
    CHECK(!HasAction(actions, 10, ActionType::kMoveTo));
    CHECK(HasAction(actions, 10, ActionType::kVendor));
}

void PartyMembersDoNotWalkAlone()
{
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    SetVendor(view, 100.0f);
    auto const actions = VendorPolicy(true).Decide(view, 0);
    CHECK(!HasAction(actions, 10, ActionType::kMoveTo));
    CHECK(HasAction(actions, 12, ActionType::kMoveTo)); // the leader walks; the members follow it
}

// Hubs (M7). A bot on map 1 with two hubs on that map. Services are off, so the walk is the only move.
BotPolicy HubPolicy()
{
    PolicyOptions options;
    options.HubsByMap[1] = { Point{ 500.0f, 0.0f, 0.0f }, Point{ 900.0f, 0.0f, 0.0f } };
    return BotPolicy(options);
}

WorldTickSnapshotView OnMap(std::uint32_t map)
{
    WorldTickSnapshotView view = PartyView({ 10 });
    view.bots[0].map_id = map;
    return view;
}

// The destination of the bot's walk in the actions, or NaN-free -1 when it does not walk.
float WalkTo(std::vector<BotAction> const& actions, std::uint64_t bot)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kMoveTo && action.bot_guid == bot)
            return action.dest_x;
    return -1.0f;
}

void IdleBotWandersToAHubAfterAWhile()
{
    BotPolicy policy = HubPolicy();
    WorldTickSnapshotView view = OnMap(1);
    CHECK(WalkTo(policy.Decide(view, 0), 10) < 0.0f);     // idle starts now
    CHECK(WalkTo(policy.Decide(view, 29000), 10) < 0.0f);
    CHECK(WalkTo(AfterMount(policy, view, 30000), 10) == 500.0f);
}

void ArrivalMovesOnToTheNextHubAfterAnInterval()
{
    BotPolicy policy = HubPolicy();
    WorldTickSnapshotView view = OnMap(1);
    policy.Decide(view, 0);
    CHECK(WalkTo(AfterMount(policy, view, 30000), 10) == 500.0f); // walking to hub 1
    view.bots[0].pos_x = 495.0f;                              // five yards from it: arrived
    CHECK(WalkTo(policy.Decide(view, 31000), 10) < 0.0f);     // waits at the hub
    CHECK(WalkTo(policy.Decide(view, 60000), 10) < 0.0f);     // not a full interval yet
    // The bot is still mounted from the first walk, so this one starts walking at once: AfterMount would read a tick too late.
    CHECK(WalkTo(policy.Decide(view, 61000), 10) == 900.0f);  // next walk, to hub 2
}

void NoHubsOnTheBotsMapMeansNoWalk()
{
    BotPolicy policy = HubPolicy();
    CHECK(WalkTo(policy.Decide(OnMap(0), 0), 10) < 0.0f);
    // Map 0 has no hubs here: the bot takes a short walk on foot after a quiet spell instead (it never walks to the hubs of map 1).
    float const x = WalkTo(policy.Decide(OnMap(0), 30000), 10);
    CHECK(x > -120.0f && x < 120.0f);
}

void AnythingSeenStopsTheWalk()
{
    BotPolicy policy = HubPolicy();
    WorldTickSnapshotView view = OnMap(1);
    policy.Decide(view, 0);
    CHECK(WalkTo(AfterMount(policy, view, 30000), 10) == 500.0f);
    // A hostile creature comes into view: the bot fights it instead of walking on.
    NearbyUnit const wolf = Hostile(7, 2.0f, 100);
    AddNearby(view, 0, wolf);
    auto const actions = policy.Decide(view, 31000);
    CHECK(WalkTo(actions, 10) < 0.0f);
    CHECK(HasAction(actions, 10, ActionType::kAttackTarget, wolf.guid));
}

void HurtBotRestsInsteadOfWalking()
{
    WorldTickSnapshotView view = OnMap(1);
    view.bots[0].health = 500; // below the rest threshold
    BotPolicy policy = HubPolicy();
    policy.Decide(view, 0);
    CHECK(WalkTo(policy.Decide(view, 30000), 10) < 0.0f);
}

void PartyMembersDoNotWanderAlone()
{
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    for (BotSnapshot& bot : view.bots)
        bot.map_id = 1;
    PolicyOptions options;
    options.HubsByMap[1] = { Point{ 500.0f, 0.0f, 0.0f } };
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = WarriorLeads;
    BotPolicy policy(options);
    policy.Decide(view, 0);
    CHECK(WalkTo(policy.Decide(view, 30000), 10) < 0.0f); // a member: it follows its leader
}

// Refused targets (M7 fix). A bot that is refused an attack, or a cast with no valid target, picks another unit.
void RefusedAttackPicksAnotherTarget()
{
    BotPolicy policy;
    NearbyUnit const wolf = Hostile(7, 2.0f, 100);
    NearbyUnit const boar = Hostile(8, 3.0f, 100);
    WorldTickSnapshotView view = PartyView({ 10 });
    AddNearby(view, 0, wolf);
    AddNearby(view, 0, boar);
    CHECK(HasAction(policy.Decide(view, 0), 10, ActionType::kAttackTarget, wolf.guid)); // the closest first

    view.bots[0].last_action_sequence = 1;
    view.bots[0].last_action_type = static_cast<std::uint8_t>(ActionType::kAttackTarget);
    view.bots[0].last_action_status = BotActionStatus::kRefused;
    auto const actions = policy.Decide(view, 1000);
    CHECK(HasAction(actions, 10, ActionType::kAttackTarget, boar.guid));
    CHECK(!HasAction(actions, 10, ActionType::kAttackTarget, wolf.guid));
}

void InvalidCastTargetIsNotCastAtAgain()
{
    PolicyOptions options;
    options.TestSpell = 585;
    options.CastEveryTicks = 1;
    BotPolicy policy(options);
    NearbyUnit const wolf = Hostile(7, 2.0f, 100);
    WorldTickSnapshotView view = PartyView({ 10 });
    AddNearby(view, 0, wolf);
    CHECK(HasCast(policy.Decide(view, 0), 585));

    view.bots[0].last_action_sequence = 1;
    view.bots[0].last_action_type = static_cast<std::uint8_t>(ActionType::kCastSpell);
    view.bots[0].last_action_spell = 585;
    view.bots[0].last_action_status = BotActionStatus::kInvalidTarget;
    view.tick_number = 2;
    CHECK(!HasCast(policy.Decide(view, 1000), 585)); // the wolf is avoided: no target, no cast
}

// A target the bot cannot reach is given up after kApproachTimeoutMs of walking toward it.
void UnreachableTargetIsGivenUp()
{
    BotPolicy policy;
    NearbyUnit const far = Hostile(7, 30.0f, 100); // 30 yards, beyond melee range
    WorldTickSnapshotView view = PartyView({ 10 });
    AddNearby(view, 0, far);
    CHECK(HasAction(policy.Decide(view, 0), 10, ActionType::kFollow, far.guid));
    CHECK(!HasAction(policy.Decide(view, 29000), 10, ActionType::kStop)); // still walking: not given up yet
    auto const given = policy.Decide(view, 31000);
    CHECK(!HasAction(given, 10, ActionType::kFollow, far.guid));
    CHECK(!HasAction(policy.Decide(view, 32000), 10, ActionType::kFollow, far.guid)); // avoided: not picked again
}

// A target given up again is avoided for longer: the second time, twice kAvoidMs.
void RepeatedlyAvoidedTargetIsLeftLonger()
{
    BotPolicy policy;
    NearbyUnit const trainer = Hostile(7, 30.0f, 100);
    WorldTickSnapshotView view = PartyView({ 10 });
    AddNearby(view, 0, trainer);
    policy.Decide(view, 0);                                      // walking to it
    CHECK(HasAction(policy.Decide(view, 31000), 10, ActionType::kStop)); // given up: avoided for 30 s
    CHECK(HasAction(policy.Decide(view, 61000), 10, ActionType::kFollow, trainer.guid)); // free again: walks back
    CHECK(HasAction(policy.Decide(view, 92000), 10, ActionType::kStop));  // given up the second time: avoided for 60 s
    CHECK(!HasAction(policy.Decide(view, 122000), 10, ActionType::kFollow, trainer.guid)); // still avoided
    CHECK(HasAction(policy.Decide(view, 153000), 10, ActionType::kFollow, trainer.guid)); // free after 60 s
    CHECK(HasAction(policy.Decide(view, 184000), 10, ActionType::kStop));   // third give-up: avoided for 120 s
    CHECK(!HasAction(policy.Decide(view, 300000), 10, ActionType::kFollow, trainer.guid)); // still avoided
    CHECK(HasAction(policy.Decide(view, 305000), 10, ActionType::kFollow, trainer.guid));  // free after 120 s
    CHECK(HasAction(policy.Decide(view, 336000), 10, ActionType::kStop));   // fourth give-up: capped at 120 s, not 240 s
    CHECK(HasAction(policy.Decide(view, 457000), 10, ActionType::kFollow, trainer.guid));  // free after 120 s
}

// Pulls the bot can win: not well above its level, not an elite. A creature already attacking the bot is still fought.
void BotDoesNotPullAboveItsLevel()
{
    NearbyUnit strong = Hostile(7, 2.0f, 100);
    strong.level = 6; // five above a level-1 bot
    NearbyUnit fair = Hostile(8, 6.0f, 100);
    fair.level = 3;   // two above: fine
    WorldTickSnapshotView view = PartyView({ 10 });
    view.bots[0].level = 1;
    AddNearby(view, 0, strong);
    AddNearby(view, 0, fair);
    BotPolicy policy;
    auto const actions = policy.Decide(view, 0);
    CHECK(!HasAction(actions, 10, ActionType::kAttackTarget, strong.guid));
    CHECK(HasAction(actions, 10, ActionType::kFollow, fair.guid) || HasAction(actions, 10, ActionType::kAttackTarget, fair.guid));
}

void BotDoesNotPullAnElite()
{
    NearbyUnit elite = Hostile(7, 2.0f, 100);
    elite.level = 1;
    elite.elite = 1;
    WorldTickSnapshotView view = PartyView({ 10 });
    view.bots[0].level = 1;
    AddNearby(view, 0, elite);
    BotPolicy policy;
    CHECK(!HasAction(policy.Decide(view, 0), 10, ActionType::kAttackTarget, elite.guid));
}

void CreatureAttackingTheBotIsFoughtWhateverItsLevel()
{
    NearbyUnit strong = Hostile(7, 2.0f, 100);
    strong.level = 9;
    WorldTickSnapshotView view = PartyView({ 10 });
    view.bots[0].level = 1;
    view.bots[0].threat_target_guid = strong.guid; // it is already fighting the bot
    AddNearby(view, 0, strong);
    BotPolicy policy;
    CHECK(HasAction(policy.Decide(view, 0), 10, ActionType::kAttackTarget, strong.guid));
}

// Looting (M7). A bot loots the creature it fought once that is dead, close, and the bot is out of combat. Once per corpse.
void BotLootsTheCreatureItKilled()
{
    BotPolicy policy;
    NearbyUnit const alive = Hostile(7, 2.0f, 100);
    WorldTickSnapshotView fight = PartyView({ 10 });
    AddNearby(fight, 0, alive);
    policy.Decide(fight, 0); // it targets the creature

    NearbyUnit dead = Hostile(7, 2.0f, 0); // the same creature, dead
    WorldTickSnapshotView after = PartyView({ 10 });
    AddNearby(after, 0, dead);
    auto const actions = policy.Decide(after, 1000);
    bool looted = false;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kLoot && action.bot_guid == 10)
        {
            looted = true;
            CHECK(action.target_guid == dead.guid);
        }
    CHECK(looted);
    CHECK(!HasAction(policy.Decide(after, 2000), 10, ActionType::kLoot)); // once per corpse
}

void GroupmateDoesNotLootACorpseAlreadyTaken()
{
    // Two members of one group fought the same creature: the core loots the whole corpse for the group, so only one sends the loot.
    BotPolicy policy;
    NearbyUnit const alive = Hostile(7, 2.0f, 100);
    WorldTickSnapshotView fight = PartyView({ 10, 11 });
    for (std::size_t i = 0; i < 2; ++i)
    {
        fight.bots[i].group_leader_guid = 10;
        AddNearby(fight, i, alive);
    }
    policy.Decide(fight, 0);

    NearbyUnit const dead = Hostile(7, 2.0f, 0);
    WorldTickSnapshotView after = PartyView({ 10, 11 });
    for (std::size_t i = 0; i < 2; ++i)
    {
        after.bots[i].group_leader_guid = 10;
        AddNearby(after, i, dead);
    }
    auto const actions = policy.Decide(after, 1000);
    int loots = 0;
    for (BotAction const& action : actions)
        loots += action.type == ActionType::kLoot ? 1 : 0;
    CHECK(loots == 1);
}

void NoLootWhileInCombat()
{
    BotPolicy policy;
    NearbyUnit const alive = Hostile(7, 2.0f, 100);
    WorldTickSnapshotView fight = PartyView({ 10 });
    AddNearby(fight, 0, alive);
    policy.Decide(fight, 0);

    WorldTickSnapshotView after = PartyView({ 10 });
    after.bots[0].in_combat = 1;
    AddNearby(after, 0, Hostile(7, 2.0f, 0));
    CHECK(!HasAction(policy.Decide(after, 1000), 10, ActionType::kLoot));
    after.bots[0].in_combat = 0; // out of combat: now it loots
    CHECK(HasAction(policy.Decide(after, 2000), 10, ActionType::kLoot));
}

void NoLootForACorpseItDidNotFight()
{
    BotPolicy policy;
    WorldTickSnapshotView view = PartyView({ 10 });
    AddNearby(view, 0, Hostile(7, 2.0f, 0)); // dead when first seen, never its target
    CHECK(!HasAction(policy.Decide(view, 0), 10, ActionType::kLoot));
}

void NoLootForADistantCorpse()
{
    BotPolicy policy;
    WorldTickSnapshotView fight = PartyView({ 10 });
    AddNearby(fight, 0, Hostile(7, 2.0f, 100));
    policy.Decide(fight, 0);
    WorldTickSnapshotView after = PartyView({ 10 });
    AddNearby(after, 0, Hostile(7, 30.0f, 0)); // 30 yards: beyond the 25 the bot asks within
    CHECK(!HasAction(policy.Decide(after, 1000), 10, ActionType::kLoot));
}

// A bot in combat with no attack target of its own is being attacked: it fights back, even a creature it would not pull.
void BotFightsBackAgainstAStrongAttacker()
{
    NearbyUnit strong = Hostile(7, 2.0f, 100);
    strong.level = 9;
    strong.elite = 1;
    WorldTickSnapshotView view = PartyView({ 10 });
    view.bots[0].level = 1;
    AddNearby(view, 0, strong);

    BotPolicy calm;
    CHECK(!HasAction(calm.Decide(view, 0), 10, ActionType::kAttackTarget, strong.guid)); // not pulled when the bot is not in combat

    view.bots[0].in_combat = 1; // the same creature is now hitting the bot
    BotPolicy attacked;
    CHECK(HasAction(attacked.Decide(view, 0), 10, ActionType::kAttackTarget, strong.guid));
}

void BotInCombatIgnoresThePackLimit()
{
    WorldTickSnapshotView view = PartyView({ 10 });
    for (std::uint64_t counter = 1; counter <= 4; ++counter) // four creatures within 10 yards of each other: more than the limit of three
        AddNearby(view, 0, Hostile(counter, 2.0f + float(counter) * 0.5f, 100));
    BotPolicy calm;
    bool pulled = false;
    for (BotAction const& action : calm.Decide(view, 0))
        pulled = pulled || action.type == ActionType::kAttackTarget || action.type == ActionType::kFollow;
    CHECK(!pulled); // a pack of four is not pulled

    view.bots[0].in_combat = 1;
    BotPolicy attacked;
    bool fights = false;
    for (BotAction const& action : attacked.Decide(view, 0))
        fights = fights || action.type == ActionType::kAttackTarget;
    CHECK(fights);
}

// Dungeon party control (doc 19): a five-bot party with the route active. Bot 12 is the warrior leader, the others priests.
bool CastsSpell(std::vector<BotAction> const& actions, std::uint64_t bot, std::uint32_t spell, std::uint64_t target = 0)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kCastSpell && action.bot_guid == bot && action.spell_id == spell &&
            (target == 0 || action.target_guid == target))
            return true;
    return false;
}

BotPolicy DungeonPolicy()
{
    BotPolicy policy = PartyPolicy(5);
    policy.SetRouteActive(true);
    return policy;
}

void HealerHealsTheHurtMember()
{
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    view.bots[3].health = 300; // bot 13
    auto const actions = DungeonPolicy().Decide(view, 0);
    CHECK(CastsSpell(actions, 10, 2050, PlayerOf(13)));
}

void HealerHealsItselfWithoutATarget()
{
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    view.bots[0].health = 300;
    auto const actions = DungeonPolicy().Decide(view, 0);
    CHECK(CastsSpell(actions, 10, 2050, 0));
}

void NoHealWhileEveryoneIsHealthy()
{
    auto const actions = DungeonPolicy().Decide(PartyView({ 10, 11, 12, 13, 14 }), 0);
    CHECK(!CastsSpell(actions, 10, 2050));
    CHECK(!CastsSpell(actions, 11, 2050));
}

void LeaderDoesNotPullWhileAMemberIsHurt()
{
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    view.bots[0].health = 500; // 50%: the party is not ready
    AddNearby(view, 2, Unit(8, 7, 20.0f, true));
    auto const actions = DungeonPolicy().Decide(view, 0);
    CHECK(!HasAction(actions, 12, ActionType::kFollow, MakeUnitRef(8, 1, 7)));
    CHECK(!HasAction(actions, 12, ActionType::kAttackTarget));
}

void LeaderPullsWhenThePartyIsReady()
{
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    AddNearby(view, 2, Unit(8, 7, 20.0f, true));
    auto const actions = DungeonPolicy().Decide(view, 0);
    CHECK(HasAction(actions, 12, ActionType::kFollow, MakeUnitRef(8, 1, 7)));
}

void LeaderPullsAfterWaitingTooLong()
{
    BotPolicy policy = DungeonPolicy();
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    view.bots[0].health = 500;
    AddNearby(view, 2, Unit(8, 7, 20.0f, true));
    policy.Decide(view, 1000);
    auto const actions = policy.Decide(view, 1000 + 130000);
    CHECK(HasAction(actions, 12, ActionType::kFollow, MakeUnitRef(8, 1, 7)));
}

void MembersDoNotPullInADungeon()
{
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    AddNearby(view, 0, Unit(8, 7, 20.0f, true));
    auto const actions = DungeonPolicy().Decide(view, 0);
    CHECK(!HasAction(actions, 10, ActionType::kFollow, MakeUnitRef(8, 1, 7)));
    CHECK(!CastsSpell(actions, 10, 585));
}

void MembersAttackedInADungeonDefendThemselves()
{
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    view.bots[0].in_combat = 1;
    AddNearby(view, 0, Unit(8, 7, 20.0f, true));
    auto const actions = DungeonPolicy().Decide(view, 0);
    CHECK(HasAction(actions, 10, ActionType::kFollow, MakeUnitRef(8, 1, 7)) || HasAction(actions, 10, ActionType::kAttackTarget));
}

void MembersWaitForTheTankToHoldTheTarget()
{
    BotPolicy policy = DungeonPolicy();
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    view.bots[2].threat_target_guid = MakeUnitRef(8, 1, 7);
    AddNearby(view, 2, Unit(8, 7, 20.0f, true));
    AddNearby(view, 0, Unit(8, 7, 20.0f, true));
    auto first = policy.Decide(view, 1000);                 // the tank starts the fight
    CHECK(!HasAction(first, 10, ActionType::kFollow, MakeUnitRef(8, 1, 7)));
    auto later = policy.Decide(view, 1000 + 3000);           // a few seconds on, the others join
    CHECK(HasAction(later, 10, ActionType::kFollow, MakeUnitRef(8, 1, 7)) || HasAction(later, 10, ActionType::kAttackTarget));
}

void LineOfSightRefusalClosesTheDistance()
{
    BotPolicy policy = DungeonPolicy();
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    view.bots[0].in_combat = 1;
    view.bots[0].threat_target_guid = MakeUnitRef(8, 1, 7);
    AddNearby(view, 0, Unit(8, 7, 15.0f, true));
    view.bots[0].last_action_type = std::uint8_t(ActionType::kCastSpell);
    view.bots[0].last_action_spell = 585;
    view.bots[0].last_action_status = BotActionStatus::kNoLineOfSight;
    view.bots[0].last_action_sequence = 5;
    auto const actions = policy.Decide(view, 1000);
    CHECK(HasAction(actions, 10, ActionType::kFollow, MakeUnitRef(8, 1, 7))); // a priest would stand at 15 yards; now it walks closer
}
// ---- dead bots: release, corpse run, reclaim, resurrect ----

std::uint32_t AllWarriors(std::uint64_t) { return 1; }

// A five-bot party (12 leads) whose events are collected. classOf defaults to WarriorLeads (priests follow the warrior).
PolicyOptions DeadOptions(std::vector<std::string>* events, std::function<std::uint32_t(std::uint64_t)> classOf = WarriorLeads)
{
    PolicyOptions options;
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = classOf;
    options.HasPortal = true;
    options.PortalMap = 0;
    options.PortalPoint = { 500.0f, 0.0f, 0.0f };
    options.PortalInsideMap = 36;
    if (events)
        options.OnEvent = [events](std::string const& text) { events->push_back(text); };
    return options;
}

void Corpse(BotSnapshot& bot, float x)
{
    bot.life_state = 1;
    bot.health = 0;
    bot.pos_x = x;
    bot.corpse_valid = 1;
    bot.corpse_map = bot.map_id;
    bot.corpse_x = x;
}

void Ghost(BotSnapshot& bot, float x, std::uint32_t corpseMap, float corpseX)
{
    bot.life_state = 2;
    bot.health = 1;
    bot.pos_x = x;
    bot.corpse_valid = 1;
    bot.corpse_map = corpseMap;
    bot.corpse_x = corpseX;
}

bool Logged(std::vector<std::string> const& events, char const* prefix)
{
    for (std::string const& text : events)
        if (text.rfind(prefix, 0) == 0)
            return true;
    return false;
}

void CorpseWaitsForARezzerThenReleases()
{
    BotPolicy policy(DeadOptions(nullptr));
    auto const build = [] { WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 }); Corpse(v.bots[3], 10.0f); return v; };
    CHECK(!HasAction(policy.Decide(build(), 1000), 13, ActionType::kReleaseSpirit));
    CHECK(!HasAction(policy.Decide(build(), 11000), 13, ActionType::kReleaseSpirit));
    CHECK(HasAction(policy.Decide(build(), 21500), 13, ActionType::kReleaseSpirit));
}

void CorpseReleasesAtOnceWithoutARezzer()
{
    std::vector<std::string> events;
    BotPolicy policy(DeadOptions(&events, AllWarriors));
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
    Corpse(v.bots[3], 10.0f);
    CHECK(HasAction(policy.Decide(v, 1000), 13, ActionType::kReleaseSpirit));
    CHECK(Logged(events, "died:") && Logged(events, "release:"));
}

void CorpseReleasesWhenTheWholePartyIsDown()
{
    BotPolicy policy(DeadOptions(nullptr));
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
    for (std::size_t i = 0; i < v.bots.size(); ++i)
        Corpse(v.bots[i], float(i));
    auto const actions = policy.Decide(v, 1000);
    for (std::uint64_t guid = 10; guid <= 14; ++guid)
        CHECK(HasAction(actions, guid, ActionType::kReleaseSpirit));
}

void ReleaseIsHeldInCombatUpToTheCap()
{
    std::vector<std::string> events;
    BotPolicy policy(DeadOptions(&events, AllWarriors));
    auto const build = [] { WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 }); Corpse(v.bots[3], 10.0f); v.bots[1].in_combat = 1; return v; };
    CHECK(!HasAction(policy.Decide(build(), 1000), 13, ActionType::kReleaseSpirit));
    CHECK(!HasAction(policy.Decide(build(), 30000), 13, ActionType::kReleaseSpirit));
    CHECK(Logged(events, "release_held:"));
    CHECK(HasAction(policy.Decide(build(), 62000), 13, ActionType::kReleaseSpirit));
}

void GhostWalksToItsCorpseAndIgnoresCombat()
{
    BotPolicy policy(DeadOptions(nullptr));
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
    Ghost(v.bots[3], 200.0f, 0, 0.0f);
    v.bots[3].in_combat = 1;
    AddNearby(v, 3, Unit(8, 2, 5.0f, true));
    auto const actions = policy.Decide(v, 1000);
    CHECK(HasAction(actions, 13, ActionType::kMoveTo));
    CHECK(!HasAction(actions, 13, ActionType::kAttackTarget));
}

void GhostOnTheOutsideMapWalksToThePortal()
{
    std::vector<std::string> events;
    BotPolicy policy(DeadOptions(&events));
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
    Ghost(v.bots[3], 0.0f, 36, 50.0f);
    auto const actions = policy.Decide(v, 1000);
    bool towardPortal = false;
    for (BotAction const& action : actions)
        towardPortal = towardPortal || (action.type == ActionType::kMoveTo && action.bot_guid == 13 && action.dest_x == 500.0f);
    CHECK(towardPortal);
}

void GhostWithoutAPortalGivesUp()
{
    std::vector<std::string> events;
    PolicyOptions options = DeadOptions(&events);
    options.HasPortal = false;
    BotPolicy policy(options);
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
    Ghost(v.bots[3], 0.0f, 36, 50.0f);
    policy.Decide(v, 1000);
    CHECK(Logged(events, "corpse_run_failed:"));
}

void GhostReclaimsInRadiusAndRetries()
{
    BotPolicy policy(DeadOptions(nullptr));
    auto const build = [](std::uint32_t wait) { WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 }); Ghost(v.bots[3], 0.0f, 0, 10.0f); v.bots[3].reclaim_wait_ms = wait; return v; };
    CHECK(!HasAction(policy.Decide(build(5000), 1000), 13, ActionType::kReclaimCorpse)); // still waiting
    CHECK(HasAction(policy.Decide(build(0), 2000), 13, ActionType::kReclaimCorpse));
    CHECK(!HasAction(policy.Decide(build(0), 3000), 13, ActionType::kReclaimCorpse));    // retried after 3 s, not before
    CHECK(HasAction(policy.Decide(build(0), 5500), 13, ActionType::kReclaimCorpse));
}

void GhostCorpseRunTimesOut()
{
    std::vector<std::string> events;
    BotPolicy policy(DeadOptions(&events));
    auto const build = [] { WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 }); Ghost(v.bots[3], 5000.0f, 0, 0.0f); return v; };
    policy.Decide(build(), 1000);
    policy.Decide(build(), 250000);
    CHECK(Logged(events, "corpse_run_failed:"));
}

void DeadBotAcceptsAResurrectOffer()
{
    BotPolicy policy(DeadOptions(nullptr));
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
    Corpse(v.bots[3], 10.0f);
    v.bots[3].resurrect_from = PlayerOf(10);
    CHECK(HasAction(policy.Decide(v, 1000), 13, ActionType::kResurrect, PlayerOf(10)));
    WorldTickSnapshotView none = PartyView({ 10, 11, 12, 13, 14 });
    Corpse(none.bots[3], 10.0f);
    BotPolicy other(DeadOptions(nullptr));
    CHECK(!HasAction(other.Decide(none, 1000), 13, ActionType::kResurrect));
}

WorldTickSnapshotView CorpseNextToPriest(float x = 10.0f)
{
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
    Corpse(v.bots[3], x);
    for (std::size_t i : { 0, 1, 2, 4 })
    {
        v.bots[i].max_power = 1000;
        v.bots[i].power = 1000;
    }
    return v;
}

void PriestResurrectsTheDeadMember()
{
    BotPolicy policy(DeadOptions(nullptr));
    auto const actions = policy.Decide(CorpseNextToPriest(), 1000);
    CHECK(CastsSpell(actions, 10, 2006, PlayerOf(13)));
    CHECK(!CastsSpell(actions, 11, 2006)); // only the closest rezzer casts
    CHECK(!CastsSpell(actions, 14, 2006));
}

void NoResurrectInCombat()
{
    BotPolicy policy(DeadOptions(nullptr));
    WorldTickSnapshotView v = CorpseNextToPriest();
    v.bots[2].in_combat = 1;
    CHECK(!CastsSpell(policy.Decide(v, 1000), 10, 2006));
}

void NoResurrectBelowTheManaReserve()
{
    BotPolicy policy(DeadOptions(nullptr));
    WorldTickSnapshotView v = CorpseNextToPriest();
    for (std::size_t i : { 0, 1, 4 })
        v.bots[i].power = 300;
    CHECK(!CastsSpell(policy.Decide(v, 1000), 10, 2006));
}

void ResurrectRetriesAfterARefusal()
{
    BotPolicy policy(DeadOptions(nullptr));
    CHECK(CastsSpell(policy.Decide(CorpseNextToPriest(), 1000), 10, 2006));
    WorldTickSnapshotView refused = CorpseNextToPriest();
    refused.bots[0].last_action_type = std::uint8_t(ActionType::kCastSpell);
    refused.bots[0].last_action_spell = 2006;
    refused.bots[0].last_action_status = forever_module::BotActionStatus::kRefused;
    refused.bots[0].last_action_sequence = 1;
    CHECK(!CastsSpell(policy.Decide(refused, 2000), 10, 2006));
    // 5 s after the refusal the next healer in line (or the same) tries again.
    auto const later = policy.Decide(CorpseNextToPriest(), 8000);
    CHECK(CastsSpell(later, 10, 2006) || CastsSpell(later, 11, 2006));
}

void OnlyTheRightClassesAtTheRightLevelsResurrect()
{
    auto const spellOf = [](std::uint32_t classId, std::uint32_t level)
    {
        BotPolicy policy(DeadOptions(nullptr, [classId](std::uint64_t guid) { return guid == 12 ? 1u : classId; }));
        WorldTickSnapshotView v = CorpseNextToPriest();
        for (BotSnapshot& bot : v.bots)
            bot.level = level;
        auto const actions = policy.Decide(v, 1000);
        for (std::uint32_t spell : { 2006u, 7328u, 2008u, 20484u })
            if (CastsSpell(actions, 10, spell))
                return spell;
        return 0u;
    };
    CHECK(spellOf(5, 10) == 2006 && spellOf(5, 9) == 0);
    CHECK(spellOf(2, 12) == 7328 && spellOf(2, 11) == 0);
    CHECK(spellOf(7, 12) == 2008 && spellOf(7, 11) == 0);
    CHECK(spellOf(11, 60) == 0); // druid
}

void HealBeatsResurrect()
{
    BotPolicy policy(DeadOptions(nullptr));
    WorldTickSnapshotView v = CorpseNextToPriest();
    v.bots[4].health = 300; // a hurt member
    auto const actions = policy.Decide(v, 1000);
    CHECK(CastsSpell(actions, 10, 2050) && !CastsSpell(actions, 10, 2006));
}

void PartyWaitIsLoggedForARezAndForAGhost()
{
    std::vector<std::string> events;
    BotPolicy policy(DeadOptions(&events));
    policy.Decide(CorpseNextToPriest(), 1000);
    CHECK(Logged(events, "party_wait:"));
    events.clear();
    BotPolicy second(DeadOptions(&events));
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
    Ghost(v.bots[3], 0.0f, 0, 100.0f);
    second.Decide(v, 1000);
    CHECK(Logged(events, "party_wait:"));
}

// ---- soak diagnosis: stalled fights, grey creatures, mana ----

// A target that cannot be hurt (out of reach): the fight is given up after the watchdog time, and the target is left alone for minutes, not
// seconds, more each time (the soak's mage went back to the same two creatures for an hour and a half).
void StalledTargetIsLeftAloneForMinutes()
{
    NearbyUnit const mob = Hostile(7, 3.0f, 100);
    BotPolicy policy;
    auto const view = [&mob]
    {
        WorldTickSnapshotView v = PartyView({ 10 });
        AddNearby(v, 0, mob);
        v.bots[0].threat_target_guid = mob.guid;
        v.bots[0].in_combat = 1;
        return v;
    };
    std::uint64_t now = 0;
    bool given = false;
    for (; now < 70000 && !given; now += 5000)
        for (BotAction const& action : policy.Decide(view(), now))
            given = given || (action.type == ActionType::kStop && action.bot_guid == 10);
    CHECK(given);                                    // given up well before a minute
    CHECK(now <= 60000);
    // Still avoided four minutes later, free again after five.
    CHECK(!HasAction(policy.Decide(view(), now + 240000), 10, ActionType::kAttackTarget, mob.guid));
    CHECK(HasAction(policy.Decide(view(), now + 310000), 10, ActionType::kAttackTarget, mob.guid));
    // Stalls again: this time it is left alone for ten minutes.
    std::uint64_t later = now + 310000;
    bool second = false;
    for (std::uint64_t t = later; t < later + 60000 && !second; t += 5000)
    {
        for (BotAction const& action : policy.Decide(view(), t))
            second = second || (action.type == ActionType::kStop && action.bot_guid == 10);
        later = t;
    }
    CHECK(second);
    CHECK(!HasAction(policy.Decide(view(), later + 540000), 10, ActionType::kAttackTarget, mob.guid));
    CHECK(HasAction(policy.Decide(view(), later + 610000), 10, ActionType::kAttackTarget, mob.guid));
}

// A late snapshot must not restart the stall timer. Steps of 15 s used to reset it (the gap was over 10 s), so a fight whose health never moved was never given up.
void SparseSnapshotsStillEndAStalledFight()
{
    NearbyUnit const mob = Hostile(7, 3.0f, 100);
    BotPolicy policy;
    auto const view = [&mob]
    {
        WorldTickSnapshotView v = PartyView({ 10 });
        AddNearby(v, 0, mob);
        v.bots[0].threat_target_guid = mob.guid;
        v.bots[0].in_combat = 1;
        return v;
    };
    policy.Decide(view(), 0);
    policy.Decide(view(), 15000);
    policy.Decide(view(), 30000);
    bool given = false;
    for (BotAction const& action : policy.Decide(view(), 60000))
        given = given || (action.type == ActionType::kStop && action.bot_guid == 10);
    CHECK(given);
    CHECK(!HasAction(policy.Decide(view(), 70000), 10, ActionType::kAttackTarget, mob.guid));
}

// A threat guid the snapshot keeps set, but that is not in the nearby list, used to hold the fight open for hours. It is given up like a stall.
void UnseenThreatIsGivenUp()
{
    std::uint64_t const threat = MakeUnitRef(8, 1, 9);
    BotPolicy policy;
    auto const view = [&threat]
    {
        WorldTickSnapshotView v = PartyView({ 10 });
        v.bots[0].threat_target_guid = threat;
        v.bots[0].in_combat = 1;
        v.bots[0].target_distance = 0;
        return v;
    };
    CHECK(HasAction(policy.Decide(view(), 0), 10, ActionType::kAttackTarget, threat));
    auto const given = policy.Decide(view(), 46000);
    CHECK(!HasAction(given, 10, ActionType::kAttackTarget, threat));
    CHECK(!HasAction(given, 10, ActionType::kFollow, threat));
    CHECK(!HasAction(policy.Decide(view(), 50000), 10, ActionType::kAttackTarget, threat));
}

// A creature far enough below the bot gives no experience: it is not pulled, and its presence does not keep the bot from walking on.
void GreyCreatureIsNotPulled()
{
    NearbyUnit grey = Hostile(7, 3.0f, 100);
    grey.level = 3; // seven below a level-10 bot: grey (zero experience from 7 below at level 10)
    NearbyUnit green = Hostile(8, 6.0f, 100);
    green.level = 4; // six below: still gives a little
    WorldTickSnapshotView view = PartyView({ 10 });
    view.bots[0].level = 10;
    AddNearby(view, 0, grey);
    BotPolicy policy;
    auto actions = policy.Decide(view, 0);
    CHECK(!HasAction(actions, 10, ActionType::kAttackTarget, grey.guid) && !HasAction(actions, 10, ActionType::kFollow, grey.guid));

    WorldTickSnapshotView withGreen = PartyView({ 10 });
    withGreen.bots[0].level = 10;
    AddNearby(withGreen, 0, grey);
    AddNearby(withGreen, 0, green);
    BotPolicy other;
    actions = other.Decide(withGreen, 0);
    CHECK(HasAction(actions, 10, ActionType::kAttackTarget, green.guid) || HasAction(actions, 10, ActionType::kFollow, green.guid));

    // The grey creature attacks the bot: it is fought whatever its level.
    WorldTickSnapshotView attacked = PartyView({ 10 });
    attacked.bots[0].level = 10;
    attacked.bots[0].in_combat = 1;
    AddNearby(attacked, 0, grey);
    BotPolicy defender;
    actions = defender.Decide(attacked, 0);
    CHECK(HasAction(actions, 10, ActionType::kAttackTarget, grey.guid) || HasAction(actions, 10, ActionType::kFollow, grey.guid));
}

void GreyCreatureDoesNotKeepTheBotFromExploring()
{
    NearbyUnit grey = Hostile(7, 30.0f, 100);
    grey.level = 1;
    WorldTickSnapshotView view = PartyView({ 10 });
    view.bots[0].level = 20;
    AddNearby(view, 0, grey);
    BotPolicy policy; // no hubs, no grind spots: the bot explores on foot after a quiet spell
    policy.Decide(view, 0);
    bool walks = false;
    for (BotAction const& action : policy.Decide(view, 11000))
        walks = walks || (action.type == ActionType::kMoveTo && action.bot_guid == 10);
    CHECK(walks);
}

// An idle mana user low on mana sits to drink, stands when it has enough, and does not wait for mana that never comes.
WorldTickSnapshotView ManaView(std::uint32_t powerPct)
{
    WorldTickSnapshotView view = PartyView({ 10 });
    view.bots[0].power_type = 0;
    view.bots[0].max_power = 100;
    view.bots[0].power = powerPct;
    return view;
}

void IdleCasterDrinksWhenLowOnMana()
{
    BotPolicy policy;
    std::uint32_t spell = 99;
    CHECK(RestAt(policy.Decide(ManaView(20), 1000), spell) >= 0 && spell == 1); // sits
    CHECK(RestAt(policy.Decide(ManaView(60), 5000), spell) < 0);               // keeps sitting: no new action
    // No walk while it drinks, even after a long quiet spell.
    for (BotAction const& action : policy.Decide(ManaView(70), 40000))
        CHECK(action.type != ActionType::kMoveTo);
    CHECK(RestAt(policy.Decide(ManaView(95), 45000), spell) >= 0 && spell == 0); // enough: stands
}

void ManaRestIsCappedAndNotRepeatedAtOnce()
{
    BotPolicy policy;
    std::uint32_t spell = 99;
    CHECK(RestAt(policy.Decide(ManaView(10), 1000), spell) >= 0 && spell == 1);
    CHECK(RestAt(policy.Decide(ManaView(10), 100000), spell) < 0);              // still drinking
    CHECK(RestAt(policy.Decide(ManaView(10), 125000), spell) >= 0 && spell == 0); // 2 minutes: stands, the mana is not coming back
    CHECK(RestAt(policy.Decide(ManaView(10), 130000), spell) < 0);              // and does not sit again at once
}

void FighterWithoutManaDoesNotDrink()
{
    BotPolicy policy;
    WorldTickSnapshotView view = ManaView(0);
    view.bots[0].power_type = 1; // rage
    std::uint32_t spell = 99;
    CHECK(RestAt(policy.Decide(view, 1000), spell) < 0);
}

// A cast result as the next snapshot reports it: spell, status, with a new sequence number.
void Result(WorldTickSnapshotView& view, std::uint32_t spell, BotActionStatus status, std::uint32_t sequence)
{
    view.bots[0].last_action_sequence = sequence;
    view.bots[0].last_action_type = uint8_t(ActionType::kCastSpell);
    view.bots[0].last_action_spell = spell;
    view.bots[0].last_action_status = status;
}

WorldTickSnapshotView HunterView(std::uint64_t tick, std::uint32_t level, std::vector<NearbyUnit> nearby, std::uint64_t threat = 0)
{
    // A far creature too strong to pull keeps the bot from being idle (after 10 s of nothing in view it explores on foot, and is then not standing).
    NearbyUnit strong = Hostile(99, 35.0f, 100);
    strong.level = 60;
    nearby.push_back(strong);
    WorldTickSnapshotView view = View(tick, threat, std::move(nearby));
    view.bots[0].level = level;
    return view;
}

void AutoShotIsNotReissuedWhileActive()
{
    // Level 1: only Auto Shot is learned. It is cast once; once accepted it is not cast again at the next global cooldown.
    BotPolicy policy = ClassPolicy(3);
    NearbyUnit const mob = Hostile(2, 20.0f, 100);
    CHECK(HasCast(policy.Decide(HunterView(1, 1, { mob }), 1000), 75));

    WorldTickSnapshotView accepted = HunterView(2, 1, { mob }, MakeUnitRef(8, 1, 2));
    Result(accepted, 75, BotActionStatus::kApplied, 1);
    CHECK(!HasCast(policy.Decide(accepted, 2600), 75));
    for (std::uint64_t i = 0; i < 5; ++i)
        CHECK(!HasCast(policy.Decide(HunterView(3 + i, 1, { mob }, MakeUnitRef(8, 1, 2)), 4200 + 1600 * i), 75));

    // A new target: Auto Shot is cast on it.
    NearbyUnit const other = Hostile(3, 20.0f, 100);
    CHECK(HasCast(policy.Decide(HunterView(10, 1, { other }, MakeUnitRef(8, 1, 3)), 20000), 75));
}

void AutoShotIsReissuedAfterTheFightAndWhenRefused()
{
    BotPolicy policy = ClassPolicy(3);
    NearbyUnit const mob = Hostile(2, 20.0f, 100);
    policy.Decide(HunterView(1, 1, { mob }), 1000);
    WorldTickSnapshotView accepted = HunterView(2, 1, { mob }, MakeUnitRef(8, 1, 2));
    Result(accepted, 75, BotActionStatus::kApplied, 1);
    policy.Decide(accepted, 2600);
    // The fight ends (nothing to fight), then a new fight with the same creature reference: shot again.
    policy.Decide(HunterView(3, 1, {}), 5000);
    CHECK(HasCast(policy.Decide(HunterView(4, 1, { mob }, MakeUnitRef(8, 1, 2)), 8000), 75));

    // A refused Auto Shot is not marked active: it is retried.
    BotPolicy refused = ClassPolicy(3);
    refused.Decide(HunterView(1, 1, { mob }), 1000);
    WorldTickSnapshotView no = HunterView(2, 1, { mob }, MakeUnitRef(8, 1, 2));
    Result(no, 75, BotActionStatus::kRefused, 1);
    refused.Decide(no, 2600);
    CHECK(HasCast(refused.Decide(HunterView(3, 1, { mob }, MakeUnitRef(8, 1, 2)), 4000), 75));
}

void AutoShotIsReissuedAfterMoving()
{
    BotPolicy policy = ClassPolicy(3);
    NearbyUnit const mob = Hostile(2, 20.0f, 100);
    policy.Decide(HunterView(1, 1, { mob }), 1000);
    WorldTickSnapshotView accepted = HunterView(2, 1, { mob }, MakeUnitRef(8, 1, 2));
    Result(accepted, 75, BotActionStatus::kApplied, 1);
    policy.Decide(accepted, 2600);
    WorldTickSnapshotView moving = HunterView(3, 1, { mob }, MakeUnitRef(8, 1, 2));
    moving.bots[0].moving = 1; // moving stops Auto Shot
    policy.Decide(moving, 4200);
    CHECK(HasCast(policy.Decide(HunterView(4, 1, { mob }, MakeUnitRef(8, 1, 2)), 5800), 75));
}

void HunterCallsItsPet()
{
    BotPolicy policy = ClassPolicy(3);
    // Below level 10 there is no pet spell.
    CHECK(!HasCast(policy.Decide(HunterView(1, 9, {}), 1000), 883));
    // From level 10: Call Pet when nothing is to be fought, on the bot (the pet is the spell's own target).
    auto actions = policy.Decide(HunterView(2, 10, {}), 2000);
    CHECK(HasCast(actions, 883));
    CHECK(CastTargetOf(actions, 883) == 0);
    // Accepted: not called again.
    WorldTickSnapshotView accepted = HunterView(3, 10, {});
    Result(accepted, 883, BotActionStatus::kApplied, 1);
    CHECK(policy.Decide(accepted, 8000).empty());
    CHECK(policy.Decide(HunterView(4, 10, {}), 20000).empty());
    // Not with a creature to fight.
    CHECK(!HasCast(ClassPolicy(3).Decide(HunterView(1, 10, { Hostile(2, 20.0f, 100) }), 1000), 883));
}

void HunterRevivesADeadPetThenBacksOff()
{
    BotPolicy policy = ClassPolicy(3);
    policy.Decide(HunterView(1, 20, {}), 1000);
    WorldTickSnapshotView refused = HunterView(2, 20, {});
    Result(refused, 883, BotActionStatus::kRefused, 1); // Call Pet refused: the pet is dead
    auto actions = policy.Decide(refused, 7000);
    CHECK(HasCast(actions, 982) && !HasCast(actions, 883));

    // Revive Pet refused too: nothing for 30 s, then Call Pet again.
    WorldTickSnapshotView no = HunterView(3, 20, {});
    Result(no, 982, BotActionStatus::kRefused, 2);
    CHECK(!HasCast(policy.Decide(no, 20000), 982));
    CHECK(policy.Decide(HunterView(4, 20, {}), 30000).empty());
    CHECK(HasCast(policy.Decide(HunterView(5, 20, {}), 51000), 883));

    // A Revive Pet that is accepted ends it.
    BotPolicy ok = ClassPolicy(3);
    ok.Decide(HunterView(1, 20, {}), 1000);
    WorldTickSnapshotView dead = HunterView(2, 20, {});
    Result(dead, 883, BotActionStatus::kRefused, 1);
    ok.Decide(dead, 7000);
    WorldTickSnapshotView revived = HunterView(3, 20, {});
    Result(revived, 982, BotActionStatus::kApplied, 2);
    CHECK(ok.Decide(revived, 20000).empty());
    CHECK(ok.Decide(HunterView(4, 20, {}), 40000).empty());
}

void HunterChecksItsPetAfterAFight()
{
    BotPolicy policy = ClassPolicy(3);
    NearbyUnit const mob = Hostile(2, 20.0f, 100);
    // Pet out (Call Pet accepted), then a fight.
    policy.Decide(HunterView(1, 20, {}), 1000);
    WorldTickSnapshotView out = HunterView(2, 20, {});
    Result(out, 883, BotActionStatus::kApplied, 1);
    policy.Decide(out, 7000);
    policy.Decide(HunterView(3, 20, { mob }, MakeUnitRef(8, 1, 2)), 9000);
    // After the fight Mend Pet checks and heals it.
    auto actions = policy.Decide(HunterView(4, 20, {}), 15000);
    CHECK(HasCast(actions, 136));
    CHECK(CastTargetOf(actions, 136) == 0);
    // Accepted: the pet is there, no more casts.
    WorldTickSnapshotView mended = HunterView(5, 20, {});
    Result(mended, 136, BotActionStatus::kApplied, 2);
    CHECK(policy.Decide(mended, 22000).empty());
    CHECK(policy.Decide(HunterView(6, 20, {}), 30000).empty());

    // Mend Pet refused (no pet to heal): Call Pet follows.
    BotPolicy gone = ClassPolicy(3);
    gone.Decide(HunterView(1, 20, {}), 1000);
    gone.Decide(out, 7000);
    gone.Decide(HunterView(3, 20, { mob }, MakeUnitRef(8, 1, 2)), 9000);
    gone.Decide(HunterView(4, 20, {}), 15000);
    WorldTickSnapshotView no = HunterView(5, 20, {});
    Result(no, 136, BotActionStatus::kRefused, 2);
    CHECK(HasCast(gone.Decide(no, 22000), 883));

    // Below level 12 there is no Mend Pet: Call Pet is the check.
    BotPolicy low = ClassPolicy(3);
    low.Decide(HunterView(1, 10, {}), 1000);
    WorldTickSnapshotView lowOut = HunterView(2, 10, {});
    Result(lowOut, 883, BotActionStatus::kApplied, 1);
    low.Decide(lowOut, 7000);
    low.Decide(HunterView(3, 10, { mob }, MakeUnitRef(8, 1, 2)), 9000);
    CHECK(HasCast(low.Decide(HunterView(4, 10, {}), 15000), 883));
}

void HunterMendsItsPetInALongFight()
{
    BotPolicy policy = ClassPolicy(3);
    NearbyUnit const mob = Hostile(2, 20.0f, 100);
    policy.Decide(HunterView(1, 20, {}), 1000);
    WorldTickSnapshotView out = HunterView(2, 20, {});
    Result(out, 883, BotActionStatus::kApplied, 1);
    policy.Decide(out, 7000);
    // The fight starts at 10 s: no Mend Pet early, one after 15 s, not again within 30 s.
    policy.Decide(HunterView(3, 20, { mob }, MakeUnitRef(8, 1, 2)), 10000);
    CHECK(!HasCast(policy.Decide(HunterView(4, 20, { mob }, MakeUnitRef(8, 1, 2)), 14000), 136));
    CHECK(HasCast(policy.Decide(HunterView(5, 20, { mob }, MakeUnitRef(8, 1, 2)), 26000), 136));
    CHECK(!HasCast(policy.Decide(HunterView(6, 20, { mob }, MakeUnitRef(8, 1, 2)), 30000), 136));
    // Not without mana.
    BotPolicy dry = ClassPolicy(3);
    dry.Decide(HunterView(1, 20, {}), 1000);
    dry.Decide(out, 7000);
    dry.Decide(HunterView(3, 20, { mob }, MakeUnitRef(8, 1, 2)), 10000);
    WorldTickSnapshotView low = HunterView(5, 20, { mob }, MakeUnitRef(8, 1, 2));
    low.bots[0].power = 10;
    low.bots[0].max_power = 100;
    CHECK(!HasCast(dry.Decide(low, 26000), 136));
}

// ---- new behaviour: kiting, fleeing, mounting ----

// A ranged bot with a melee target inside kiting distance backs off instead of attacking.
void RangedBotKitesACloseTarget()
{
    BotPolicy policy = ClassPolicy(8);
    WorldTickSnapshotView view = View(1, MakeUnitRef(8, 1, 2), { Hostile(2, 5.0f, 100) });
    view.bots[0].in_combat = 1;
    auto actions = policy.Decide(view, 1000);
    bool kited = false;
    for (BotAction const& action : actions)
        kited = kited || (action.type == ActionType::kMoveTo && action.target_guid == 0);
    CHECK(kited);
    for (BotAction const& action : actions)
        CHECK(action.type != ActionType::kAttackTarget);
}

// A ranged bot at or beyond kiting distance attacks normally.
void RangedBotAttacksWhenBackAtRange()
{
    BotPolicy policy = ClassPolicy(8);
    WorldTickSnapshotView view = View(1, MakeUnitRef(8, 1, 2), { Hostile(2, 12.0f, 100) });
    view.bots[0].in_combat = 1;
    auto actions = policy.Decide(view, 1000);
    bool attacked = false;
    for (BotAction const& action : actions)
        attacked = attacked || (action.type == ActionType::kAttackTarget && action.target_guid == MakeUnitRef(8, 1, 2));
    CHECK(attacked);
}

// Below 30% health, in combat, threat close: flee away from the threat target.
void HurtBotFleesFromThreat()
{
    WorldTickSnapshotView view = Hurt(250, { Unit(8, 2, 10.0f, true) }, MakeUnitRef(8, 1, 9));
    view.bots[0].in_combat = 1;
    BotPolicy policy;
    auto actions = policy.Decide(view, 1000);
    bool fled = false;
    for (BotAction const& action : actions)
        fled = fled || (action.type == ActionType::kMoveTo && action.target_guid == 0);
    CHECK(fled);
}

// Below 30% health, but the threat is farther than kFleeYards: stop, do not flee.
void HurtBotDoesNotFleeWhenThreatIsFar()
{
    WorldTickSnapshotView view = Hurt(250, { Unit(8, 2, 20.0f, true) }, MakeUnitRef(8, 1, 9));
    view.bots[0].in_combat = 1;
    BotPolicy policy;
    auto actions = policy.Decide(view, 1000);
    for (BotAction const& action : actions)
        CHECK(action.type != ActionType::kMoveTo);
}

// Vendor 200 yards away: above the mount threshold, the bot mounts before walking.
void BotMountsForALongWalk()
{
    WorldTickSnapshotView view = PartyView({ 10 });
    SetVendor(view, 200.0f);
    BotPolicy policy = VendorPolicy();
    auto actions = policy.Decide(view, 0);
    bool mounted = false;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kMount && action.spell_id == 1)
            mounted = true;
    CHECK(mounted);
}

// Vendor 50 yards away: below the mount threshold, the bot walks without mounting.
void BotDoesNotMountForAShortWalk()
{
    WorldTickSnapshotView view = PartyView({ 10 });
    SetVendor(view, 50.0f);
    BotPolicy policy = VendorPolicy();
    auto actions = policy.Decide(view, 0);
    for (BotAction const& action : actions)
        CHECK(action.type != ActionType::kMount);
    bool walks = false;
    for (BotAction const& action : actions)
        walks = walks || (action.type == ActionType::kMoveTo && action.dest_x == 50.0f);
    CHECK(walks);
}

// Already mounted with the vendor close: dismount.
void BotDismountsWhenClose()
{
    WorldTickSnapshotView view = PartyView({ 10 });
    SetVendor(view, 10.0f);
    view.bots[0].last_action_sequence = 1;
    view.bots[0].last_action_type = uint8_t(ActionType::kMount);
    view.bots[0].last_action_spell = 1;
    view.bots[0].last_action_status = BotActionStatus::kApplied;
    BotPolicy policy = VendorPolicy();
    auto actions = policy.Decide(view, 1000);
    bool dismounted = false;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kMount && action.spell_id == 0)
            dismounted = true;
    CHECK(dismounted);
}

// In combat with a target: do not mount even if a vendor is far away.
void BotDoesNotMountInCombat()
{
    WorldTickSnapshotView view = PartyView({ 10 });
    SetVendor(view, 200.0f);
    view.bots[0].in_combat = 1;
    view.bots[0].threat_target_guid = MakeUnitRef(8, 1, 2);
    AddNearby(view, 0, Hostile(2, 5.0f, 100));
    BotPolicy policy = VendorPolicy();
    auto actions = policy.Decide(view, 0);
    for (BotAction const& action : actions)
        CHECK(action.type != ActionType::kMount);
}

// A hostile ten yards away that the bot is not fighting: below 30% health it runs, it does not stand and rest.
void LowHealthFleesFromAHostileItIsNotFighting()
{
    WorldTickSnapshotView view = Hurt(250, { Unit(8, 2, 10.0f, true) });
    BotPolicy policy;
    auto actions = policy.Decide(view, 0);
    CHECK(actions.size() == 1);
    if (actions.size() == 1)
        CHECK(actions[0].type == ActionType::kMoveTo);
}

// ---- weakest target, interrupt sharing, heal sharing, party buffs ----

// A pull is the weakest creature that may be pulled, not the closest: finishing it before the next is more experience per hour.
void WeakestTargetIsPulledFirst()
{
    BotPolicy policy;
    WorldTickSnapshotView view = PartyView({ 10 });
    AddNearby(view, 0, Hostile(7, 20.0f, 20)); // hurt, far away
    AddNearby(view, 0, Hostile(8, 3.0f, 100)); // healthy, close
    auto const actions = policy.Decide(view, 0);
    CHECK(HasAction(actions, 10, ActionType::kFollow, MakeUnitRef(8, 1, 7)));
}

// Two that may equally well be pulled: the closest one. It is 3 yards away, so the bot is already in range and attacks rather than
// walking up (a target still out of range is kFollow: see WeakestTargetIsPulledFirst).
void ClosestBreaksTheTieBetweenEqualTargets()
{
    BotPolicy policy;
    WorldTickSnapshotView view = PartyView({ 10 });
    AddNearby(view, 0, Hostile(7, 12.0f, 100));
    AddNearby(view, 0, Hostile(8, 3.0f, 100));
    auto const actions = policy.Decide(view, 0);
    CHECK(HasAction(actions, 10, ActionType::kAttackTarget, MakeUnitRef(8, 1, 8)));
}

// Two rogues of one party, a target that is casting: one interrupts it and the other keeps building combo points.
void OnlyOneRogueInterruptsACastingTarget()
{
    PolicyOptions options;
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t) { return 4; }; // every bot a rogue
    BotPolicy policy(options);

    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    NearbyUnit caster = Hostile(7, 3.0f, 100);
    caster.casting = 1;
    AddNearby(view, 0, caster);
    view.bots[0].casting_spell = 1766; // bot 10 already has Kick on it
    auto const actions = policy.Decide(view, 0);
    CHECK(!HasCast(actions, 1766)); // a second Kick lands on nothing
}

// One rogue with no party: it interrupts as before.
void RogueWithoutAPartyStillInterrupts()
{
    NearbyUnit caster = Hostile(2, 3.0f, 100);
    caster.casting = 1;
    auto const actions = ClassPolicy(4).Decide(View(1, 0, { caster }), 1000);
    CHECK(HasCast(actions, 1766));
}

// Two priests of one party, one member hurt: one heals it and the others look elsewhere while that heal is in flight.
void TwoHealersDoNotHealTheSameMember()
{
    PolicyOptions options;
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t) { return 5; }; // every bot a priest
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    view.bots[3].health = 300; // bot 13 is hurt
    auto const actions = policy.Decide(view, 0);
    int heals = 0;
    std::uint64_t healed = 0;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kCastSpell && action.spell_id == 2050)
        {
            ++heals;
            healed = action.target_guid;
        }
    CHECK(heals == 1);
    CHECK(healed == PlayerOf(13));
}

// The leader (the tank) is kept alive first while it holds a target, even when someone else is on less health.
void TankIsHealedBeforeTheLowestMember()
{
    PolicyOptions options;
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = WarriorLeads; // bot 12 is the warrior and leads
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    view.bots[2].health = 600; // the leader: 60%, holding a target
    view.bots[3].health = 300; // a member on 30%
    view.bots[2].threat_target_guid = Hostile(7, 3.0f, 100).guid; // the tank is fighting
    AddNearby(view, 2, Hostile(7, 3.0f, 100));
    auto const actions = policy.Decide(view, 0);
    CHECK(CastsSpell(actions, 10, 2050, PlayerOf(12))); // the tank, not the member
}

// Two druids of one party with one party buff: each member gets it once, however many druids know it.
void TwoDruidsDoNotBothCastThePartyBuff()
{
    std::istringstream file("class 11\nbuff 1126 duration 1800 party\n");
    std::vector<std::string> drop;
    PolicyOptions options;
    options.Rotations = std::make_shared<rotation::RotationData>(rotation::ParseRotationData(file, drop));
    CHECK(drop.empty());
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t) { return 11; };
    BotPolicy policy(options);

    auto const group = [] { return MakeUnitRef(2, 0, 10); }; // the core formed the group, the leader is bot 10
    auto const partied = [&group]
    {
        WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
        for (BotSnapshot& bot : view.bots)
            bot.group_leader_guid = group();
        return view;
    };

    std::size_t onMember11 = 0, total = 0;
    for (std::uint64_t now = 0; now < 12000; now += 3000)
        for (BotAction const& action : policy.Decide(partied(), now))
            if (action.type == ActionType::kCastSpell && action.spell_id == 1126)
            {
                ++total;
                onMember11 += action.target_guid == PlayerOf(11) ? 1 : 0;
            }
    CHECK(total == 5);      // one cast per member
    CHECK(onMember11 == 1); // and not one per druid
}

// A hub walk that timed out is not worth a full idle interval: an unreachable hub (in rock, or in another zone) must not
// cost the bot 30 seconds of standing every time it goes round the list.
void HubTimeoutTriesTheNextHubSoonerThanAnArrival()
{
    BotPolicy policy = HubPolicy();
    WorldTickSnapshotView view = OnMap(1);
    policy.Decide(view, 0);                                   // the idle time starts
    CHECK(WalkTo(AfterMount(policy, view, 30000), 10) == 500.0f); // walking to hub 1
    policy.Decide(view, 31000);                              // unchanged
    CHECK(WalkTo(policy.Decide(view, 210001), 10) < 0.0f);    // dropped after the timeout
    CHECK(WalkTo(AfterMount(policy, view, 215001), 10) == 900.0f); // the next hub, after a short pause
}

// ---- layout 13: auras, pet, lost-target reason, vendor guid ----

// The core says a creature leashed or despawned. It is not worth pulling again at once, so it is avoided like a refused one.
void LeashedTargetIsNotPulledAgain()
{
    NearbyUnit const mob = Hostile(7, 3.0f, 100);
    BotPolicy policy;
    WorldTickSnapshotView fighting = PartyView({ 10 });
    AddNearby(fighting, 0, mob);
    fighting.bots[0].threat_target_guid = mob.guid;
    fighting.bots[0].in_combat = 1;
    CHECK(HasAction(policy.Decide(fighting, 0), 10, ActionType::kAttackTarget, mob.guid));

    // The creature is gone and the core says it leashed: not pulled again for the avoid window.
    WorldTickSnapshotView lost = PartyView({ 10 });
    AddNearby(lost, 0, mob); // still in view, but the bot's threat dropped
    lost.bots[0].in_combat = 0;
    lost.bots[0].last_target_lost_reason = 2; // leashed
    auto const after = policy.Decide(lost, 1000);
    CHECK(!HasAction(after, 10, ActionType::kAttackTarget, mob.guid));
    CHECK(!HasAction(after, 10, ActionType::kFollow, mob.guid));
    // A kill is different: the bot goes straight on to the next creature.
    BotPolicy killer;
    WorldTickSnapshotView killed = PartyView({ 10 });
    killed.bots[0].last_target_lost_reason = 1; // killed
    AddNearby(killed, 0, Hostile(8, 5.0f, 100));
    auto const next = killer.Decide(killed, 1000);
    CHECK(HasAction(next, 10, ActionType::kFollow, MakeUnitRef(8, 1, 8)) ||
          HasAction(next, 10, ActionType::kAttackTarget, MakeUnitRef(8, 1, 8)));
}

// The pet is in the snapshot now, so it can be sent at the target and called back.
void PetIsSentAtTheTargetAndCalledBack()
{
    BotPolicy policy = ClassPolicy(3);
    NearbyUnit const mob = Hostile(2, 20.0f, 100);
    WorldTickSnapshotView fight = HunterView(1, 20, { mob }, MakeUnitRef(8, 1, 2));
    fight.bots[0].pet_state = 1; // out
    auto const first = policy.Decide(fight, 1000);
    CHECK(HasAction(first, kBot, ActionType::kPetCommand, mob.guid)); // sent at this fight's target, once

    WorldTickSnapshotView over = HunterView(2, 20, {}, 0);
    over.bots[0].pet_state = 1;
    auto const after = policy.Decide(over, 5000);
    CHECK(HasAction(after, kBot, ActionType::kPetCommand)); // called back
    CHECK(!HasAction(policy.Decide(over, 6000), kBot, ActionType::kPetCommand)); // once, not every tick
}

// A pet the snapshot reports as dead is revived directly: no Call Pet first, and no 30-second backoff.
void DeadPetIsRevivedFromTheSnapshot()
{
    BotPolicy policy = ClassPolicy(3);
    WorldTickSnapshotView view = HunterView(1, 20, {});
    view.bots[0].pet_state = 2; // dead
    auto const actions = policy.Decide(view, 1000);
    CHECK(HasCast(actions, 982) && !HasCast(actions, 883)); // Revive Pet, not Call Pet
}

// A pet that is out and healthy is not touched: no probe, no Mend Pet.
void HealthyPetOutIsLeftAlone()
{
    BotPolicy policy = ClassPolicy(3);
    WorldTickSnapshotView view = HunterView(1, 20, {});
    view.bots[0].pet_state = 1;
    view.bots[0].pet_health = 500;
    view.bots[0].pet_max_health = 500;
    auto const actions = policy.Decide(view, 1000);
    CHECK(!HasCast(actions, 136) && !HasCast(actions, 883) && !HasCast(actions, 982));
}

// A pet out and below the health threshold is healed, from fact rather than from a probe.
void HurtPetIsMendedFromTheSnapshot()
{
    BotPolicy policy = ClassPolicy(3);
    WorldTickSnapshotView view = HunterView(1, 20, {});
    view.bots[0].pet_state = 1;
    view.bots[0].pet_health = 100;
    view.bots[0].pet_max_health = 1000;
    auto const actions = policy.Decide(view, 1000);
    CHECK(HasCast(actions, 136)); // Mend Pet
}

// A buff the bot already carries is read from the snapshot's auras, not from the module's own timer.
void SelfBuffIsReadFromTheAura()
{
    std::istringstream file("class 5\nbuff 17 duration 1800 self\n");
    std::vector<std::string> drop;
    PolicyOptions options;
    options.Rotations = std::make_shared<rotation::RotationData>(rotation::ParseRotationData(file, drop));
    CHECK(drop.empty());
    options.ClassId = 5;
    options.WanderWhenIdle = false;

    WorldTickSnapshotView view = View(1, 0, {});
    view.bots[0].level = 30;
    view.bots[0].aura_count = 1;
    view.bots[0].auras[0].spell_id = 17;      // Power Word: Shield, running
    view.bots[0].auras[0].remaining_ms = 15000;
    auto const actions = BotPolicy(options).Decide(view, 1000);
    CHECK(!HasCast(actions, 17)); // it is up: not cast

    WorldTickSnapshotView gone = View(1, 0, {});
    gone.bots[0].level = 30;
    gone.bots[0].aura_count = 1;
    gone.bots[0].auras[0].spell_id = 17;
    gone.bots[0].auras[0].remaining_ms = 0; // just expired
    CHECK(HasCast(BotPolicy(options).Decide(gone, 1000), 17)); // cast again

    WorldTickSnapshotView none = View(1, 0, {});
    none.bots[0].level = 30;
    CHECK(HasCast(BotPolicy(options).Decide(none, 1000), 17)); // a core that reports no auras: the timer still works
}

// The vendor action carries the NPC's guid now, so buy and repair can be aimed at it.
void VendorCarriesItsGuid()
{
    WorldTickSnapshotView view = PartyView({ 10 });
    SetVendor(view, 100.0f);
    view.bots[0].pos_x = 98.0f; // within reach of the vendor
    view.bots[0].vendor_guid = MakeUnitRef(8, 1234, 77);
    auto const actions = VendorPolicy().Decide(view, 0);
    bool aimed = false;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kVendor && action.bot_guid == 10)
        {
            aimed = true;
            CHECK(action.target_guid == MakeUnitRef(8, 1234, 77));
        }
    CHECK(aimed);
}

int main()
{
    AutoShotIsNotReissuedWhileActive();
    AutoShotIsReissuedAfterTheFightAndWhenRefused();
    AutoShotIsReissuedAfterMoving();
    HunterCallsItsPet();
    HunterRevivesADeadPetThenBacksOff();
    HunterChecksItsPetAfterAFight();
    HunterMendsItsPetInALongFight();
    HealerHealsTheHurtMember();
    HealerHealsItselfWithoutATarget();
    NoHealWhileEveryoneIsHealthy();
    LeaderDoesNotPullWhileAMemberIsHurt();
    LeaderPullsWhenThePartyIsReady();
    LeaderPullsAfterWaitingTooLong();
    MembersDoNotPullInADungeon();
    MembersAttackedInADungeonDefendThemselves();
    MembersWaitForTheTankToHoldTheTarget();
    LineOfSightRefusalClosesTheDistance();
    BotFightsBackAgainstAStrongAttacker();
    BotInCombatIgnoresThePackLimit();
    BotLootsTheCreatureItKilled();
    GroupmateDoesNotLootACorpseAlreadyTaken();
    NoLootWhileInCombat();
    NoLootForACorpseItDidNotFight();
    NoLootForADistantCorpse();
    BotDoesNotPullAboveItsLevel();
    BotDoesNotPullAnElite();
    CreatureAttackingTheBotIsFoughtWhateverItsLevel();
    RepeatedlyAvoidedTargetIsLeftLonger();
    UnreachableTargetIsGivenUp();
    RefusedAttackPicksAnotherTarget();
    InvalidCastTargetIsNotCastAtAgain();
    IdleBotWandersToAHubAfterAWhile();
    ArrivalMovesOnToTheNextHubAfterAnInterval();
    NoHubsOnTheBotsMapMeansNoWalk();
    AnythingSeenStopsTheWalk();
    HurtBotRestsInsteadOfWalking();
    PartyMembersDoNotWanderAlone();
    IdleBotWalksToItsVendor();
    WornGearSendsTheBotToTheRepairman();
    GoodGearGoesToTheNearestVendor();
    VendorVisitWhenInReach();
    TravelIsDroppedAfterTheTimeout();
    WalkingBotDoesNotSit();
    PartyMembersDoNotWalkAlone();
    NoVendorOnTheMapMeansNoWalk();
    PartyOffByDefault();
    LeaderInvitesItsMembers();
    OnlyTheFirstPartyIsFormed();
    InvitesWaitForTheRetry();
    InvitedMemberIsNotInvitedAgain();
    MemberAcceptsItsLeadersInvite();
    MemberLeavesAForeignParty();
    MemberFollowsItsLeaderUntilClose();
    MemberFightsItsLeadersTarget();
    NoPartyActionsInCombat();
    SealIsNotRecastWhileUp();
    EviscerateAtFivePoints();
    SaberSlashCountsAsAComboPoint();
    HostilePlayerDoesNotBlockRest();
    RangedBotWithoutPowerWalksIn();
    MageCastsFromRange();
    NoCastWhileASpellIsInFlight();
    RangedRowsKeepASlackInsideTheirRange();
    OutOfRangeRefusalMakesTheCasterStandCloser();
    CooldownRefusalHoldsEverySpellForAMoment();
    RepeatedLineOfSightRefusalsAvoidTheTarget();
    WarlockDotIsNotRecastWhileRunning();
    PriestShieldsItselfWhenHurt();
    RogueKicksACastingTarget();
    SpellsAboveTheBotsLevelAreNotCast();
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
    RetreatFollowsAPlayerWhenNothingNearToFlee();
    RetreatWithNoPlayerStillFlees();
    ThirtyPercentIsNotLow();
    LoneCorpseReleasesAndDoesNotFight();
    CorpseWaitsForARezzerThenReleases();
    CorpseReleasesAtOnceWithoutARezzer();
    CorpseReleasesWhenTheWholePartyIsDown();
    ReleaseIsHeldInCombatUpToTheCap();
    GhostWalksToItsCorpseAndIgnoresCombat();
    GhostOnTheOutsideMapWalksToThePortal();
    GhostWithoutAPortalGivesUp();
    GhostReclaimsInRadiusAndRetries();
    GhostCorpseRunTimesOut();
    DeadBotAcceptsAResurrectOffer();
    PriestResurrectsTheDeadMember();
    NoResurrectInCombat();
    NoResurrectBelowTheManaReserve();
    ResurrectRetriesAfterARefusal();
    OnlyTheRightClassesAtTheRightLevelsResurrect();
    HealBeatsResurrect();
    PartyWaitIsLoggedForARezAndForAGhost();
    OnlyChangesArePublished();
    StalledTargetIsLeftAloneForMinutes();
    SparseSnapshotsStillEndAStalledFight();
    UnseenThreatIsGivenUp();
    GreyCreatureIsNotPulled();
    GreyCreatureDoesNotKeepTheBotFromExploring();
    IdleCasterDrinksWhenLowOnMana();
    ManaRestIsCappedAndNotRepeatedAtOnce();
    FighterWithoutManaDoesNotDrink();
    RangedBotKitesACloseTarget();
    RangedBotAttacksWhenBackAtRange();
    HurtBotFleesFromThreat();
    HurtBotDoesNotFleeWhenThreatIsFar();
    BotMountsForALongWalk();
    BotDoesNotMountForAShortWalk();
    BotDismountsWhenClose();
    BotDoesNotMountInCombat();
    LeashedTargetIsNotPulledAgain();
    PetIsSentAtTheTargetAndCalledBack();
    DeadPetIsRevivedFromTheSnapshot();
    HealthyPetOutIsLeftAlone();
    HurtPetIsMendedFromTheSnapshot();
    SelfBuffIsReadFromTheAura();
    VendorCarriesItsGuid();
    LowHealthFleesFromAHostileItIsNotFighting();
    WeakestTargetIsPulledFirst();
    ClosestBreaksTheTieBetweenEqualTargets();
    OnlyOneRogueInterruptsACastingTarget();
    RogueWithoutAPartyStillInterrupts();
    TwoHealersDoNotHealTheSameMember();
    TankIsHealedBeforeTheLowestMember();
    TwoDruidsDoNotBothCastThePartyBuff();
    HubTimeoutTriesTheNextHubSoonerThanAnArrival();

    if (failures == 0)
        std::printf("policy_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
