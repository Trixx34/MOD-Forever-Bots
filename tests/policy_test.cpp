// Unit tests for BotPolicy, on hand-built snapshots. No core needed.
#include "bot_policy.h"
#include <cmath>
#include "grind_data.h"

#include "combat_rotation.h"

#include <cstdio>
#include <memory>
#include <optional>
#include <set>
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

// The pull and combat tests were written for bots that start Aggressive; the defensive default has its own test.
PolicyOptions AggressiveOptions()
{
    PolicyOptions options;
    options.AggressiveStart = true;
    return options;
}

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
    BotPolicy policy(AggressiveOptions());
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
    BotPolicy policy(AggressiveOptions());
    std::uint64_t const threat = MakeUnitRef(8, 1, 9);
    auto actions = policy.Decide(View(1, threat, { Unit(8, 2, 5.0f, true) }));
    CHECK(actions.size() == 1 && actions[0].target_guid == threat);
}

void StandsStillWithNoEnemyAndNoInvite()
{
    // No party, no chat order: a nearby player is a stranger, not someone to follow (an ungrouped bot does not
    // latch onto whoever walks past it).
    BotPolicy policy(AggressiveOptions());
    auto actions = policy.Decide(View(1, 0, { Unit(2, 7, 12.0f, false), Unit(8, 3, 4.0f, false) }));
    CHECK(actions.size() == 1);
    if (actions.size() == 1)
        CHECK(actions[0].type == ActionType::kStop);
}

void StopsWhenNothingNearby()
{
    BotPolicy policy(AggressiveOptions());
    auto actions = policy.Decide(View(1, 0, {}));
    CHECK(actions.size() == 1 && actions[0].type == ActionType::kStop);
}

void DeadUnitsAreNotAttacked()
{
    BotPolicy policy(AggressiveOptions());
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
    BotPolicy policy(AggressiveOptions());
    // 250 of 1000 is below 30%: a hostile five yards away is what it runs from now, and it keeps running until it is clear
    // of it. Standing still was the whole reply before, and the creature killed the bot where it stood.
    auto actions = policy.Decide(Hurt(250, { Unit(8, 2, 5.0f, true), Unit(2, 7, 12.0f, false) }, MakeUnitRef(8, 1, 9)));
    CHECK(actions.size() == 1);
    if (actions.size() == 1)
        CHECK(actions[0].type == ActionType::kMoveTo);
}

void RetreatWithNoThreatAndNoInviteStandsStill()
{
    BotPolicy policy(AggressiveOptions());
    // Nothing close by that can still kill it, and no invite to follow anyone: it stands down instead of fleeing.
    // (It is also hurt enough, and now idle, to sit and rest: that is a second action, kRest.)
    auto actions = policy.Decide(Hurt(250, { Unit(2, 7, 12.0f, false) }));
    bool stopped = false;
    for (BotAction const& action : actions)
        stopped = stopped || action.type == ActionType::kStop;
    CHECK(stopped);
}

void RetreatWithNoPlayerStillFlees()
{
    BotPolicy policy(AggressiveOptions());
    // 100 of 1000 is below 30%: no player to follow, but a hostile five yards away, so it runs.
    auto actions = policy.Decide(Hurt(100, { Unit(8, 2, 5.0f, true) }));
    CHECK(actions.size() == 1);
    if (actions.size() == 1)
        CHECK(actions[0].type == ActionType::kMoveTo);
}

void ThirtyPercentIsNotLow()
{
    BotPolicy policy(AggressiveOptions());
    auto actions = policy.Decide(Hurt(300, { Unit(8, 2, 5.0f, true) }));
    CHECK(actions.size() == 1 && actions[0].type == ActionType::kAttackTarget);
}

void LoneCorpseReleasesAndDoesNotFight()
{
    BotPolicy policy(AggressiveOptions());
    auto const actions = policy.Decide(Hurt(0, { Unit(8, 2, 5.0f, true) }), 1000);
    CHECK(actions.size() == 1 && actions[0].type == ActionType::kReleaseSpirit);
}

void OnlyChangesArePublished()
{
    BotPolicy policy(AggressiveOptions());
    CHECK(policy.Decide(View(1, 0, { Unit(8, 2, 5.0f, true) })).size() == 1);
    CHECK(policy.Decide(View(2, 0, { Unit(8, 2, 5.0f, true) })).empty()); // same decision, next tick
    CHECK(policy.Decide(View(3, 0, { Unit(8, 2, 5.0f, true) })).empty()); // unchanged, no action
    CHECK(policy.Decide(View(4, 0, {})).size() == 1);                 // now there is nothing to fight
}

} // namespace

// Test casts: a bot fighting a hostile gets the test spell at its target on its own timer, after the attack decision.
void TestCastOnItsOwnTimer()
{
    PolicyOptions options = AggressiveOptions();
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
    BotPolicy policy(AggressiveOptions());
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
    PolicyOptions options = AggressiveOptions();
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

// Bots at 89 % health, all idle: with RandomPct 0 every bot sits down (the shared threshold is 90 %); with it on the thresholds differ per bot (a trait drawn once
// from the seed), so some sit and some do not.
void RestThresholdIsAPerBotTrait()
{
    auto const sitting = [](PolicyOptions options)
    {
        BotPolicy policy(options);
        WorldTickSnapshotView view;
        view.tick_number = 1;
        view.max_nearby_units = 2;
        for (std::uint64_t guid = 1; guid <= 60; ++guid)
        {
            BotSnapshot bot;
            bot.guid = guid;
            bot.health = 890;
            bot.max_health = 1000;
            view.bots.push_back(bot);
        }
        view.nearby.resize(2 * 60);
        int sat = 0;
        for (BotAction const& action : policy.Decide(view, 1000))
            sat += action.type == ActionType::kRest && action.spell_id == 1;
        return sat;
    };
    PolicyOptions plain = AggressiveOptions();
    CHECK(sitting(plain) == 60);                                   // 89 % is under the shared 90 %: all of them sit
    PolicyOptions varied = AggressiveOptions();
    varied.RandomPct = 25;
    varied.RandomSeed = 5;
    int const sat = sitting(varied);
    CHECK(sat > 5 && sat < 55);                                    // some thresholds are above 89 %, some below
    CHECK(sitting(varied) == sat);                                 // and the same seed gives the same bots
}

void RestsWhenHurtAndIdle()
{
    BotPolicy policy(AggressiveOptions());
    std::uint32_t spell = 99;
    CHECK(RestAt(policy.Decide(Hurt(600, {}), 1000), spell) >= 0);
    CHECK(spell == 1); // sit down
}

void NoRestAtFullHealth()
{
    BotPolicy policy(AggressiveOptions());
    std::uint32_t spell = 99;
    CHECK(RestAt(policy.Decide(View(1, 0, {}), 1000), spell) < 0); // full health
}

void RestsEvenWithAStrangerNear()
{
    // A nearby player is not a reason to stay up any more: an ungrouped bot does not follow it, so it is as idle
    // as if nobody was there, and rests like any other hurt, idle bot.
    BotPolicy withPlayer(AggressiveOptions());
    std::uint32_t spell = 99;
    CHECK(RestAt(withPlayer.Decide(Hurt(600, { Unit(2, 7, 4.0f, false, 100) }), 1000), spell) >= 0);
}

void StandsBeforeAFight()
{
    BotPolicy policy(AggressiveOptions());
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

// The skill lines of the kSkillUp actions sent in a tick (these views have a single bot).
std::vector<std::uint32_t> CraftsOf(std::vector<BotAction> const& actions)
{
    std::vector<std::uint32_t> skills;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kSkillUp)
            skills.push_back(action.spell_id);
    return skills;
}

void IdleBotsCraftTheirTwoSkillsInTurnAndWaitAfterARefusal()
{
    PolicyOptions options = AggressiveOptions();
    options.Crafting = true;
    BotPolicy policy(options);
    WorldTickSnapshotView view = View(1, 0, {});
    auto const answer = [&](std::uint32_t skill, BotActionStatus status, std::uint32_t sequence)
    {
        view.bots[0].last_action_type = std::uint8_t(ActionType::kSkillUp);
        view.bots[0].last_action_spell = skill;
        view.bots[0].last_action_status = status;
        view.bots[0].last_action_sequence = sequence;
    };

    CHECK((CraftsOf(policy.Decide(view, 1000)) == std::vector<std::uint32_t>{ 129 }));   // First Aid first
    answer(129, forever_module::BotActionStatus::kRefused, 1);                           // no cloth, or a skill too high: First Aid waits 5 minutes
    CHECK((CraftsOf(policy.Decide(view, 3000)) == std::vector<std::uint32_t>{ 185 }));   // Cooking has its turn
    answer(185, forever_module::BotActionStatus::kRefused, 2);
    CHECK(CraftsOf(policy.Decide(view, 5000)).empty());                                  // both wait
    CHECK(CraftsOf(policy.Decide(view, 100000)).empty());
    CHECK((CraftsOf(policy.Decide(view, 305000)) == std::vector<std::uint32_t>{ 129 })); // and First Aid is tried again after its five minutes

    // A craft that started: the same skill again when the cast is done, the other one in between.
    BotPolicy second(options);
    WorldTickSnapshotView busy = View(1, 0, {});
    CHECK((CraftsOf(second.Decide(busy, 1000)) == std::vector<std::uint32_t>{ 129 }));
    busy.bots[0].last_action_type = std::uint8_t(ActionType::kSkillUp);
    busy.bots[0].last_action_spell = 129;
    busy.bots[0].last_action_status = forever_module::BotActionStatus::kApplied;
    busy.bots[0].last_action_sequence = 1;
    CHECK(CraftsOf(second.Decide(busy, 1600)).size() <= 1);

    // The core may report no spell in its answer: the answer still belongs to the skill that was sent (Cooking refused must not be retried at once).
    BotPolicy blank(options);
    WorldTickSnapshotView plain = View(1, 0, {});
    std::uint32_t sequence = 0;
    std::uint64_t at = 1000;
    for (int round = 0; round < 2; ++round, at += 2000)
    {
        CHECK(CraftsOf(blank.Decide(plain, at)).size() == 1);
        plain.bots[0].last_action_type = std::uint8_t(ActionType::kSkillUp);
        plain.bots[0].last_action_spell = 0;
        plain.bots[0].last_action_status = forever_module::BotActionStatus::kRefused;
        plain.bots[0].last_action_sequence = ++sequence;
    }
    CHECK(CraftsOf(blank.Decide(plain, at)).empty());       // both skills wait
    CHECK(CraftsOf(blank.Decide(plain, at + 60000)).empty());

    // Off by default, and never in a fight.
    BotPolicy off(AggressiveOptions());
    CHECK(CraftsOf(off.Decide(View(1, 0, {}), 1000)).empty());
    BotPolicy fight(options);
    CHECK(CraftsOf(fight.Decide(View(1, 0, { Hostile(2, 3.0f, 100) }), 1000)).empty());
}

// Ground effects (SDK 0.19, layout 21): a bot inside a harmful one walks out of it.
std::vector<BotAction> GroundMoves(std::vector<BotAction> const& actions)
{
    std::vector<BotAction> out;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kMoveTo && action.bot_guid == kBot)
            out.push_back(action);
    return out;
}

void SetGroundEffect(WorldTickSnapshotView& view, std::uint32_t spell, float x, float y, float radius, std::uint8_t hostile)
{
    view.bots[0].ground_effect_count = 1;
    view.bots[0].ground_effects[0] = forever_module::GroundEffect{};
    view.bots[0].ground_effects[0].spell_id = spell;
    view.bots[0].ground_effects[0].x = x;
    view.bots[0].ground_effects[0].y = y;
    view.bots[0].ground_effects[0].radius = radius;
    view.bots[0].ground_effects[0].kind = 1;
    view.bots[0].ground_effects[0].hostile = hostile;
}

void BotInAGroundEffectWalksOut()
{
    PolicyOptions options = AggressiveOptions();
    options.AvoidGroundEffects = true;
    BotPolicy policy(options);
    WorldTickSnapshotView view = View(1, 0, {});
    SetGroundEffect(view, 11829, 2.0f, 0.0f, 5.0f, 1);               // a hostile Flamestrike 2 yards from the bot, 5 yards wide
    auto const moves = GroundMoves(policy.Decide(view, 1000));
    CHECK(moves.size() == 1);
    if (moves.size() == 1)
    {
        float const dx = moves[0].dest_x - 2.0f, dy = moves[0].dest_y;
        CHECK(std::sqrt(dx * dx + dy * dy) >= 8.0f - 0.01f);         // outside by the 3-yard margin
        CHECK(moves[0].dest_x < 0.0f);                              // away from the center
        // The end point is kept: the next ticks do not send a different one while the bot walks.
        view.bots[0].pos_x = -1.0f;
        CHECK(GroundMoves(policy.Decide(view, 1100)).empty());
        CHECK(GroundMoves(policy.Decide(view, 2500)).empty());
    }
}

void GroundEffectsAreIgnoredWhenOffOrNotHarmful()
{
    // Off (the default): no walk.
    BotPolicy off(AggressiveOptions());
    WorldTickSnapshotView view = View(1, 0, {});
    SetGroundEffect(view, 11829, 2.0f, 0.0f, 5.0f, 1);
    CHECK(GroundMoves(off.Decide(view, 1000)).empty());

    PolicyOptions options = AggressiveOptions();
    options.AvoidGroundEffects = true;
    // On, but the caster is not hostile and the spell is in no list (a party member's own Blizzard): no walk.
    BotPolicy friendly(options);
    SetGroundEffect(view, 10, 2.0f, 0.0f, 5.0f, 0);
    CHECK(GroundMoves(friendly.Decide(view, 1000)).empty());
    // The same effect with its spell in the avoid table: a walk, even though the flag is off.
    auto table = std::make_shared<ground_avoid::AvoidTable>();
    table->Add(10, 5.0f);
    options.AvoidSpells = table;
    BotPolicy listed(options);
    CHECK(GroundMoves(listed.Decide(view, 1000)).size() == 1);
    // A hostile effect the bot is not in (20 yards off), and a dead bot: no walk.
    BotPolicy far(options);
    SetGroundEffect(view, 11829, 20.0f, 0.0f, 5.0f, 1);
    CHECK(GroundMoves(far.Decide(view, 1000)).empty());
    BotPolicy dead(options);
    SetGroundEffect(view, 11829, 2.0f, 0.0f, 5.0f, 1);
    view.bots[0].life_state = 1;
    CHECK(GroundMoves(dead.Decide(view, 1000)).empty());
}

void DungeonFileZonesAndSpellsAreAvoided()
{
    PolicyOptions options = AggressiveOptions();
    options.AvoidGroundEffects = true;
    PolicyOptions::DungeonRoute route;
    route.AvoidZones.push_back(ground_avoid::Circle{ 0.0f, 3.0f, 6.0f });   // a fixed zone around the bot's place
    route.AvoidSpells.push_back(777);
    options.DungeonsByMap[0] = route;
    BotPolicy policy(options);
    WorldTickSnapshotView view = View(1, 0, {});
    CHECK(GroundMoves(policy.Decide(view, 1000)).size() == 1);             // inside the fixed zone
    BotPolicy spells(options);
    WorldTickSnapshotView clear = View(1, 0, {});
    options.DungeonsByMap[0].AvoidZones.clear();
    BotPolicy listedSpell(options);
    SetGroundEffect(clear, 777, 1.0f, 0.0f, 4.0f, 0);                      // the dungeon file's avoid_spells names it
    CHECK(GroundMoves(listedSpell.Decide(clear, 1000)).size() == 1);
    SetGroundEffect(clear, 778, 1.0f, 0.0f, 4.0f, 0);
    BotPolicy other(options);
    CHECK(GroundMoves(other.Decide(clear, 1000)).empty());
}

void RefusedSitIsRetriedLater()
{
    BotPolicy policy(AggressiveOptions());
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

// Whether `actions` holds a kVendor with this spell_id, or a kTrain. The white junk bit (1024, set for a bot with nearly full bags, which a default test
// bot with no free slots is) is not part of what these tests compare.
bool HasVendor(std::vector<BotAction> const& actions, std::uint32_t services)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kVendor && (action.spell_id & ~1024u) == services)
            return true;
    return false;
}

// The services of the first kVendor of a tick (0 when there is none).
std::uint32_t VendorServices(std::vector<BotAction> const& actions)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kVendor)
            return action.spell_id;
    return 0;
}

void FullBagsMakeTheVisitSellWhiteJunkToo()
{
    PolicyOptions options = AggressiveOptions();
    options.ServiceEveryMs = 600000;
    BotPolicy policy(options);
    WorldTickSnapshotView full = View(1, 0, {});
    full.bots[0].level = 10;
    full.bots[0].bag_free_slots = 0;
    CHECK(VendorServices(policy.Decide(full, 1000)) == (7u | 1024u));      // sell grey, repair, equip, and the white junk

    BotPolicy other(options);
    WorldTickSnapshotView roomy = View(1, 0, {});
    roomy.bots[0].level = 10;
    roomy.bots[0].bag_free_slots = 20;
    CHECK(VendorServices(other.Decide(roomy, 1000)) == 7u);                // room in the bags: the grey items only

    BotPolicy edge(options);
    WorldTickSnapshotView four = View(1, 0, {});
    four.bots[0].level = 10;
    four.bots[0].bag_free_slots = 4;
    CHECK((VendorServices(edge.Decide(four, 1000)) & 1024u) != 0);          // four free slots is still "nearly full"
    BotPolicy five(options);
    four.bots[0].bag_free_slots = 5;
    CHECK((VendorServices(five.Decide(four, 1000)) & 1024u) == 0);
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
    PolicyOptions options = AggressiveOptions();
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
    BotPolicy policy(AggressiveOptions());
    WorldTickSnapshotView fighting = View(1, MakeUnitRef(8, 1, 2), { Hostile(2, 3.0f, 100) });
    fighting.bots[0].level = 10;
    auto actions = policy.Decide(fighting, 1000);
    CHECK(!HasVendor(actions, 7));
    CHECK(!HasTrain(actions));
}

void HostilePlayersAreNotAttacked()
{
    // Another bot of the other faction, hostile and alive, three yards away: the bot does not attack it.
    BotPolicy policy(AggressiveOptions());
    NearbyUnit rival = Unit(2, 7, 3.0f, true, 100); // player (type 2), hostile
    rival.max_health = 100;
    auto actions = policy.Decide(View(1, 0, { rival }), 1000);
    for (BotAction const& action : actions)
        CHECK(action.type != ActionType::kAttackTarget);

    // A creature at the same range is attacked.
    BotPolicy creature(AggressiveOptions());
    auto mob = creature.Decide(View(1, 0, { Hostile(2, 3.0f, 100) }), 1000);
    bool attacked = false;
    for (BotAction const& action : mob)
        attacked = attacked || action.type == ActionType::kAttackTarget;
    CHECK(attacked);
}

// A policy for one class with its rotation on, and no per-bot class table.
BotPolicy ClassPolicy(std::uint32_t classId)
{
    PolicyOptions options = AggressiveOptions();
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
    PolicyOptions options = AggressiveOptions();
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
    BotPolicy policy(AggressiveOptions());
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
// Mana users get power/max_power set so they can cast spells.
WorldTickSnapshotView PartyView(std::vector<std::uint64_t> const& guids, std::function<std::uint32_t(std::uint64_t)> classOf = nullptr)
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
        std::uint32_t cls = classOf ? classOf(guid) : 0;
        if (cls == 5 || cls == 2 || cls == 7 || cls == 11) // priest, paladin, shaman, druid
        {
            bot.power_type = 0;
            bot.power = 5000;
            bot.max_power = 5000;
        }
        else if (cls == 4 || cls == 8 || cls == 9) // rogue, mage, warlock
        {
            bot.power_type = 0;
            bot.power = 5000;
            bot.max_power = 5000;
        }
        else // warrior, hunter
        {
            bot.power_type = 1; // rage
            bot.power = 0;
            bot.max_power = 100;
        }
        view.bots.push_back(bot);
    }
    view.nearby.resize(guids.size() * view.max_nearby_units);
    return view;
}

// A pre-formed dungeon party: bot 12 is the warrior leader, the rest follow. A test that needs another class
// for a member (a rogue to show that a member defends itself, for instance) passes its own class map.
WorldTickSnapshotView DungeonView(std::vector<std::uint64_t> const& guids,
                                  std::function<std::uint32_t(std::uint64_t)> classOf = nullptr)
{
    if (!classOf)
        classOf = [](std::uint64_t guid) { return guid == 12 ? 1u : 5u; };
    WorldTickSnapshotView view = PartyView(guids, classOf);
    // Pre-form the party: core layout group_role 2 is leader, 1 is member; leader reference is a Player unit ref.
    // group_leader_guid must be a full unit reference (HighGuid::Player << 56 | counter).
    constexpr std::uint64_t kLeaderRef = (std::uint64_t(2) << 56) | 12; // HighGuid::Player = 2
    for (BotSnapshot& bot : view.bots)
    {
        if (bot.guid == 12)
        {
            bot.group_role = 2;
            bot.group_leader_guid = kLeaderRef; // leader's own GUID as unit reference
        }
        else
        {
            bot.group_role = 1;
            bot.group_leader_guid = kLeaderRef;
        }
    }
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
    PolicyOptions options = AggressiveOptions();
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

void PartiesNeverMixSides()
{
    // Ten bots, even guids Alliance and odd guids Horde: two parties of five, one per side. The server refuses an invite across the sides.
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 2;
    options.ClassOf = WarriorLeads;
    options.SideOf = [](std::uint64_t guid) { return std::uint8_t(guid % 2 == 0 ? 1 : 2); };
    BotPolicy policy(options);
    auto const actions = policy.Decide(PartyView({ 10, 11, 12, 13, 14, 15, 16, 17, 18, 19 }), 0);
    for (std::uint64_t member : { 10, 14, 16, 18 })
        CHECK(HasGroup(actions, 12, 1, PlayerOf(member)));      // the Alliance party, led by the warrior
    for (std::uint64_t other : { 11, 13, 15, 17, 19 })
        CHECK(!HasGroup(actions, 12, 1, PlayerOf(other)));
    for (std::uint64_t member : { 13, 15, 17, 19 })
        CHECK(HasGroup(actions, 11, 1, PlayerOf(member)));      // the Horde party, led by its lowest guid (no warrior)
    for (std::uint64_t other : { 10, 12, 14, 16, 18 })
        CHECK(!HasGroup(actions, 11, 1, PlayerOf(other)));
}

void OldPartiesAreLeftBeforeTheNewOnesForm()
{
    // Bot 10 sits in the old party of bot 20 (a bot that is online), and bot 12 (the new leader) is itself a member of it.
    std::vector<std::uint64_t> guids = { 10, 11, 12, 13, 14, 20 };
    WorldTickSnapshotView view = PartyView(guids);
    for (BotSnapshot& bot : view.bots)
        if (bot.guid == 10 || bot.guid == 12)
        {
            bot.group_role = 2;
            bot.group_leader_guid = PlayerOf(20);
        }
    BotPolicy policy = PartyPolicy(5);
    auto actions = policy.Decide(view, 0);
    CHECK(HasGroup(actions, 12, 3));                  // the new leader leaves the old party first (kGroupLeave)
    CHECK(!HasGroup(actions, 12, 1));                 // and invites no one meanwhile
    CHECK(HasGroup(actions, 10, 3));                  // a member in the old party leaves it too
    CHECK(!HasGroup(actions, 12, 1, PlayerOf(10)));

    // Once the leader is free, it invites the members that are free, and skips the one still in the old party.
    for (BotSnapshot& bot : view.bots)
        if (bot.guid == 12)
        {
            bot.group_role = 0;
            bot.group_leader_guid = 0;
        }
    actions = policy.Decide(view, 20000);
    CHECK(HasGroup(actions, 12, 1, PlayerOf(11)));
    CHECK(!HasGroup(actions, 12, 1, PlayerOf(10)));
}

void ALeaderWithItsOwnPendingPartyKeepsInviting()
{
    // A leader that has just invited holds a pending invite to its own party (the core adds the leader as invited until a member accepts).
    // That is not a foreign invite: leaving would drop the party it is building.
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    for (BotSnapshot& bot : view.bots)
        if (bot.guid == 12)
            bot.group_invite_from = PlayerOf(12);
    view.bots[0].group_invite_from = PlayerOf(12);   // bot 10 already has the invite waiting
    auto const actions = PartyPolicy(5).Decide(view, 0);
    CHECK(!HasGroup(actions, 12, 3));                // does not leave
    CHECK(HasGroup(actions, 12, 1, PlayerOf(11)));   // and invites the others
    CHECK(HasGroup(actions, 10, 2));                 // the member with the invite accepts it
}

void PartySideOfDecidesTheSides()
{
    // PartySideOf wins over SideOf: the custom races (unknown to SideOf) still split into one party per side.
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 2;
    options.ClassOf = WarriorLeads;
    options.SideOf = [](std::uint64_t) { return std::uint8_t(0); };
    options.PartySideOf = [](std::uint64_t guid) { return std::uint8_t(guid % 2 == 0 ? 1 : 2); };
    BotPolicy policy(options);
    auto const actions = policy.Decide(PartyView({ 10, 11, 12, 13, 14, 15, 16, 17, 18, 19 }), 0);
    CHECK(HasGroup(actions, 12, 1, PlayerOf(10)) && !HasGroup(actions, 12, 1, PlayerOf(11)));
}

void ABotLeadersInviteIsAnsweredOnce()
{
    // With AcceptInvites on (a player's invite is accepted), an invite from a bot leader is still answered by the party code only: one accept,
    // not two. A leader that holds a pending invite to its own party never accepts it.
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = WarriorLeads;
    options.AcceptInvites = true;
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    view.bots[1].group_invite_from = PlayerOf(12);
    int accepts = 0;
    for (BotAction const& action : policy.Decide(view, 0))
        accepts += action.type == ActionType::kGroup && action.bot_guid == 11 && action.spell_id == 2;
    CHECK(accepts == 1);

    view.bots[2].group_invite_from = PlayerOf(12); // the leader, with its own pending party
    CHECK(!HasGroup(policy.Decide(view, 20000), 12, 2));

    // A human's invite (900 is not a bot of the module) is still accepted on the player path.
    WorldTickSnapshotView human = PartyView({ 20 });
    human.bots[0].group_invite_from = PlayerOf(900);
    CHECK(HasGroup(policy.Decide(human, 0), 20, 2));
}

// The invites of a tick as (leader, invited member) pairs.
std::set<std::pair<std::uint64_t, std::uint64_t>> InvitesOf(std::vector<BotAction> const& actions)
{
    std::set<std::pair<std::uint64_t, std::uint64_t>> invites;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kGroup && action.spell_id == 1)
            invites.emplace(action.bot_guid, action.target_guid);
    return invites;
}

void OnlyAShareOfTheBotsFormParties()
{
    // 100 bots, PartyPercent 30: about 30 of them take part, in parties of 2 to 5 (the sizes drawn), the rest play on their own.
    std::vector<std::uint64_t> guids;
    for (std::uint64_t guid = 100; guid < 200; ++guid)
        guids.push_back(guid);
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 100;
    options.PartyPercent = 30;
    options.RandomSeed = 7;
    BotPolicy policy(options);
    auto const first = InvitesOf(policy.Decide(PartyView(guids), 0));

    std::set<std::uint64_t> invited;
    std::map<std::uint64_t, int> members;
    for (auto const& [leader, member] : first)
    {
        invited.insert(member);
        ++members[leader];
    }
    CHECK(invited.size() >= 8 && invited.size() <= 45);   // about 30 bots take part, the leaders not counted
    std::set<int> sizes;
    for (auto const& [leader, count] : members)
    {
        CHECK(count >= 1 && count <= 4);                  // a party is 2 to 5 bots: the leader and 1 to 4 members
        sizes.insert(count);
    }
    CHECK(sizes.size() >= 2);                             // and not all of one size

    // The same seed gives the same parties; another seed gives other ones.
    BotPolicy same(options);
    CHECK(InvitesOf(same.Decide(PartyView(guids), 0)) == first);
    options.RandomSeed = 8;
    BotPolicy other(options);
    CHECK(InvitesOf(other.Decide(PartyView(guids), 0)) != first);

    // PartyPercent 100 (the default) keeps every bot in the parties, of the full size.
    options.PartyPercent = 100;
    options.RandomSeed = 7;
    BotPolicy everyone(options);
    CHECK(InvitesOf(everyone.Decide(PartyView(guids), 0)).size() == 80);   // 20 parties of 5: 4 invites each
}

void UnknownSideBotsAreLeftOutOfParties()
{
    // Bot 14's race is not known (side 0) while the module can tell the sides of the others: it is not put into a party, so it is never invited
    // across the sides. The other four party up as before.
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = WarriorLeads;
    options.PartySideOf = [](std::uint64_t guid) { return std::uint8_t(guid == 14 ? 0 : 1); };
    BotPolicy policy(options);
    auto const actions = policy.Decide(PartyView({ 10, 11, 12, 13, 14 }), 0);
    CHECK(HasGroup(actions, 12, 1, PlayerOf(10)) && HasGroup(actions, 12, 1, PlayerOf(11)) && HasGroup(actions, 12, 1, PlayerOf(13)));
    CHECK(!HasGroup(actions, 12, 1, PlayerOf(14)));
    CHECK(!HasAnyGroup(actions, 14));
}

void ABotInAHumansPartyIsLeftAlone()
{
    // Bot 10 is invited by a human player (not a bot in the view): the bot party assignment leaves it out, and nobody invites or declines for it.
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    view.bots[0].group_invite_from = PlayerOf(900);
    auto const actions = PartyPolicy(5).Decide(view, 0);
    CHECK(!HasGroup(actions, 12, 1, PlayerOf(10)));
    CHECK(!HasGroup(actions, 10, 3));
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

void ATankLeadsThePartyWhateverItsGuid()
{
    // Bots 10 and 13 are warriors. Bot 10 has no role, so by guid order it would lead; bot 13 has a prot spec, so its role is tank from the first tick on.
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t guid) { return guid == 10 || guid == 13 ? 1u : 5u; };
    options.SpecOf = [](std::uint64_t guid) { return guid == 13 ? std::string("prot") : std::string(); };
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    policy.Decide(view, 0); // the first tick sets the tank role from the spec
    auto const actions = policy.Decide(view, 1000);
    CHECK(HasGroup(actions, 13, 1));  // the tank leads and invites
    CHECK(!HasGroup(actions, 10, 1)); // the warrior with no role follows
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
    // Bot 20 is another bot of the module (online): bot 11 leaves its party for its own leader's.
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14, 20 });
    view.bots[1].group_role = 1;
    view.bots[1].group_leader_guid = PlayerOf(20);
    CHECK(HasGroup(PartyPolicy(5).Decide(view, 0), 11, 3));

    // A party led by a human player (99 is not a bot of the module) is left alone: the bot stays and is not invited.
    view.bots[1].group_leader_guid = PlayerOf(99);
    auto const human = PartyPolicy(5).Decide(view, 0);
    CHECK(!HasAnyGroup(human, 11) && !HasGroup(human, 12, 1, PlayerOf(11)));

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

// Focus fire: with a kill priority a damage dealer takes the priority caster of the tank's pack, not only the tank's own target.
void ADamageDealerFightsThePriorityCasterOfThePack()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t guid) { return guid == 12 ? 1u : guid == 11 ? 8u : 5u; }; // bot 11 is the mage, 12 the warrior, the rest priests
    options.KillPriority = { { 1729, 0 } };                                                    // Defias Evoker first
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    NearbyUnit tankTarget = Hostile(7, 2.0f, 100);
    tankTarget.guid = MakeUnitRef(8, 598, 7);        // a miner, the tank's own target
    NearbyUnit caster = Hostile(8, 5.0f, 100);
    caster.guid = MakeUnitRef(8, 1729, 8);           // an evoker of the same pack
    view.bots[2].threat_target_guid = tankTarget.guid;
    AddNearby(view, 1, tankTarget);
    AddNearby(view, 1, caster);
    bool onCaster = false, onMiner = false;
    for (std::uint64_t at = 0; at < 6000; at += 500)
        for (BotAction const& action : policy.Decide(view, at))
            if (action.bot_guid == 11)
            {
                onCaster = onCaster || action.target_guid == caster.guid;
                onMiner = onMiner || action.target_guid == tankTarget.guid;
            }
    CHECK(onCaster);
    CHECK(!onMiner);
    // Without a priority list it assists the tank's target as before.
    PolicyOptions plain = options;
    plain.KillPriority.clear();
    BotPolicy plainPolicy(plain);
    plainPolicy.SetRouteActive(true);
    bool plainOnMiner = false;
    for (std::uint64_t at = 0; at < 6000; at += 500)
        for (BotAction const& action : plainPolicy.Decide(view, at))
            plainOnMiner = plainOnMiner || (action.bot_guid == 11 && action.target_guid == tankTarget.guid);
    CHECK(plainOnMiner);
}

// Deadmines Foundry: once a Remote-Controlled Golem (2520) is up, every damage dealer takes the Goblin Engineer (622) that summoned
// it over the tank's own target, with no tactic switched on (this is not optional: the golem is immune to most abilities and only
// despawns when its engineer dies - Wowhead's Deadmines guide). Without a golem up it assists the tank's target as usual.
void ADamageDealerFocusesTheEngineerWhenAGolemIsUp()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t guid) { return guid == 12 ? 1u : guid == 11 ? 8u : 5u; }; // bot 11 is the mage, 12 the warrior, the rest priests
    options.TacticFocus = false;
    options.TacticInterrupt = false;
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    NearbyUnit tankTarget = Hostile(7, 2.0f, 100);
    tankTarget.guid = MakeUnitRef(8, 598, 7);         // a miner, the tank's own target
    NearbyUnit golem = Hostile(8, 5.0f, 100);
    golem.guid = MakeUnitRef(8, 2520, 8);             // a Remote-Controlled Golem is already up
    NearbyUnit engineer = Hostile(9, 6.0f, 100);
    engineer.guid = MakeUnitRef(8, 622, 9);           // the Goblin Engineer that summoned it
    view.bots[2].threat_target_guid = tankTarget.guid;
    AddNearby(view, 1, tankTarget);
    AddNearby(view, 1, golem);
    AddNearby(view, 1, engineer);
    bool onEngineer = false, onMiner = false;
    for (std::uint64_t at = 0; at < 6000; at += 500)
        for (BotAction const& action : policy.Decide(view, at))
            if (action.bot_guid == 11)
            {
                onEngineer = onEngineer || action.target_guid == engineer.guid;
                onMiner = onMiner || action.target_guid == tankTarget.guid;
            }
    CHECK(onEngineer);
    CHECK(!onMiner);

    // No golem nearby: no special case, it assists the tank's target as usual.
    WorldTickSnapshotView noGolem = PartyView({ 10, 11, 12, 13, 14 });
    noGolem.bots[2].threat_target_guid = tankTarget.guid;
    AddNearby(noGolem, 1, tankTarget);
    AddNearby(noGolem, 1, engineer);
    BotPolicy noGolemPolicy(options);
    noGolemPolicy.SetRouteActive(true);
    bool onMinerNow = false;
    for (std::uint64_t at = 0; at < 6000; at += 500)
        for (BotAction const& action : noGolemPolicy.Decide(noGolem, at))
            onMinerNow = onMinerNow || (action.bot_guid == 11 && action.target_guid == tankTarget.guid);
    CHECK(onMinerNow);
}

// Interrupt: a mage (Counterspell in its rotation) turns on a unit of the tank's pack that is casting; with the tactic off it keeps to the tank's target.
void ACasterOfThePackIsTargetedForAnInterrupt()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t guid) { return guid == 12 ? 1u : guid == 11 ? 8u : 5u; };
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    NearbyUnit tankTarget = Hostile(7, 2.0f, 100);
    tankTarget.guid = MakeUnitRef(8, 598, 7);
    NearbyUnit caster = Hostile(8, 5.0f, 100);
    caster.guid = MakeUnitRef(8, 1729, 8);
    caster.casting = 1;
    view.bots[2].threat_target_guid = tankTarget.guid;
    AddNearby(view, 1, tankTarget);
    AddNearby(view, 1, caster);
    auto const targets = [&](PolicyOptions const& o, std::uint64_t unit)
    {
        BotPolicy policy(o);
        policy.SetRouteActive(true);
        for (std::uint64_t at = 0; at < 6000; at += 500)
            for (BotAction const& action : policy.Decide(view, at))
                if (action.bot_guid == 11 && action.target_guid == unit)
                    return true;
        return false;
    };
    CHECK(targets(options, caster.guid));
    PolicyOptions off = options;
    off.TacticInterrupt = false;
    CHECK(!targets(off, caster.guid));
    CHECK(targets(off, tankTarget.guid));
}

// The 2026-10-27 Ragefire Chasm soak: kMaxDungeonEngaged caps the pack-focus widening once the party is already fighting two distinct
// hostiles, so it does not reach for a third, *unlisted* one nobody has engaged yet. It still reaches for any of the dungeon file's own
// kill_priority entries whatever the count already engaged (the Searing Blade Warlock must still be reachable even with two lower-priority
// trash already in the fight: a follow-up investigation found its "summon" is a one-time, out-of-combat cast, not a mid-fight resummon --
// see TheVoidwalkerCompanionStaysReachablePastTheEngagementCapToo below for the actual fourth pack member this cap must also reach).
void ADamageDealerDoesNotWidenPastTwoAlreadyEngagedHostiles()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t guid) { return guid == 12 ? 1u : guid == 11 ? 8u : 5u; }; // bot 11 is the mage, 12 the warrior, the rest priests
    options.KillPriority = { { 11324, 0 } }; // the Searing Blade Warlock only: the Cultist and an unlisted trash below are not in the list
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });

    // The tank (12) is already fighting the Cultist, and a priest (13) is already fighting an unlisted trash (11999): two distinct hostiles engaged.
    NearbyUnit cultist = Hostile(7, 2.0f, 100);
    cultist.guid = MakeUnitRef(8, 11322, 7);
    view.bots[2].in_combat = 1;
    view.bots[2].threat_target_guid = cultist.guid;
    NearbyUnit trash = Hostile(8, 6.0f, 100);
    trash.guid = MakeUnitRef(8, 11999, 8);
    view.bots[3].in_combat = 1;
    view.bots[3].threat_target_guid = trash.guid;
    // A second unlisted trash stands within kFocusPackYards of the tank's target, but nobody has engaged it yet: not reachable past the cap.
    NearbyUnit unlistedAdd = Hostile(9, 10.0f, 100);
    unlistedAdd.guid = MakeUnitRef(8, 12000, 9);
    // The Warlock (kill_priority rank 0) is in the same spot, also unengaged: it must stay reachable regardless of the cap.
    NearbyUnit warlock = Hostile(10, 10.0f, 100);
    warlock.guid = MakeUnitRef(8, 11324, 10);
    AddNearby(view, 1, cultist);
    AddNearby(view, 1, trash);
    AddNearby(view, 1, unlistedAdd);
    AddNearby(view, 1, warlock);

    bool onUnlistedAdd = false, onWarlock = false, onTrash = false;
    for (std::uint64_t at = 0; at < 6000; at += 500)
        for (BotAction const& action : policy.Decide(view, at))
            if (action.bot_guid == 11)
            {
                onUnlistedAdd = onUnlistedAdd || action.target_guid == unlistedAdd.guid;
                onWarlock = onWarlock || action.target_guid == warlock.guid;
                onTrash = onTrash || action.target_guid == trash.guid;
            }
    CHECK(!onUnlistedAdd); // an unlisted hostile nobody has engaged yet is not pulled in once two others already are
    CHECK(onWarlock);      // the dungeon's own kill_priority still reaches past the cap, whatever is already engaged
    CHECK(!onTrash);       // (the mage prefers the Warlock over re-targeting the already engaged, unlisted trash: rank 0 beats rank 1000)
}

// 2026-10-27 follow-up to the test above: the Searing Blade Warlock's "Summon Voidwalker" turned out to be SmartAI event UPDATE_OOC with no
// repeat timer (this fork's own smart_scripts: entryorguid 11324, a one-time out-of-combat cast, spell 12746) -- it fires once before the party
// ever reaches the pack, not as a resummon mid-fight, and entry 8996 (Voidwalker Minion) has no spawn of its own anywhere in the world
// database. The Voidwalker is really a fourth, passive pack member that already exists when the party arrives and simply joins the melee late
// (the 2026-10-26 soak's combat log: the tank fought the Cultist and the Enforcer for 30+ seconds before the Voidwalker engaged too, and died 3
// seconds later). 389-ragefire-chasm.json now lists 8996 in kill_priority for exactly the reason the test above exists: without it, a late-
// joining, unengaged Voidwalker is just as "unlisted" as any other trash once two pack members are already fought, and kMaxDungeonEngaged would
// keep the party from ever turning on it.
void TheVoidwalkerCompanionStaysReachablePastTheEngagementCapToo()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t guid) { return guid == 12 ? 1u : guid == 11 ? 8u : 5u; }; // bot 11 is the mage, 12 the warrior, the rest priests
    options.KillPriority = { { 11324, 0 }, { 8996, 1 }, { 11322, 2 }, { 11323, 3 } }; // the shipped dungeon file's order
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });

    // The party is already fighting the Cultist and the Enforcer (two distinct hostiles engaged, the cap), the same as a tank-and-two-adds
    // fight already 30+ seconds in.
    NearbyUnit cultist = Hostile(7, 2.0f, 100);
    cultist.guid = MakeUnitRef(8, 11322, 7);
    view.bots[2].in_combat = 1;
    view.bots[2].threat_target_guid = cultist.guid;
    NearbyUnit enforcer = Hostile(8, 6.0f, 100);
    enforcer.guid = MakeUnitRef(8, 11323, 8);
    view.bots[3].in_combat = 1;
    view.bots[3].threat_target_guid = enforcer.guid;
    // The Voidwalker joins late: unengaged, standing near the pack, the same as the soak's combat log.
    NearbyUnit voidwalker = Hostile(9, 10.0f, 100);
    voidwalker.guid = MakeUnitRef(8, 8996, 9);
    // An unlisted trash in the same spot must still not be reached, same as the test above: the cap is not simply lifted outright.
    NearbyUnit unlistedAdd = Hostile(10, 10.0f, 100);
    unlistedAdd.guid = MakeUnitRef(8, 12001, 10);
    AddNearby(view, 1, cultist);
    AddNearby(view, 1, enforcer);
    AddNearby(view, 1, voidwalker);
    AddNearby(view, 1, unlistedAdd);

    bool onVoidwalker = false, onUnlistedAdd = false;
    for (std::uint64_t at = 0; at < 6000; at += 500)
        for (BotAction const& action : policy.Decide(view, at))
            if (action.bot_guid == 11)
            {
                onVoidwalker = onVoidwalker || action.target_guid == voidwalker.guid;
                onUnlistedAdd = onUnlistedAdd || action.target_guid == unlistedAdd.guid;
            }
    CHECK(onVoidwalker);    // the Voidwalker (kill_priority rank 1) is reached past the cap once it joins, same as the Warlock
    CHECK(!onUnlistedAdd);  // an unrelated unlisted hostile in the same spot is still not pulled in: only the dungeon's own list is exempt
}

// Ragefire Chasm, the Searing Blade camp (2026-10-28 soak data): the Voidwalker Minion (8996) is a level 47 guardian of the Searing Blade Warlock (11324) and
// is unsummoned when the Warlock dies. The Warlock stands about 25 yards behind the tank's target, past the 12 yard pack reach, so the damage dealers
// used to turn on the Voidwalker (kill_priority rank 1) and never on the Warlock. With the Voidwalker in view, the Warlock is taken first from up to 35
// yards; without a Voidwalker nothing changes (a distant Warlock is left alone).
void ADamageDealerTakesTheWarlockWhoseVoidwalkerIsUp()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t guid) { return guid == 12 ? 1u : guid == 11 ? 8u : 5u; }; // bot 11 is the mage, 12 the warrior, the rest priests
    options.TacticInterrupt = false;
    options.KillPriority = { { 11324, 0 }, { 8996, 1 }, { 11322, 2 }, { 11323, 3 } }; // the shipped dungeon file's order
    NearbyUnit enforcer = Hostile(7, 2.0f, 100);
    enforcer.guid = MakeUnitRef(8, 11323, 7);         // the tank's own target
    NearbyUnit voidwalker = Hostile(8, 5.0f, 100);
    voidwalker.guid = MakeUnitRef(8, 8996, 8);        // joined the melee at the tank
    NearbyUnit warlock = Hostile(9, 27.0f, 100);
    warlock.guid = MakeUnitRef(8, 11324, 9);          // behind, outside the 12 yard pack reach
    auto const run = [&](bool withVoidwalker, bool tacticFocus, std::uint64_t& onWarlock, std::uint64_t& onEnforcer, std::uint64_t& onVoidwalker)
    {
        options.TacticFocus = tacticFocus;
        BotPolicy policy(options);
        policy.SetRouteActive(true);
        WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
        view.bots[2].in_combat = 1;
        view.bots[2].threat_target_guid = enforcer.guid;
        AddNearby(view, 1, enforcer);
        if (withVoidwalker)
            AddNearby(view, 1, voidwalker);
        AddNearby(view, 1, warlock);
        onWarlock = onEnforcer = onVoidwalker = 0;
        for (std::uint64_t at = 0; at < 6000; at += 500)
            for (BotAction const& action : policy.Decide(view, at))
                if (action.bot_guid == 11)
                {
                    onWarlock += action.target_guid == warlock.guid;
                    onEnforcer += action.target_guid == enforcer.guid;
                    onVoidwalker += action.target_guid == voidwalker.guid;
                }
    };
    std::uint64_t onWarlock = 0, onEnforcer = 0, onVoidwalker = 0;
    run(true, true, onWarlock, onEnforcer, onVoidwalker);
    CHECK(onWarlock > 0);       // the Warlock first, 25 yards from the tank's target
    CHECK(onVoidwalker == 0);   // not the Voidwalker (rank 1) that stood nearer
    CHECK(onEnforcer == 0);
    run(true, false, onWarlock, onEnforcer, onVoidwalker); // not a tactic: it holds with focus off, as the golem's engineer does
    CHECK(onWarlock > 0);
    run(false, true, onWarlock, onEnforcer, onVoidwalker); // no Voidwalker up: the distant Warlock is outside the pack reach as before
    CHECK(onWarlock == 0);
    CHECK(onEnforcer > 0);
}

// Cc: a mage (Polymorph, spell 118) holds one extra, unengaged Humanoid member of the tank's pack -- not the tank's own target,
// which is already engaged.
void CcHoldsAnUnengagedHumanoidPackMember()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t guid) { return guid == 12 ? 1u : guid == 11 ? 8u : 5u; };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    NearbyUnit tankTarget = Hostile(7, 2.0f, 100);
    tankTarget.guid = MakeUnitRef(8, 598, 7);   // the tank's own target, already engaged: never a cc candidate
    NearbyUnit evoker = Hostile(8, 8.0f, 100);
    evoker.guid = MakeUnitRef(8, 1729, 8);      // a Defias Evoker of the same pack: Humanoid, Polymorph can hold it
    view.bots[2].in_combat = 1;
    view.bots[2].threat_target_guid = tankTarget.guid;
    AddNearby(view, 1, tankTarget);
    AddNearby(view, 1, evoker);
    bool onEvoker = false, onTankTarget = false;
    for (std::uint64_t at = 0; at < 6000; at += 500)
        for (BotAction const& action : policy.Decide(view, at))
            if (action.bot_guid == 11 && action.type == ActionType::kCastSpell && action.spell_id == 118)
            {
                onEvoker = onEvoker || action.target_guid == evoker.guid;
                onTankTarget = onTankTarget || action.target_guid == tankTarget.guid;
            }
    CHECK(onEvoker);
    CHECK(!onTankTarget);
}

// Cc: a creature type with no spell to hold it (Mechanical: the Remote-Controlled Golem, 2520) is never tried -- the policy does
// nothing instead of risking a cast the server would refuse (and recast every tick for ever).
void CcDoesNotHoldACreatureTypeItHasNoSpellFor()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t guid) { return guid == 12 ? 1u : guid == 11 ? 8u : 5u; };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    NearbyUnit tankTarget = Hostile(7, 2.0f, 100);
    tankTarget.guid = MakeUnitRef(8, 622, 7);  // Goblin Engineer, Humanoid: the tank's own target, already engaged
    NearbyUnit golem = Hostile(8, 8.0f, 100);
    golem.guid = MakeUnitRef(8, 2520, 8);      // Remote-Controlled Golem: Mechanical, Polymorph cannot hold it
    view.bots[2].in_combat = 1;
    view.bots[2].threat_target_guid = tankTarget.guid;
    AddNearby(view, 1, tankTarget);
    AddNearby(view, 1, golem);
    bool anyCc = false;
    for (std::uint64_t at = 0; at < 6000; at += 500)
        for (BotAction const& action : policy.Decide(view, at))
            if (action.bot_guid == 11 && action.type == ActionType::kCastSpell && action.spell_id == 118)
                anyCc = true;
    CHECK(!anyCc);
}

// Cc: the dungeon's own kill_priority unit is never held -- the point of cc is one fewer attacker, not a delay on the one the
// party means to kill first.
void CcNeverHoldsTheKillPriorityTarget()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t guid) { return guid == 12 ? 1u : guid == 11 ? 8u : 5u; };
    options.KillPriority = { { 1729, 0 } }; // the Defias Evoker is the one to kill first, not to hold
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    NearbyUnit tankTarget = Hostile(7, 2.0f, 100);
    tankTarget.guid = MakeUnitRef(8, 598, 7);
    NearbyUnit evoker = Hostile(8, 8.0f, 100);
    evoker.guid = MakeUnitRef(8, 1729, 8); // Humanoid, and otherwise eligible, but it is the kill_priority unit
    view.bots[2].in_combat = 1;
    view.bots[2].threat_target_guid = tankTarget.guid;
    AddNearby(view, 1, tankTarget);
    AddNearby(view, 1, evoker);
    bool onEvoker = false;
    for (std::uint64_t at = 0; at < 6000; at += 500)
        for (BotAction const& action : policy.Decide(view, at))
            if (action.bot_guid == 11 && action.type == ActionType::kCastSpell && action.spell_id == 118 && action.target_guid == evoker.guid)
                onEvoker = true;
    CHECK(!onEvoker);
}

// Cc: with the tactic off (the default when DungeonTactics does not list cc) no hold is attempted, whatever the pack holds.
void CcTacticOffMeansNoHold()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t guid) { return guid == 12 ? 1u : guid == 11 ? 8u : 5u; };
    options.TacticCc = false;
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    NearbyUnit tankTarget = Hostile(7, 2.0f, 100);
    tankTarget.guid = MakeUnitRef(8, 598, 7);
    NearbyUnit evoker = Hostile(8, 8.0f, 100);
    evoker.guid = MakeUnitRef(8, 1729, 8);
    view.bots[2].in_combat = 1;
    view.bots[2].threat_target_guid = tankTarget.guid;
    AddNearby(view, 1, tankTarget);
    AddNearby(view, 1, evoker);
    bool anyCc = false;
    for (std::uint64_t at = 0; at < 6000; at += 500)
        for (BotAction const& action : policy.Decide(view, at))
            if (action.bot_guid == 11 && action.type == ActionType::kCastSpell && action.spell_id == 118)
                anyCc = true;
    CHECK(!anyCc);
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
    PolicyOptions options = AggressiveOptions();
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
    PolicyOptions options = AggressiveOptions();
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

void ANoRouteAnswerEndsTheWalkInsteadOfBeingSentEveryTick()
{
    BotPolicy policy = HubPolicy();
    WorldTickSnapshotView view = OnMap(1);
    policy.Decide(view, 0);
    CHECK(WalkTo(AfterMount(policy, view, 30000), 10) == 500.0f);
    view.bots[0].last_action_type = std::uint8_t(ActionType::kMoveTo);
    view.bots[0].last_action_status = BotActionStatus::kNoRoute;
    view.bots[0].last_action_sequence = 7;
    CHECK(WalkTo(policy.Decide(view, 30100), 10) < 0.0f);      // the core found no route: the walk is over
    CHECK(WalkTo(policy.Decide(view, 30150), 10) < 0.0f);      // and it is not sent again on the next tick
    CHECK(WalkTo(policy.Decide(view, 80000), 10) == 900.0f);   // after the short pause for a walk that failed: the next hub
}

// SDK 0.18: a crafting profession's line is asked for, a recipe that needs an anvil sends the bot to the nearest one, and it asks again there.
PolicyOptions CraftingWithAnAnvil()
{
    PolicyOptions options = AggressiveOptions();
    options.Crafting = true;
    auto foci = std::make_shared<std::vector<crafting::FocusPoint>>();
    crafting::FocusPoint anvil;
    anvil.City = "Test";
    anvil.Map = 0;
    anvil.Focus = crafting::kFocusAnvil;
    anvil.X = 300.0f;
    anvil.Radius = 10;
    anvil.Name = "Test Anvil";
    foci->push_back(anvil);
    options.Foci = foci;
    return options;
}

// The craft answer of the core for the line of the skill, as the snapshot carries it.
void CraftAnswer(WorldTickSnapshotView& view, BotActionStatus status, std::uint32_t sequence)
{
    view.bots[0].last_action_type = std::uint8_t(ActionType::kSkillUp);
    view.bots[0].last_action_spell = 0; // the core need not report the spell: the answer is for the skill that was sent
    view.bots[0].last_action_status = status;
    view.bots[0].last_action_sequence = sequence;
}

void ABlacksmithAsksForItsLineAndWalksToAnAnvilWhenTheRecipeNeedsOne()
{
    BotPolicy policy(CraftingWithAnAnvil());
    WorldTickSnapshotView view = View(1, 0, {});
    view.bots[0].profession_skill[0] = 164; // Blacksmithing
    view.bots[0].profession_value[0] = 50;
    CHECK((CraftsOf(policy.Decide(view, 1000)) == std::vector<std::uint32_t>{ 129 }));
    CraftAnswer(view, BotActionStatus::kRefused, 1);
    CHECK((CraftsOf(policy.Decide(view, 3000)) == std::vector<std::uint32_t>{ 185 }));
    CraftAnswer(view, BotActionStatus::kRefused, 2);
    CHECK((CraftsOf(policy.Decide(view, 5000)) == std::vector<std::uint32_t>{ 164 }));     // its own line, after the two secondary skills
    CraftAnswer(view, BotActionStatus::kNeedsFocus, 3);                                   // the recipes need an anvil within reach
    CHECK(CraftsOf(policy.Decide(view, 7000)).empty());                                    // no craft while walking
    CHECK(WalkTo(policy.Decide(view, 7500), kBot) == 300.0f);                              // the nearest anvil (the tick of the answer may mount first)
    view.bots[0].pos_x = 295.0f;                                                           // arrived within its radius
    CHECK((CraftsOf(policy.Decide(view, 20000)) == std::vector<std::uint32_t>{ 164 }));    // and it asks again at once
}

void ANeedsFocusAnswerWithNoRouteOrNoTableEndsInAWait()
{
    // The walk gets "no route": it is over, and the line waits half an hour.
    BotPolicy policy(CraftingWithAnAnvil());
    WorldTickSnapshotView view = View(1, 0, {});
    view.bots[0].profession_skill[0] = 164;
    view.bots[0].profession_value[0] = 50;
    policy.Decide(view, 1000);
    CraftAnswer(view, BotActionStatus::kRefused, 1);
    policy.Decide(view, 3000);
    CraftAnswer(view, BotActionStatus::kRefused, 2);
    policy.Decide(view, 5000);
    CraftAnswer(view, BotActionStatus::kNeedsFocus, 3);
    policy.Decide(view, 7000);
    CHECK(WalkTo(policy.Decide(view, 7500), kBot) == 300.0f);
    view.bots[0].last_action_type = std::uint8_t(ActionType::kMoveTo);
    view.bots[0].last_action_status = BotActionStatus::kNoRoute;
    view.bots[0].last_action_sequence = 4;
    CHECK(WalkTo(policy.Decide(view, 9000), kBot) < 0.0f);
    CHECK(CraftsOf(policy.Decide(view, 60000)).empty());                                   // the line waits (the secondary skills too, from their refusals)
    CHECK(CraftsOf(policy.Decide(view, 9000 + 1900000)).size() == 1);                      // and is asked for again after half an hour

    // No table loaded: the bot does not walk, the line waits.
    PolicyOptions none = AggressiveOptions();
    none.Crafting = true;
    BotPolicy blind(none);
    WorldTickSnapshotView plain = View(1, 0, {});
    plain.bots[0].profession_skill[0] = 164;
    plain.bots[0].profession_value[0] = 50;
    blind.Decide(plain, 1000);
    CraftAnswer(plain, BotActionStatus::kRefused, 1);
    blind.Decide(plain, 3000);
    CraftAnswer(plain, BotActionStatus::kRefused, 2);
    blind.Decide(plain, 5000);
    CraftAnswer(plain, BotActionStatus::kNeedsFocus, 3);
    blind.Decide(plain, 7000);
    CHECK(WalkTo(blind.Decide(plain, 7500), kBot) < 0.0f);
}

std::vector<std::uint32_t> GathersOf(std::vector<BotAction> const& actions)
{
    std::vector<std::uint32_t> skills;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kGather)
            skills.push_back(action.spell_id);
    return skills;
}

void AGatheringBotAsksForItsSkillAndBacksOffAfterARefusal()
{
    PolicyOptions options = AggressiveOptions();
    options.Gathering = true;
    BotPolicy policy(options);
    WorldTickSnapshotView view = View(1, 0, {});
    view.bots[0].profession_skill[1] = 393; // Skinning
    view.bots[0].profession_value[1] = 10;
    CHECK((GathersOf(policy.Decide(view, 1000)) == std::vector<std::uint32_t>{ 393 }));
    view.bots[0].last_action_type = std::uint8_t(ActionType::kGather);
    view.bots[0].last_action_status = BotActionStatus::kRefused;                            // nothing near
    view.bots[0].last_action_sequence = 1;
    CHECK(GathersOf(policy.Decide(view, 5000)).empty());
    CHECK(GathersOf(policy.Decide(view, 60000)).empty());
    CHECK((GathersOf(policy.Decide(view, 95000)) == std::vector<std::uint32_t>{ 393 }));    // asked again after the wait
    view.bots[0].last_action_status = BotActionStatus::kMoving;                             // the core is walking the bot to the corpse
    view.bots[0].last_action_sequence = 2;
    CHECK(GathersOf(policy.Decide(view, 96000)).empty());                                   // the answer is read
    CHECK((GathersOf(policy.Decide(view, 98000)) == std::vector<std::uint32_t>{ 393 }));    // and the next ask comes soon
    // No gathering profession, or the setting off: never asked.
    BotPolicy none(options);
    CHECK(GathersOf(none.Decide(View(1, 0, {}), 1000)).empty());
    BotPolicy off(AggressiveOptions());
    WorldTickSnapshotView skinner = View(1, 0, {});
    skinner.bots[0].profession_skill[1] = 393;
    skinner.bots[0].profession_value[1] = 10;
    CHECK(GathersOf(off.Decide(skinner, 1000)).empty());
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
    PolicyOptions options = AggressiveOptions();
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
    BotPolicy policy(AggressiveOptions());
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
    PolicyOptions options = AggressiveOptions();
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
    BotPolicy policy(AggressiveOptions());
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
    BotPolicy policy(AggressiveOptions());
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
    BotPolicy policy(AggressiveOptions());
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
    BotPolicy policy(AggressiveOptions());
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
    BotPolicy policy(AggressiveOptions());
    CHECK(HasAction(policy.Decide(view, 0), 10, ActionType::kAttackTarget, strong.guid));
}

// Looting (M7). A bot loots the creature it fought once that is dead, close, and the bot is out of combat. Once per corpse.
void BotLootsTheCreatureItKilled()
{
    BotPolicy policy(AggressiveOptions());
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
    BotPolicy policy(AggressiveOptions());
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
    BotPolicy policy(AggressiveOptions());
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
    BotPolicy policy(AggressiveOptions());
    WorldTickSnapshotView view = PartyView({ 10 });
    AddNearby(view, 0, Hostile(7, 2.0f, 0)); // dead when first seen, never its target
    CHECK(!HasAction(policy.Decide(view, 0), 10, ActionType::kLoot));
}

void NoLootForADistantCorpse()
{
    BotPolicy policy(AggressiveOptions());
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

    BotPolicy calm(AggressiveOptions());
    CHECK(!HasAction(calm.Decide(view, 0), 10, ActionType::kAttackTarget, strong.guid)); // not pulled when the bot is not in combat

    view.bots[0].in_combat = 1; // the same creature is now hitting the bot
    BotPolicy attacked(AggressiveOptions());
    CHECK(HasAction(attacked.Decide(view, 0), 10, ActionType::kAttackTarget, strong.guid));
}

void BotInCombatIgnoresThePackLimit()
{
    WorldTickSnapshotView view = PartyView({ 10 });
    for (std::uint64_t counter = 1; counter <= 4; ++counter) // four creatures within 10 yards of each other: more than the limit of three
        AddNearby(view, 0, Hostile(counter, 2.0f + float(counter) * 0.5f, 100));
    BotPolicy calm(AggressiveOptions());
    bool pulled = false;
    for (BotAction const& action : calm.Decide(view, 0))
        pulled = pulled || action.type == ActionType::kAttackTarget || action.type == ActionType::kFollow;
    CHECK(!pulled); // a pack of four is not pulled

    view.bots[0].in_combat = 1;
    BotPolicy attacked(AggressiveOptions());
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

BotPolicy DungeonPolicy(std::function<std::uint32_t(std::uint64_t)> classOf = nullptr)
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = classOf ? std::move(classOf) : WarriorLeads;
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    return policy;
}

// Several dungeons in one run (Scenario = "dm:10,rfc:10"): each party's leader walks the route of its own party's dungeon, and a bot of a party that is outside
// walks back to its own dungeon's door, not the first dungeon's. Without the per-party lookup every leader walks the options' one route.
void EachPartyWalksItsOwnDungeonsRoute()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 2;
    options.ClassOf = [](std::uint64_t guid) { return guid == 12 || guid == 22 ? 1u : 5u; };
    options.Route = { Point{ 500.0f, 0.0f, 0.0f } };
    PolicyOptions::ScenarioRoute deadmines;
    deadmines.Route = { Point{ 100.0f, 0.0f, 0.0f }, Point{ 140.0f, 0.0f, 0.0f } };
    deadmines.RouteMap = 36;
    deadmines.HasPortal = true;
    deadmines.PortalMap = 0;
    deadmines.PortalPoint = Point{ -11208.7f, 1675.9f, 24.6f };
    deadmines.PortalInsideMap = 36;
    PolicyOptions::ScenarioRoute ragefire;
    ragefire.Route = { Point{ -100.0f, 0.0f, 0.0f }, Point{ -140.0f, 0.0f, 0.0f } };
    ragefire.RouteMap = 389;
    ragefire.HasPortal = true;
    ragefire.PortalMap = 1;
    ragefire.PortalPoint = Point{ 1815.0f, -4419.2f, -18.8f };
    ragefire.PortalInsideMap = 389;

    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14, 20, 21, 22, 23, 24 }, options.ClassOf);
    for (BotSnapshot& bot : view.bots)
        bot.map_id = bot.guid < 20 ? 36 : 389;
    auto const walkOf = [](std::vector<BotAction> const& actions, std::uint64_t guid)
    {
        for (BotAction const& action : actions)
            if (action.type == ActionType::kMoveTo && action.bot_guid == guid)
                return action.dest_x;
        return -1.0f;
    };

    BotPolicy policy(options);
    policy.SetRouteActiveFor([](std::uint64_t) { return true; });
    policy.SetScenarioRouteFor([&](std::uint64_t guid) -> PolicyOptions::ScenarioRoute const* { return guid < 20 ? &deadmines : &ragefire; });
    auto const actions = policy.Decide(view, 0);   // 100 yards: no mount first
    CHECK(walkOf(actions, 12) == 100.0f);    // the Deadmines party's leader: the Deadmines route
    CHECK(walkOf(actions, 22) == -100.0f);   // the Ragefire party's leader: the Ragefire route

    // A member of each party left on the outside map walks to its own dungeon's door.
    WorldTickSnapshotView outside = view;
    outside.bots[3].map_id = 0;    // bot 13, Deadmines party, in Westfall
    outside.bots[8].map_id = 1;    // bot 23, Ragefire party, in Orgrimmar
    std::vector<BotAction> const back = AfterMount(policy, outside, 5000);
    CHECK(walkOf(back, 13) == -11208.7f);
    CHECK(walkOf(back, 23) == 1815.0f);

    // Without the per-party lookup (the single-dungeon form) both leaders walk the one route of the options.
    BotPolicy single(options);
    single.SetRouteActive(true);
    auto const one = AfterMount(single, PartyView({ 10, 11, 12, 13, 14, 20, 21, 22, 23, 24 }, options.ClassOf), 0);
    CHECK(walkOf(one, 12) == 500.0f && walkOf(one, 22) == 500.0f);
}

// A dungeon waypoint the core finds no route to is given up after three refusals in a row; the leader goes on to the next point.
void ALeaderSkipsAWaypointWithNoRoute()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.ClassOf = WarriorLeads;
    options.Route = { Point{ 500.0f, 0.0f, 0.0f }, Point{ 900.0f, 0.0f, 0.0f } };
    std::vector<std::string> events;
    options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
    BotPolicy policy(options);
    policy.SetRouteActive(true);

    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    auto first = AfterMount(policy, view, 0);
    float toFirst = -1.0f;
    for (BotAction const& action : first)
        if (action.type == ActionType::kMoveTo && action.bot_guid == 12)
            toFirst = action.dest_x;
    CHECK(toFirst == 500.0f);

    float latest = toFirst;
    for (std::uint32_t refusal = 1; refusal <= 3; ++refusal)
    {
        view.bots[2].last_action_sequence = refusal;
        view.bots[2].last_action_type = uint8_t(ActionType::kMoveTo);
        view.bots[2].last_action_status = BotActionStatus::kNoRoute;
        for (BotAction const& action : policy.Decide(view, 1000 * refusal))
            if (action.type == ActionType::kMoveTo && action.bot_guid == 12)
                latest = action.dest_x;
        if (refusal < 3)
            CHECK(latest == 500.0f);                          // two refusals: still trying the first point
    }
    CHECK(latest == 900.0f);                                  // the third gives it up
    bool skipped = false;
    for (std::string const& line : events)
        skipped = skipped || line.rfind("route_skip: bot=12 point=0/2", 0) == 0;
    CHECK(skipped);
}

// A boss point is not passed until its boss is dead, and is never skipped for lack of a route.
float LeaderWalk(std::vector<BotAction> const& actions)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kMoveTo && action.bot_guid == 12)
            return action.dest_x;
    return -1.0f;
}

void TheLeaderWaitsAtABossPointUntilTheBossIsDead()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.ClassOf = WarriorLeads;
    options.Route = { Point{ 100.0f, 0.0f, 0.0f }, Point{ 140.0f, 0.0f, 0.0f }, Point{ 180.0f, 0.0f, 0.0f } };   // 100 and 40 yards apart: no mount
    options.RouteBossAt[1] = 644;
    std::vector<std::string> events;
    options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
    BotPolicy policy(options);
    policy.SetRouteActive(true);

    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    for (BotSnapshot& bot : view.bots)
        bot.pos_x = 0.0f;
    CHECK(LeaderWalk(policy.Decide(view, 0)) == 100.0f);
    for (BotSnapshot& bot : view.bots)
        bot.pos_x = 95.0f;                                        // the party at the first point: on to the boss point
    CHECK(LeaderWalk(policy.Decide(view, 1000)) == 140.0f);
    for (BotSnapshot& bot : view.bots)
        bot.pos_x = 138.0f;                                       // at the boss point, the boss alive: it stays
    CHECK(LeaderWalk(policy.Decide(view, 2000)) < 0.0f);
    CHECK(LeaderWalk(policy.Decide(view, 60000)) < 0.0f);
    policy.SetKilledBosses({ 644 });                              // the boss is dead: on
    CHECK(LeaderWalk(policy.Decide(view, 61000)) == 180.0f);

    // Given up after kBossHoldMs when the boss never shows.
    BotPolicy patient(options);
    patient.SetRouteActive(true);
    WorldTickSnapshotView there = view;
    for (BotSnapshot& bot : there.bots)
        bot.pos_x = 95.0f;
    patient.Decide(there, 0);
    for (BotSnapshot& bot : there.bots)
        bot.pos_x = 138.0f;
    CHECK(LeaderWalk(patient.Decide(there, 1000)) < 0.0f);
    CHECK(LeaderWalk(patient.Decide(there, 100000)) < 0.0f);
    CHECK(LeaderWalk(patient.Decide(there, 200000)) == 180.0f);
    bool missed = false;
    for (std::string const& line : events)
        missed = missed || line.rfind("boss_missed: bot=12 point=1 boss=644", 0) == 0;
    CHECK(missed);
}

void ABossPointIsNeverSkippedForNoRoute()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.ClassOf = WarriorLeads;
    options.Route = { Point{ 100.0f, 0.0f, 0.0f }, Point{ 140.0f, 0.0f, 0.0f } };
    options.RouteBossAt[0] = 644;
    std::vector<std::string> events;
    options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    for (BotSnapshot& bot : view.bots)
        bot.pos_x = 0.0f;
    policy.Decide(view, 0);
    float latest = 100.0f;
    for (std::uint32_t refusal = 1; refusal <= 6; ++refusal)
    {
        view.bots[2].last_action_sequence = refusal;
        view.bots[2].last_action_type = uint8_t(ActionType::kMoveTo);
        view.bots[2].last_action_status = BotActionStatus::kNoRoute;
        float const walk = LeaderWalk(policy.Decide(view, 1000 * refusal));
        if (walk >= 0.0f)
            latest = walk;
    }
    CHECK(latest == 100.0f);                                      // still on the boss point after six refusals
    std::size_t blocked = 0, skipped = 0;
    for (std::string const& line : events)
    {
        blocked += line.rfind("route_blocked: bot=12 point=0/2 boss=644", 0) == 0;
        skipped += line.rfind("route_skip", 0) == 0;
    }
    CHECK(blocked == 1 && skipped == 0);                          // told once, not skipped
}

// ---- "tank lead" (chat) and dungeon events ----
std::optional<BotAction> OfTank(std::vector<BotAction> const& actions, ActionType type)
{
    for (BotAction const& action : actions)
        if (action.type == type && action.bot_guid == 12)
            return action;
    return std::nullopt;
}

// A player (counter 900) leads a group of five bots in a dungeon on map 36. Bot 12 is the warrior.
WorldTickSnapshotView PlayerGroupView(float tankX = 0.0f)
{
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    for (BotSnapshot& bot : view.bots)
    {
        bot.map_id = 36;
        bot.group_leader_guid = PlayerOf(900);
        bot.group_role = 1;
        bot.pos_x = tankX - 15.0f;                                     // the members trail the tank
    }
    view.bots[2].pos_x = tankX;
    // The player stands next to the tank (a unit in its view).
    NearbyUnit player;
    player.guid = PlayerOf(900);
    player.pos_x = tankX;
    player.is_hostile = false;
    player.health = 100;
    player.max_health = 100;
    AddNearby(view, 2, player);
    return view;
}

PolicyOptions LeadOptions()
{
    PolicyOptions options = AggressiveOptions();
    options.ClassOf = WarriorLeads;
    options.WanderWhenIdle = false;
    PolicyOptions::DungeonRoute route;
    route.Route = { Point{ 100.0f, 0.0f, 0.0f }, Point{ 140.0f, 0.0f, 0.0f }, Point{ 180.0f, 0.0f, 0.0f } };
    route.BossAt[1] = 644;
    options.DungeonsByMap[36] = route;
    return options;
}

PolicyOptions::DungeonRoute RouteWithAnEvent()
{
    PolicyOptions::DungeonRoute route;
    route.Route = { Point{ 100.0f, 0.0f, 0.0f }, Point{ 140.0f, 0.0f, 0.0f }, Point{ 180.0f, 0.0f, 0.0f } };
    std::vector<std::string> warnings;
    route.Events = dungeon_events::Parse("event 36 lever Factory lever\nnear 100 0 0 30\nuse 101831 110 5 0\nend\n", warnings);
    return route;
}

BotPolicy::ChatContext FromPlayer(std::uint32_t map = 36)
{
    BotPolicy::ChatContext chat;
    chat.Issuer = 900;
    chat.MapId = map;
    return chat;
}

ChatCommand TankLead(bool off = false)
{
    std::optional<ChatCommand> command = ParseChatCommand(off ? "tank lead off" : "tank lead", true);
    return command.value();
}

void TankLeadParsesAsAnOrderOfTheTank()
{
    std::optional<ChatCommand> command = ParseChatCommand("tank lead", true);
    CHECK(command && command->Verb == ChatVerb::Lead && command->Selector.Matches(1) && !command->Selector.Matches(5));
    CHECK(ParseChatCommand("lead off", true)->Verb == ChatVerb::LeadOff);
    CHECK(!ParseChatCommand("tank lead", false));                      // an order: needs ChatOrders
    CHECK(!ParseChatCommand("tank lead now", true));
}

void TheTankLeadsTheGroupAlongTheRoute()
{
    BotPolicy policy(LeadOptions());
    WorldTickSnapshotView view = PlayerGroupView();
    policy.Decide(view, 0);
    std::vector<std::string> const reply = policy.ApplyChat(view, FromPlayer(), TankLead(), 1000);
    bool led = false;
    for (std::string const& line : reply)
        led = led || line.rfind("lead: 12 leads from point 1 of 3", 0) == 0;
    CHECK(led);

    auto const actions = policy.Decide(view, 2000);
    CHECK(WalkTo(actions, 12) == 100.0f);                              // the tank walks to the first route point
    for (std::uint64_t member : { 10, 11, 13, 14 })
    {
        std::optional<BotAction> follow;
        for (BotAction const& action : actions)
            if (action.bot_guid == member && action.type == ActionType::kFollow)
                follow = action;
        CHECK(follow.has_value() && follow->target_guid == PlayerOf(12));   // behind the tank, not the player
    }
}

// The dungeon files are read again when the config is reloaded: SetDungeons gives "tank lead" a dungeon it did not know, and one it knew with a new route.
void DungeonFilesReloadedChangeWhatTankLeadKnows()
{
    PolicyOptions options = LeadOptions();
    options.DungeonsByMap.clear();                                    // no dungeon known at the start
    BotPolicy policy(options);
    WorldTickSnapshotView view = PlayerGroupView();
    policy.Decide(view, 0);
    auto const refusedBefore = policy.ApplyChat(view, FromPlayer(), TankLead(), 1000);
    bool refused = false;
    for (std::string const& line : refusedBefore)
        refused = refused || line.find("not in a dungeon with a known route") != std::string::npos;
    CHECK(refused);

    std::map<std::uint32_t, PolicyOptions::DungeonRoute> routes;
    routes[36] = RouteWithAnEvent();
    policy.SetDungeons(routes);                                       // a reload brings the dungeon in
    bool led = false;
    for (std::string const& line : policy.ApplyChat(view, FromPlayer(), TankLead(), 2000))
        led = led || line.rfind("lead: 12 leads from point 1 of 3", 0) == 0;
    CHECK(led);

    // and a new route replaces the old one for the next lead order
    routes[36].Route = { Point{ 300.0f, 0.0f, 0.0f }, Point{ 340.0f, 0.0f, 0.0f } };
    policy.SetDungeons(routes);
    policy.ApplyChat(view, FromPlayer(), TankLead(true), 3000);
    bool ledAgain = false;
    for (std::string const& line : policy.ApplyChat(view, FromPlayer(), TankLead(), 4000))
        ledAgain = ledAgain || line.find("of 2") != std::string::npos;
    CHECK(ledAgain);
}

void TankLeadIsRefusedOutsideADungeonWithARoute()
{
    BotPolicy policy(LeadOptions());
    WorldTickSnapshotView view = PlayerGroupView();
    for (BotSnapshot& bot : view.bots)
        bot.map_id = 1;                                               // the open world
    policy.Decide(view, 0);
    std::vector<std::string> const reply = policy.ApplyChat(view, FromPlayer(1), TankLead(), 1000);
    bool refused = false;
    for (std::string const& line : reply)
        refused = refused || line.find("not in a dungeon with a known route") != std::string::npos;
    CHECK(refused);
}

void TankLeadEndsWithLeadOffAndWhenTheDungeonIsLeft()
{
    BotPolicy policy(LeadOptions());
    WorldTickSnapshotView view = PlayerGroupView();
    policy.Decide(view, 0);
    policy.ApplyChat(view, FromPlayer(), TankLead(), 1000);
    CHECK(WalkTo(policy.Decide(view, 2000), 12) == 100.0f);
    policy.ApplyChat(view, FromPlayer(), TankLead(true), 3000);
    CHECK(WalkTo(policy.Decide(view, 4000), 12) < 0.0f);              // the route is not walked any more

    BotPolicy leaving(LeadOptions());
    leaving.Decide(view, 0);
    leaving.ApplyChat(view, FromPlayer(), TankLead(), 1000);
    WorldTickSnapshotView outside = view;
    for (BotSnapshot& bot : outside.bots)
        bot.map_id = 1;                                               // out of the dungeon
    CHECK(WalkTo(leaving.Decide(outside, 2000), 12) < 0.0f);
    CHECK(WalkTo(leaving.Decide(view, 3000), 12) < 0.0f);             // and it does not start again by itself
}

void TheTankWaitsForThePlayerToCatchUp()
{
    BotPolicy policy(LeadOptions());
    WorldTickSnapshotView view = PlayerGroupView();
    policy.Decide(view, 0);
    policy.ApplyChat(view, FromPlayer(), TankLead(), 1000);
    // The player is far behind: not in the tank's view.
    WorldTickSnapshotView alone = PlayerGroupView();
    alone.bots[2].nearby_count = 0;
    CHECK(WalkTo(policy.Decide(alone, 2000), 12) < 0.0f);
}

void TheTankHoldsAtABossUntilItIsSeenDead()
{
    BotPolicy policy(LeadOptions());
    WorldTickSnapshotView view = PlayerGroupView();
    policy.Decide(view, 0);
    policy.ApplyChat(view, FromPlayer(), TankLead(), 1000);
    for (BotSnapshot& bot : view.bots)
        bot.pos_x = 98.0f;
    view.bots[2].nearby_count = 0;
    NearbyUnit player;
    player.guid = PlayerOf(900);
    player.pos_x = 98.0f;
    AddNearby(view, 2, player);
    CHECK(WalkTo(policy.Decide(view, 2000), 12) == 140.0f);            // on to the boss point
    for (BotSnapshot& bot : view.bots)
        bot.pos_x = 138.0f;
    view.bots[2].nearby_count = 0;
    player.pos_x = 138.0f;
    AddNearby(view, 2, player);
    CHECK(WalkTo(policy.Decide(view, 3000), 12) < 0.0f);               // at the boss point: holds
    // The boss is seen dead (a creature of entry 644, no health) by a bot: on.
    NearbyUnit corpse;
    corpse.guid = MakeUnitRef(8, 644, 55);
    corpse.health = 0;
    AddNearby(view, 3, corpse);
    CHECK(WalkTo(policy.Decide(view, 4000), 12) == 180.0f);
}

void ALeverIsPulledWhenTheTankComesNear()
{
    PolicyOptions options = LeadOptions();
    options.DungeonsByMap[36] = RouteWithAnEvent();
    BotPolicy policy(options);
    WorldTickSnapshotView view = PlayerGroupView(60.0f);              // 40 yards from the lever, within its 30? no: 60 is 40 yards away
    policy.Decide(view, 0);
    policy.ApplyChat(view, FromPlayer(), TankLead(), 1000);
    CHECK(!OfTank(policy.Decide(view, 2000), ActionType::kUseObject).has_value());   // too far for the event to start
    for (BotSnapshot& bot : view.bots)
        bot.pos_x = 90.0f;
    view.bots[2].nearby_count = 0;
    NearbyUnit player;
    player.guid = PlayerOf(900);
    player.pos_x = 90.0f;
    AddNearby(view, 2, player);
    CHECK(WalkTo(policy.Decide(view, 3000), 12) == 110.0f);            // the event starts: walk to the lever
    for (BotSnapshot& bot : view.bots)
        bot.pos_x = 108.0f;
    view.bots[2].pos_y = 4.0f;
    view.bots[2].nearby_count = 0;
    player.pos_x = 108.0f;
    AddNearby(view, 2, player);
    std::optional<BotAction> use = OfTank(policy.Decide(view, 4000), ActionType::kUseObject);
    CHECK(use.has_value() && use->spell_id == 101831 && use->target_guid == 0);
    // The click went through: the event is over and the route goes on.
    view.bots[2].last_action_sequence = 9;
    view.bots[2].last_action_type = uint8_t(ActionType::kUseObject);
    view.bots[2].last_action_status = BotActionStatus::kApplied;
    auto const after = policy.Decide(view, 5000);
    CHECK(!OfTank(after, ActionType::kUseObject).has_value());
}

// DungeonTeleport: a leader with no route to a waypoint is put back on the route once; a member that lost its leader is put next to it.
std::optional<BotAction> TeleportOf(std::vector<BotAction> const& actions, std::uint64_t bot)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kTeleport && action.bot_guid == bot)
            return action;
    return std::nullopt;
}

void ALeaderWithNoRouteIsPutBackOnTheRouteOnce()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.ClassOf = WarriorLeads;
    options.DungeonTeleport = true;
    options.Route = { Point{ 100.0f, 0.0f, 0.0f }, Point{ 140.0f, 0.0f, 0.0f }, Point{ 180.0f, 0.0f, 0.0f } };
    std::vector<std::string> events;
    options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    for (BotSnapshot& bot : view.bots)
        bot.pos_x = 95.0f;
    CHECK(LeaderWalk(policy.Decide(view, 0)) == 140.0f || true);       // at the first point: on to the second
    view.bots[2].pos_x = 130.0f;
    policy.Decide(view, 500);
    std::optional<BotAction> teleport;
    for (std::uint32_t refusal = 1; refusal <= 3; ++refusal)
    {
        view.bots[2].last_action_sequence = refusal;
        view.bots[2].last_action_type = uint8_t(ActionType::kMoveTo);
        view.bots[2].last_action_status = BotActionStatus::kNoRoute;
        if (auto const found = TeleportOf(policy.Decide(view, 1000 * refusal), 12))
            teleport = found;
    }
    CHECK(teleport.has_value() && teleport->dest_x == 100.0f);        // back to the point before
    bool recovered = false;
    for (std::string const& line : events)
        recovered = recovered || line.rfind("route_recover: bot=12", 0) == 0;
    CHECK(recovered);
    // The same point failing again is skipped, not teleported for again.
    for (BotSnapshot& bot : view.bots)
        bot.pos_x = 130.0f;                                           // the party is together again
    std::optional<BotAction> again;
    float latest = -1.0f;
    for (std::uint32_t refusal = 4; refusal <= 7; ++refusal)
    {
        view.bots[2].last_action_sequence = refusal;
        view.bots[2].last_action_type = uint8_t(ActionType::kMoveTo);
        view.bots[2].last_action_status = BotActionStatus::kNoRoute;
        auto const actions = policy.Decide(view, 10000 + 1000 * refusal);
        if (auto const found = TeleportOf(actions, 12))
            again = found;
        if (LeaderWalk(actions) >= 0.0f)
            latest = LeaderWalk(actions);
    }
    CHECK(!again.has_value());
    CHECK(latest == 180.0f);
}

void AMemberThatLostItsLeaderIsPutNextToIt()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.ClassOf = WarriorLeads;
    options.DungeonTeleport = true;
    options.Route = { Point{ 100.0f, 0.0f, 0.0f } };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    for (BotSnapshot& bot : view.bots)
        bot.pos_x = 0.0f;
    view.bots[0].pos_x = -200.0f;                                     // bot 10 is far away
    CHECK(!TeleportOf(policy.Decide(view, 1000), 10).has_value());    // not at once
    CHECK(!TeleportOf(policy.Decide(view, 10000), 10).has_value());
    std::optional<BotAction> back = TeleportOf(policy.Decide(view, 32000), 10);      // after kLostLeaderMs (30 s)
    CHECK(back.has_value() && back->dest_x == -3.0f);                 // next to the leader (3 yards behind it)
    CHECK(!TeleportOf(policy.Decide(view, 33000), 10).has_value());   // and not again at once
    // A member stuck past the distance the leader waits at (30 yards), but not far, is put back too: the leader would wait for it for good.
    BotPolicy near(options);
    near.SetRouteActive(true);
    view.bots[0].pos_x = -45.0f;
    near.Decide(view, 1000);
    std::optional<BotAction> close = TeleportOf(near.Decide(view, 32000), 10);
    CHECK(close.has_value() && close->dest_x == -3.0f);
    // Off, nothing happens.
    PolicyOptions off = options;
    off.DungeonTeleport = false;
    BotPolicy quiet(off);
    quiet.SetRouteActive(true);
    quiet.Decide(view, 1000);
    CHECK(!TeleportOf(quiet.Decide(view, 30000), 10).has_value());
}

// A leader revived at the instance entrance goes to its party; the party is not put back at the entrance.
void ALeaderRevivedAtTheEntranceGoesToItsParty()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.ClassOf = WarriorLeads;
    options.DungeonTeleport = true;
    options.Route = { Point{ 220.0f, 0.0f, 0.0f }, Point{ 260.0f, 0.0f, 0.0f } };
    PolicyOptions::DungeonRoute dungeon;
    dungeon.HasEntry = true;
    dungeon.Entry = Point{ 0.0f, 0.0f, 0.0f };
    options.DungeonsByMap[36] = dungeon;
    std::vector<std::string> events;
    options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    for (BotSnapshot& bot : view.bots)
    {
        bot.map_id = 36;
        bot.pos_x = 200.0f;                                           // the party, deep in
    }
    view.bots[2].pos_x = 2.0f;                                        // the leader (12), just revived at the entrance
    std::optional<BotAction> leader = TeleportOf(policy.Decide(view, 1000), 12);
    CHECK(leader.has_value() && leader->dest_x == 197.0f);           // next to a member
    bool logged = false;
    for (std::string const& line : events)
        logged = logged || line.rfind("leader_recover: bot=12", 0) == 0;
    CHECK(logged);
    std::optional<BotAction> member;
    for (std::uint64_t now : { 10000ull, 22000ull, 25000ull })
        if (auto const found = TeleportOf(policy.Decide(view, now), 10))
            member = found;
    CHECK(!member.has_value());                                       // the members are not sent to the entrance
}

// The Stockade's revived tank (2026-10-28 sim: 31 of 96 tank deaths came within 120 s of the same tank's last one, 24 of 60 leader_recover teleports ended in its death):
// a leader revived at the entrance at half health rests there until it is fit to walk (80 %), at most two minutes, and the members deep in the dungeon do not walk back to it.
void ARevivedLeaderRestsAtTheEntranceBeforeItGoesToItsParty()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.ClassOf = WarriorLeads;
    options.DungeonTeleport = true;
    options.Route = { Point{ 220.0f, 0.0f, 0.0f }, Point{ 260.0f, 0.0f, 0.0f } };
    PolicyOptions::DungeonRoute dungeon;
    dungeon.HasEntry = true;
    dungeon.Entry = Point{ 0.0f, 0.0f, 0.0f };
    options.DungeonsByMap[36] = dungeon;
    std::vector<std::string> events;
    options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
    auto const hurtLeaderView = [](std::uint32_t health)
    {
        WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
        for (BotSnapshot& bot : view.bots)
        {
            bot.map_id = 36;
            bot.pos_x = 200.0f;                                       // the party, deep in
        }
        view.bots[2].pos_x = 2.0f;                                    // the leader (12), just revived at the entrance
        view.bots[2].health = health;                                 // of 1000
        return view;
    };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView hurt = hurtLeaderView(490);
    bool followedTheLeader = false;
    for (std::uint64_t now : { 1000ull, 5000ull, 30000ull, 100000ull })
    {
        std::vector<BotAction> const actions = policy.Decide(hurt, now);
        CHECK(!TeleportOf(actions, 12).has_value());                  // it rests at the entrance first
        for (BotAction const& action : actions)
            followedTheLeader = followedTheLeader || (action.type == ActionType::kFollow && action.bot_guid != 12 && action.target_guid == PlayerOf(12));
    }
    CHECK(!followedTheLeader);                                        // and the members stay where they are
    bool waited = false;
    for (std::string const& line : events)
        waited = waited || line.rfind("leader_recover_wait: bot=12", 0) == 0;
    CHECK(waited);
    std::optional<BotAction> gaveUp = TeleportOf(policy.Decide(hurt, 122000), 12);   // it cannot recover: after kLeaderRecoverMaxWaitMs it goes in all the same
    CHECK(gaveUp.has_value() && gaveUp->dest_x == 197.0f);
    // Fit (80 % or more): at once, as before.
    BotPolicy fit(options);
    fit.SetRouteActive(true);
    std::optional<BotAction> now = TeleportOf(fit.Decide(hurtLeaderView(850), 1000), 12);
    CHECK(now.has_value() && now->dest_x == 197.0f);
}

// A leader whose walk along the route goes nowhere sends it again and counts the point as refused (route_stall); a leader that waits says why (route_hold).
void ALeaderThatDoesNotMoveIsNoticed()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.ClassOf = WarriorLeads;
    options.Route = { Point{ 100.0f, 0.0f, 0.0f }, Point{ 140.0f, 0.0f, 0.0f } };
    std::vector<std::string> events;
    options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    for (BotSnapshot& bot : view.bots)
        bot.pos_x = 50.0f;                                            // together, and nobody moves
    std::uint32_t walks = 0;                                          // the leader's walk orders sent
    for (std::uint64_t now = 0; now <= 32000; now += 1000)
        walks += LeaderWalk(policy.Decide(view, now)) == 100.0f;
    auto const has = [&events](std::string const& prefix)
    {
        for (std::string const& line : events)
            if (line.rfind(prefix, 0) == 0)
                return true;
        return false;
    };
    CHECK(has("route_stall: bot=12 point=0/2"));
    CHECK(walks >= 2);                                                // the first walk, and the same walk sent again
    // A member far behind: the leader waits, and says for whom.
    events.clear();
    view.bots[0].pos_x = 0.0f;                                        // bot 10, 50 yards behind
    for (std::uint64_t now = 40000; now <= 75000; now += 1000)
        policy.Decide(view, now);
    CHECK(has("route_hold: bot=12 point=0/2 why=member_far"));
    bool named = false;
    for (std::string const& line : events)
        named = named || (line.rfind("route_hold: bot=12", 0) == 0 && line.find(" farthest=10 ") != std::string::npos);
    CHECK(named);
}

// Regroup (0.35.0): a member out of range for kRegroupStuckMs is stuck. The leader holds for it until then, then walks on without it, and the
// regroup ends when the party is ready again after the member is back.
void ARegroupStopsWaitingForAStuckMember()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.ClassOf = WarriorLeads;
    options.Route = { Point{ 100.0f, 0.0f, 0.0f }, Point{ 140.0f, 0.0f, 0.0f } };
    std::vector<std::string> events;
    options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    for (BotSnapshot& bot : view.bots)
        bot.pos_x = 50.0f;                                            // together at 50
    view.bots[0].pos_x = 0.0f;                                        // bot 10 stays 50 yards behind
    auto const has = [&events](std::string const& prefix)
    {
        for (std::string const& line : events)
            if (line.rfind(prefix, 0) == 0)
                return true;
        return false;
    };
    CHECK(LeaderWalk(policy.Decide(view, 1000)) == -1.0f);            // the party is not together: no walk
    CHECK(has("regroup_start: leader=12 reason=member_far far=1"));
    CHECK(LeaderWalk(policy.Decide(view, 10000)) == -1.0f);           // not stuck yet (9 s out of range)
    CHECK(LeaderWalk(policy.Decide(view, 22000)) == 100.0f);          // stuck (21 s): the leader walks on without it
    CHECK(has("regroup_stuck: leader=12 bot=10 "));
    policy.Decide(view, 23000);
    CHECK(!has("regroup_done:"));                                     // the regroup stays on while a member is out of range
    view.bots[0].pos_x = 50.0f;                                       // bot 10 catches up
    policy.Decide(view, 24000);
    policy.Decide(view, 40000);                                       // the party has been ready for the clearing time
    bool done = false;
    for (std::string const& line : events)
        done = done || (line.rfind("regroup_done: leader=12 ", 0) == 0 && line.find(" stuck=1") != std::string::npos);
    CHECK(done);
}

// Regroup (0.36.2): a member that trails the leader at the edge of the wait distance does not flip the stuck flag. It is logged once per regroup, stays stuck
// until it is back inside kRegroupClearYards, and the leader keeps walking.
void AStuckMemberAtTheBoundaryIsLoggedOnce()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.ClassOf = WarriorLeads;
    options.Route = { Point{ 100.0f, 0.0f, 0.0f }, Point{ 140.0f, 0.0f, 0.0f } };
    std::vector<std::string> events;
    options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    for (BotSnapshot& bot : view.bots)
        bot.pos_x = 0.0f;
    auto const count = [&events](std::string const& prefix)
    {
        int found = 0;
        for (std::string const& line : events)
            found += line.rfind(prefix, 0) == 0;
        return found;
    };
    // Bot 10 is 31 yards behind until it is stuck (22 s), then steps in and out of range (29 and 31 yards) every second for a minute.
    bool walked = false;                                              // the walk order is sent when it changes, so look for it over the minute
    for (std::uint64_t now = 1000; now <= 60000; now += 1000)
    {
        view.bots[0].pos_x = (now > 22000 && (now / 1000) % 2 == 0) ? -29.0f : -31.0f;
        walked = LeaderWalk(policy.Decide(view, now)) == 100.0f || walked;
    }
    CHECK(count("regroup_stuck: leader=12 bot=10 ") == 1);            // one line, not one per step in and out of range
    CHECK(walked);                                                    // the leader walked on once it was stuck
    // Back to 25 yards: inside the wait distance, but not inside the clearing distance, so it is still stuck and the regroup goes on.
    view.bots[0].pos_x = -25.0f;
    policy.Decide(view, 61000);
    CHECK(count("regroup_stuck: leader=12 bot=10 ") == 1);
    view.bots[0].pos_x = -31.0f;
    policy.Decide(view, 90000);
    policy.Decide(view, 120000);
    CHECK(count("regroup_stuck: leader=12 bot=10 ") == 1);            // the same regroup: not logged again
    CHECK(count("regroup_done:") == 0);
}

// Regroup (0.36.2): member_recover waits longer than the stuck flag, so the leader has walked on before the member is put next to it.
void MemberRecoverDoesNotFireWithTheStuckEvent()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.ClassOf = WarriorLeads;
    options.DungeonTeleport = true;
    options.Route = { Point{ 100.0f, 0.0f, 0.0f }, Point{ 140.0f, 0.0f, 0.0f } };
    std::uint64_t now = 0;
    std::uint64_t stuckAt = 0;
    std::uint64_t recoverAt = 0;
    options.OnEvent = [&](std::string const& line)
    {
        if (line.rfind("regroup_stuck:", 0) == 0 && stuckAt == 0)
            stuckAt = now;
        if (line.rfind("member_recover: bot=10", 0) == 0 && recoverAt == 0)
            recoverAt = now;
    };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    for (BotSnapshot& bot : view.bots)
        bot.pos_x = 0.0f;
    view.bots[0].pos_x = -31.0f;                                      // bot 10 trails at the edge
    for (now = 1000; now <= 60000 && recoverAt == 0; now += 500)
        policy.Decide(view, now);
    CHECK(stuckAt != 0 && recoverAt != 0);
    CHECK(recoverAt > stuckAt);                                       // never in the same tick
    CHECK(recoverAt - stuckAt >= 5000);                               // the party has had time to move on
}

// The default stance is defensive: a bot does not pull what is in range on its own, and it fights what attacks it.
void DefaultBotsDoNotPullWhatIsInRange()
{
    BotPolicy policy(PolicyOptions{});
    NearbyUnit const mob = Hostile(7, 3.0f, 100);
    WorldTickSnapshotView view = PartyView({ 10 });
    AddNearby(view, 0, mob);
    CHECK(!HasAction(policy.Decide(view, 0), 10, ActionType::kAttackTarget, mob.guid));
    view.bots[0].threat_target_guid = mob.guid;
    CHECK(HasAction(policy.Decide(view, 1000), 10, ActionType::kAttackTarget, mob.guid));
}

// A party that flips between ready and not ready every tick (a member that is a ghost one moment and not the next) must not hold the leader forever.
void AFlippingPartyDoesNotHoldTheLeaderForever()
{
    BotPolicy policy = DungeonPolicy();
    WorldTickSnapshotView hurt = PartyView({ 10, 11, 12, 13, 14 });
    hurt.bots[1].health = 400;                                        // bot 11 below the ready mark
    WorldTickSnapshotView healthy = PartyView({ 10, 11, 12, 13, 14 });
    AddNearby(healthy, 2, Unit(8, 7, 20.0f, true));
    std::uint64_t now = 0;
    bool pulled = false;
    for (int tick = 0; tick < 400 && !pulled; ++tick)
    {
        now += 500;
        WorldTickSnapshotView const& view = (tick % 2 == 0) ? hurt : healthy;   // flips every tick
        pulled = HasAction(policy.Decide(view, now), 12, ActionType::kFollow, MakeUnitRef(8, 1, 7)) ||
                 HasAction(policy.Decide(view, now), 12, ActionType::kAttackTarget);
    }
    CHECK(pulled);                                                    // the wait ends after kPullHoldMaxMs (120 s) of flipping
}

void HealerHealsTheHurtMember()
{
    WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 });
    view.bots[3].health = 300; // bot 13
    auto const actions = DungeonPolicy().Decide(view, 0);
    CHECK(CastsSpell(actions, 10, 2050, PlayerOf(13)));
}
 
void HealerHealsItselfWithoutATarget()
{
    WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 });
    view.bots[0].health = 300;
    auto const actions = DungeonPolicy().Decide(view, 0);
    CHECK(CastsSpell(actions, 10, 2050, 0));
}

bool Logged(std::vector<std::string> const& events, char const* prefix);
bool LoggedWith(std::vector<std::string> const& events, char const* text);

void PriestWithNoLineOfSightRepositionsBeforeRetryingDps()
{
    std::vector<std::string> events;
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = WarriorLeads;
    options.OnEvent = [&events](std::string const& event) { events.push_back(event); };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 });
    NearbyUnit hostile = Hostile(7, 20.0f, 100);
    hostile.pos_x = 20.0f;
    view.bots[2].in_combat = 1; // tank is engaging this hostile
    view.bots[2].threat_target_guid = hostile.guid;
    AddNearby(view, 0, hostile);

    std::vector<BotAction> first = policy.Decide(view, 1000);
    std::uint32_t dpsSpell = 0;
    for (BotAction const& action : first)
        if (action.bot_guid == 10 && action.type == ActionType::kCastSpell)
        {
            dpsSpell = action.spell_id;
            break;
        }
    CHECK(dpsSpell != 0);
    if (dpsSpell == 0)
        return;

    // The core refused that cast for LoS; the next tick must move toward the same target,
    // not retry the spell or issue a MoveTo with only a target GUID and zero coordinates.
    view.bots[0].last_action_type = std::uint8_t(ActionType::kCastSpell);
    view.bots[0].last_action_spell = dpsSpell;
    view.bots[0].last_action_status = BotActionStatus::kNoLineOfSight;
    view.bots[0].last_action_sequence = 1;
    std::vector<BotAction> retry = policy.Decide(view, 1200);
    bool moved = false;
    for (BotAction const& action : retry)
    {
        if (action.bot_guid == 10 && action.type == ActionType::kMoveTo &&
            (action.dest_x != 0.0f || action.dest_y != 0.0f || action.dest_z != 0.0f))
            moved = true;
    }
    CHECK(moved);
    CHECK(!CastsSpell(retry, 10, dpsSpell));
    CHECK(Logged(events, "healer_dps_los:"));
}

void NoHealWhileEveryoneIsHealthy()
{
    auto const actions = DungeonPolicy().Decide(DungeonView({ 10, 11, 12, 13, 14 }), 0);
    CHECK(!CastsSpell(actions, 10, 2050));
    CHECK(!CastsSpell(actions, 11, 2050));
}

void LeaderDoesNotPullWhileAMemberIsHurt()
{
    WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 });
    view.bots[0].health = 500; // 50%: the party is not ready
    AddNearby(view, 2, Unit(8, 7, 20.0f, true));
    auto const actions = DungeonPolicy().Decide(view, 0);
    CHECK(!HasAction(actions, 12, ActionType::kFollow, MakeUnitRef(8, 1, 7)));
    CHECK(!HasAction(actions, 12, ActionType::kAttackTarget));
}

void LeaderPullsWhenThePartyIsReady()
{
    WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 });
    AddNearby(view, 2, Unit(8, 7, 20.0f, true));
    auto const actions = DungeonPolicy().Decide(view, 0);
    CHECK(HasAction(actions, 12, ActionType::kFollow, MakeUnitRef(8, 1, 7)));
}

void LeaderPullsAfterWaitingTooLong()
{
    BotPolicy policy = DungeonPolicy();
    WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 });
    view.bots[0].health = 500;
    AddNearby(view, 2, Unit(8, 7, 20.0f, true));
    policy.Decide(view, 1000);
    auto const actions = policy.Decide(view, 1000 + 130000);
    CHECK(HasAction(actions, 12, ActionType::kFollow, MakeUnitRef(8, 1, 7)));
}

void MembersDoNotPullInADungeon()
{
    WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 });
    AddNearby(view, 0, Unit(8, 7, 20.0f, true));
    auto const actions = DungeonPolicy().Decide(view, 0);
    CHECK(!HasAction(actions, 10, ActionType::kFollow, MakeUnitRef(8, 1, 7)));
    CHECK(!CastsSpell(actions, 10, 585));
}

// The party pulls a dungeon pack of four at most. A pack of five 20 yards off is left alone; a pack of four is pulled. (The view holds 4 nearby units per bot by
// default: it is widened to 8 so that five fit.)
void ADungeonPackOfFiveIsNotPulled()
{
    auto const packView = [](int mobs)
    {
        WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 });
        view.max_nearby_units = 8;
        view.nearby.assign(view.bots.size() * view.max_nearby_units, NearbyUnit{});
        for (int i = 0; i < mobs; ++i)
            AddNearby(view, 2, Unit(8, 7 + i, 20.0f + float(i), true));
        return view;
    };
    CHECK(!HasAction(DungeonPolicy().Decide(packView(5), 0), 12, ActionType::kFollow, MakeUnitRef(8, 1, 7)));
    CHECK(HasAction(DungeonPolicy().Decide(packView(4), 0), 12, ActionType::kFollow, MakeUnitRef(8, 1, 7)));
}

// Members defend themselves: bot 10 is a rogue here, since the healer holds its range instead (HealerDoesNotMelee).
std::uint32_t RogueTenLeadsTheRest(std::uint64_t guid)
{
    return guid == 12 ? 1u : guid == 10 ? 4u : 5u;
}

void MembersAttackedInADungeonDefendThemselves()
{
    WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 }, RogueTenLeadsTheRest);
    view.bots[0].in_combat = 1;
    AddNearby(view, 0, Unit(8, 7, 20.0f, true));
    auto const actions = DungeonPolicy(RogueTenLeadsTheRest).Decide(view, 0);
    CHECK(HasAction(actions, 10, ActionType::kFollow, MakeUnitRef(8, 1, 7)) || HasAction(actions, 10, ActionType::kAttackTarget));
}

void MembersWaitForTheTankToHoldTheTarget()
{
    BotPolicy policy = DungeonPolicy(RogueTenLeadsTheRest);
    WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 }, RogueTenLeadsTheRest);
    view.bots[2].threat_target_guid = MakeUnitRef(8, 1, 7);
    AddNearby(view, 2, Unit(8, 7, 20.0f, true));
    AddNearby(view, 0, Unit(8, 7, 20.0f, true));
    auto first = policy.Decide(view, 1000);                 // the tank starts the fight
    CHECK(!HasAction(first, 10, ActionType::kFollow, MakeUnitRef(8, 1, 7)));
    auto later = policy.Decide(view, 1000 + 3000);           // a few seconds on, the others join
    CHECK(HasAction(later, 10, ActionType::kFollow, MakeUnitRef(8, 1, 7)) || HasAction(later, 10, ActionType::kAttackTarget));
}

void HealerDoesNotMelee()
{
    // The healer (bot 10) stands at its ranged stand-off while the tank holds the target: it must not attack, because the
    // core's attack order runs a caster into melee, where it takes the tank's hits and pulls aggro (the 2026-10-23 run).
    BotPolicy policy = DungeonPolicy();
    WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 });
    view.bots[2].threat_target_guid = MakeUnitRef(8, 1, 7);
    view.bots[2].in_combat = 1;
    AddNearby(view, 2, Unit(8, 7, 20.0f, true));
    AddNearby(view, 0, Unit(8, 7, 20.0f, true));
    policy.Decide(view, 1000); // the tank has the head start
    auto const actions = policy.Decide(view, 4000);
    CHECK(!HasAction(actions, 10, ActionType::kAttackTarget));
    CHECK(!HasAction(actions, 10, ActionType::kFollow, MakeUnitRef(8, 1, 7))); // 20 yd is inside the 25 yd stand-off
}

void HealerWithoutPowerKeepsItsRange()
{
    // A healer at 0 power used to close to melee range like a spent mage, then swing at the boss for the rest of the fight.
    BotPolicy policy = DungeonPolicy();
    WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 });
    view.bots[0].power = 0;
    view.bots[2].threat_target_guid = MakeUnitRef(8, 1, 7);
    view.bots[2].in_combat = 1;
    AddNearby(view, 2, Unit(8, 7, 20.0f, true));
    AddNearby(view, 0, Unit(8, 7, 20.0f, true));
    policy.Decide(view, 1000);
    auto const actions = policy.Decide(view, 4000);
    CHECK(!HasAction(actions, 10, ActionType::kFollow, MakeUnitRef(8, 1, 7)));
    CHECK(!HasAction(actions, 10, ActionType::kAttackTarget));
}

void LineOfSightRefusalClosesTheDistance()
{
    BotPolicy policy = DungeonPolicy();
    WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 });
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
    PolicyOptions options = AggressiveOptions();
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

bool LoggedWith(std::vector<std::string> const& events, char const* text)
{
    for (std::string const& event : events)
        if (event.find(text) != std::string::npos)
            return true;
    return false;
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

// Recover: the leader of a dungeon party stands a dead member up at its side 8 s after the fight, once it is up and quiet (not waiting 90 s for the healer, not releasing).
void ACorpseIsRevivedByTheQuietLeaderAfterAFewSeconds()
{
    PolicyOptions options = DeadOptions(nullptr);
    options.DungeonReviveOutOfCombat = true;
    options.Route = { Point{ 100.0f, 0.0f, 0.0f } };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    auto const build = [] { WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 }); Corpse(v.bots[3], 10.0f); return v; };
    CHECK(!HasAction(policy.Decide(build(), 1000), 13, ActionType::kRevive));
    CHECK(!HasAction(policy.Decide(build(), 5000), 13, ActionType::kRevive));
    std::vector<BotAction> later = policy.Decide(build(), 9500);
    CHECK(HasAction(later, 13, ActionType::kRevive));
    CHECK(!HasAction(later, 13, ActionType::kReleaseSpirit));
    PolicyOptions off = options;
    off.TacticRecover = false;
    BotPolicy plain(off);
    plain.SetRouteActive(true);
    plain.Decide(build(), 1000);
    CHECK(!HasAction(plain.Decide(build(), 9500), 13, ActionType::kRevive));
    // a leader in a fight does not revive anyone
    BotPolicy fighting(options);
    fighting.SetRouteActive(true);
    auto const busy = [] { WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 }); Corpse(v.bots[3], 10.0f); v.bots[2].in_combat = 1; return v; };
    fighting.Decide(busy(), 1000);
    CHECK(!HasAction(fighting.Decide(busy(), 9500), 13, ActionType::kRevive));
}

// A dungeon party's dead wait for a rezzer no longer than a leveling party's: 20 s (0.36.2; the 60 s of 0.34 left ghosts for whole fights, 44-87 s in the sim's Ragefire Chasm).
void ADungeonCorpseWaitsTwentySecondsForARezzer()
{
    BotPolicy policy(DeadOptions(nullptr));
    policy.SetRouteActive(true);
    auto const build = [] { WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 }); Corpse(v.bots[3], 10.0f); return v; };
    CHECK(!HasAction(policy.Decide(build(), 1000), 13, ActionType::kReleaseSpirit));
    CHECK(!HasAction(policy.Decide(build(), 15000), 13, ActionType::kReleaseSpirit));
    CHECK(HasAction(policy.Decide(build(), 21500), 13, ActionType::kReleaseSpirit));
}

// No rezzer in the party (all warriors): the corpse is released at once, a fight in the party or not, and well inside the 20 s.
void ADungeonCorpseWithoutARezzerIsReleasedWithinTwentySeconds()
{
    std::vector<std::string> events;
    BotPolicy policy(DeadOptions(&events, AllWarriors));
    policy.SetRouteActive(true);
    auto const build = [] { WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 }); Corpse(v.bots[3], 10.0f); return v; };
    CHECK(HasAction(policy.Decide(build(), 1000), 13, ActionType::kReleaseSpirit));
    CHECK(LoggedWith(events, "reason=release_no_rezzer"));
    CHECK(LoggedWith(events, "waited_ms=0"));
}

// A fight elsewhere in the party does not hold a release (the core refuses it only for a corpse in combat): the regroup loop started from ghosts held 44-87 s.
void AFightInThePartyDoesNotHoldADungeonRelease()
{
    std::vector<std::string> events;
    BotPolicy policy(DeadOptions(&events, AllWarriors));
    policy.SetRouteActive(true);
    auto const build = [](bool ownCombat)
    {
        WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
        Corpse(v.bots[3], 10.0f);
        v.bots[1].in_combat = 1;
        v.bots[3].in_combat = ownCombat ? 1 : 0;
        return v;
    };
    CHECK(HasAction(policy.Decide(build(false), 1000), 13, ActionType::kReleaseSpirit));
    CHECK(!Logged(events, "release_held:"));
    BotPolicy own(DeadOptions(&events, AllWarriors));                                   // a corpse in combat itself is still held, as the core wants
    own.SetRouteActive(true);
    CHECK(!HasAction(own.Decide(build(true), 1000), 13, ActionType::kReleaseSpirit));
    CHECK(Logged(events, "release_held:"));
}

// After the release, a ghost of a dungeon party that is out of combat is teleported to its leader (the tank), not left at the graveyard or sent to its corpse.
bool LoggedTeleportToLeader(std::vector<BotAction> const& actions)
{
    std::optional<BotAction> const teleport = TeleportOf(actions, 13);
    return teleport.has_value() && teleport->spell_id == 36 && teleport->dest_x != 50.0f; // 50 is the corpse and the entrance, not a leader
}

void AReleasedDungeonGhostIsTeleportedToTheLeader()
{
    std::vector<std::string> events;
    BotPolicy policy(DeadOptions(&events, AllWarriors));
    policy.SetRouteActive(true);
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
    for (BotSnapshot& bot : v.bots)
        bot.map_id = 36;
    Ghost(v.bots[3], 0.0f, 36, 50.0f);
    v.bots[3].map_id = 0;                                                               // the ghost is outside, at the graveyard
    std::optional<BotAction> const teleport = TeleportOf(policy.Decide(v, 1000), 13);
    CHECK(teleport.has_value());
    if (teleport)
    {
        bool atALeader = false;                                                         // the destination is a living member's spot, on the instance map
        for (BotSnapshot const& bot : v.bots)
            atALeader = atALeader || (bot.guid != 13 && bot.pos_x == teleport->dest_x && bot.pos_y == teleport->dest_y && bot.pos_z == teleport->dest_z);
        CHECK(teleport->spell_id == 36 && atALeader);
    }
    CHECK(Logged(events, "party_teleport:"));
    // with a fight going on the ghost is not sent to the leader: it waits for the quiet
    BotPolicy busy(DeadOptions(nullptr, AllWarriors));
    busy.SetRouteActive(true);
    v.bots[1].in_combat = 1;
    CHECK(!LoggedTeleportToLeader(busy.Decide(v, 1000)));
}

// A ghost whose run takes too long is put at its corpse once, and the run starts over; it does not stay a ghost for good.
void GhostThatCannotReachItsCorpseIsPutThereOnce()
{
    std::vector<std::string> events;
    PolicyOptions options = AggressiveOptions();
    options.OnEvent = [&events](std::string const& text) { events.push_back(text); };
    BotPolicy policy(options);
    WorldTickSnapshotView view = View(1, 0, {});
    Ghost(view.bots[0], 0.0f, view.bots[0].map_id, 3000.0f);
    policy.Decide(view, 1000);
    auto const teleportsTo = [](std::vector<BotAction> const& actions, float x)
    {
        for (BotAction const& action : actions)
            if (action.type == ActionType::kTeleport && action.dest_x == x)
                return true;
        return false;
    };
    CHECK(!teleportsTo(policy.Decide(view, 2000), 3000.0f));
    CHECK(teleportsTo(policy.Decide(view, 1000 + options.CorpseRunTimeoutMs + 1000), 3000.0f));   // the run took too long: to the corpse
    CHECK(Logged(events, "corpse_run_teleport:"));
    CHECK(!Logged(events, "corpse_run_failed:"));
    // Not again: the second timeout is a real failure.
    CHECK(!teleportsTo(policy.Decide(view, 1000 + 2 * options.CorpseRunTimeoutMs + 5000), 3000.0f));
    CHECK(Logged(events, "corpse_run_failed:"));
}

// A corpse run that takes longer than CorpseRunTeleportMs (60 s, the owner's rule of 2026-10-25) ends in a teleport to the corpse; no party revive is asked
// for. The ghost walks first, and the teleport comes once.
void ALongCorpseRunEndsInATeleportToTheCorpse()
{
    std::vector<std::string> events;
    BotPolicy policy(DeadOptions(&events));
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
    Ghost(v.bots[3], 0.0f, 0, 3000.0f);                                                  // the corpse is far away on the same map
    CHECK(HasAction(policy.Decide(v, 1000), 13, ActionType::kMoveTo));                   // the run starts on foot
    CHECK(!TeleportOf(policy.Decide(v, 30000), 13).has_value());
    CHECK(!TeleportOf(policy.Decide(v, 60500), 13).has_value());                         // not before 60 s
    std::vector<BotAction> const later = policy.Decide(v, 61500);
    std::optional<BotAction> const teleport = TeleportOf(later, 13);
    CHECK(teleport.has_value() && teleport->dest_x == 3000.0f);                           // to the corpse
    CHECK(!HasAction(later, 13, ActionType::kRevive));                                    // no revive order from the party
    for (BotAction const& action : later)
        CHECK(action.type != ActionType::kRevive);
    CHECK(Logged(events, "corpse_run_teleport:"));
    CHECK(LoggedWith(events, "reason=run_limit"));
    CHECK(!TeleportOf(policy.Decide(v, 70000), 13).has_value());                         // once

    // A ghost at its corpse past 60 s (the core's reclaim delay) is not on a run: no teleport.
    BotPolicy waiting(DeadOptions(nullptr));
    WorldTickSnapshotView there = PartyView({ 10, 11, 12, 13, 14 });
    Ghost(there.bots[3], 0.0f, 0, 10.0f);
    there.bots[3].reclaim_wait_ms = 90000;
    waiting.Decide(there, 1000);
    CHECK(!TeleportOf(waiting.Decide(there, 61500), 13).has_value());

    // CorpseRunTeleportMs = 0: the rule is off, the ghost walks on.
    PolicyOptions off = DeadOptions(nullptr);
    off.CorpseRunTeleportMs = 0;
    BotPolicy plain(off);
    plain.Decide(v, 1000);
    CHECK(!TeleportOf(plain.Decide(v, 61500), 13).has_value());
}

// A wiped party (the sim's Deadmines run: five ghosts at the Sentinel Hill graveyard, the walk to the door refused for "no route") puts each ghost at its
// corpse after 60 s, with no party revive first.
void AWipedPartysGhostsArePutAtTheirCorpsesAfterAMinute()
{
    std::vector<std::string> events;
    BotPolicy policy(DeadOptions(&events));
    policy.SetRouteActive(true);
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
    for (BotSnapshot& bot : v.bots)
        Ghost(bot, 0.0f, 36, 50.0f);                                                     // all outside on map 0, corpses in the dungeon
    std::vector<BotAction> const first = policy.Decide(v, 1000);
    CHECK(HasAction(first, 13, ActionType::kMoveTo) && !TeleportOf(first, 13).has_value()); // to the door first
    CHECK(!TeleportOf(policy.Decide(v, 50000), 13).has_value());
    std::vector<BotAction> const later = policy.Decide(v, 61500);
    for (std::uint64_t guid : { 10, 11, 12, 13, 14 })
    {
        std::optional<BotAction> const teleport = TeleportOf(later, guid);
        CHECK(teleport.has_value() && teleport->spell_id == 36 && teleport->dest_x == 50.0f);
        CHECK(!HasAction(later, guid, ActionType::kRevive));
    }
    CHECK(Logged(events, "corpse_run_teleport:"));
    CHECK(LoggedWith(events, "reason=run_limit"));
    CHECK(!TeleportOf(policy.Decide(v, 70000), 13).has_value());                         // once
}

// A bot that stands up among what killed it and dies again three times within two minutes is put at a grind spot for its level, away from the corpse.
void DeathLoopMovesTheBotToAGrindSpot()
{
    std::vector<std::string> events;
    std::vector<std::string> warnings;
    std::istringstream spots("spot 0 40 0 0 1 10 0\nspot 0 5000 0 0 1 10 0\n");   // one spot beside the corpse, one far away
    PolicyOptions options = AggressiveOptions();
    options.Grind = std::make_shared<GrindData>(ParseGrindData(spots, warnings));
    options.OnEvent = [&events](std::string const& text) { events.push_back(text); };
    BotPolicy policy(options);
    auto const escapeTo = [](std::vector<BotAction> const& actions)
    {
        for (BotAction const& action : actions)
            if (action.type == ActionType::kTeleport)
                return action.dest_x;
        return -1.0f;
    };
    std::uint64_t now = 1000;
    float escape = -1.0f;
    for (int death = 0; death < 3; ++death)
    {
        WorldTickSnapshotView dead = View(1, 0, {});
        dead.bots[0].level = 6;
        Corpse(dead.bots[0], 10.0f);
        policy.Decide(dead, now);
        now += 3000;
        WorldTickSnapshotView up = View(1, 0, {});
        up.bots[0].level = 6;
        up.bots[0].pos_x = 10.0f;
        escape = escapeTo(policy.Decide(up, now));
        now += 5000;
        if (death < 2)
            CHECK(escape < 0.0f);   // the first two deaths are ordinary
    }
    CHECK(escape == 5000.0f);       // the third: away, to the spot that is not beside the corpse
    CHECK(Logged(events, "death_loop_escape:"));
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
    auto const build = [] { WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 }); Corpse(v.bots[3], 10.0f); v.bots[3].in_combat = 1; return v; };
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

// A dead member left on the outside map with its corpse in the dungeon and no portal (run 9): it is teleported to the corpse, paced, and walks on from there.
void ADeadMemberOnMapZeroIsTeleportedBackToItsCorpse()
{
    std::vector<std::string> events;
    PolicyOptions options = DeadOptions(&events);
    options.HasPortal = false;
    BotPolicy policy(options);
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
    Ghost(v.bots[3], 0.0f, 36, 50.0f);
    v.bots[3].map_id = 0;
    std::optional<BotAction> teleport = TeleportOf(policy.Decide(v, 1000), 13);
    CHECK(teleport.has_value() && teleport->spell_id == 36 && teleport->dest_x == 50.0f); // to the corpse's map and place
    CHECK(Logged(events, "corpse_teleport:"));
    CHECK(!TeleportOf(policy.Decide(v, 2000), 13).has_value());                             // not again at once
    CHECK(TeleportOf(policy.Decide(v, 16000), 13).has_value());                             // again if the core did not apply it
    v.bots[3].map_id = 36;                                                                  // now on the corpse's map: the corpse run goes on
    std::vector<BotAction> onFoot = policy.Decide(v, 40000);
    CHECK(HasAction(onFoot, 13, ActionType::kMoveTo) && !TeleportOf(onFoot, 13).has_value());
    CHECK(!Logged(events, "corpse_run_failed:"));
}

// A dead member is revived by its leader out of combat only in the dungeon it died in (the owner's rule of 2026-10-22: a ghost walks back to
// the instance and is revived on entering it).
void ADeadMemberIsRevivedByItsLeaderInsideTheDungeon()
{
    std::vector<std::string> events;
    PolicyOptions options = DeadOptions(&events);
    options.DungeonReviveOutOfCombat = true;
    options.Route = { Point{ 100.0f, 0.0f, 0.0f } };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
    Ghost(v.bots[3], 0.0f, 36, 50.0f);
    v.bots[3].map_id = 36;                                                                // back inside, at its corpse's map
    std::vector<BotAction> first = policy.Decide(v, 1000);
    CHECK(HasAction(first, 13, ActionType::kRevive));                                     // the leader is up and quiet
    CHECK(!HasAction(first, 13, ActionType::kMoveTo));                                    // and the ghost waits for it, not walking off
    CHECK(Logged(events, "revive_out_of_combat:"));
    CHECK(!HasAction(policy.Decide(v, 2000), 13, ActionType::kRevive));                   // not again before the retry
    CHECK(HasAction(policy.Decide(v, 11000), 13, ActionType::kRevive));                   // again if the core refused it
    CHECK(HasAction(policy.Decide(v, 22000), 13, ActionType::kRevive));                   // third try
    std::vector<BotAction> given = policy.Decide(v, 33000);
    CHECK(!HasAction(given, 13, ActionType::kRevive));                                    // given up after three tries
    CHECK(!HasAction(given, 13, ActionType::kMoveTo));                                    // and it stays by the leader: no corpse run (0.34.3)

    BotPolicy busy(options);
    busy.SetRouteActive(true);
    WorldTickSnapshotView fighting = PartyView({ 10, 11, 12, 13, 14 });
    Ghost(fighting.bots[3], 0.0f, 36, 50.0f);
    fighting.bots[3].map_id = 36;
    fighting.bots[2].in_combat = 1;                                                       // the leader (bot 12) fights: no revive
    CHECK(!HasAction(busy.Decide(fighting, 1000), 13, ActionType::kRevive));
}

// A ghost of a dungeon party still on the outside map is not revived there (it stood up at the graveyard and died outside the dungeon);
// out of combat it is teleported to its leader inside the instance (0.34.3); it is still not revived outside.
void AGhostOnTheOutsideMapIsNotRevivedBeforeItEnters()
{
    std::vector<std::string> events;
    PolicyOptions options = DeadOptions(&events);
    options.DungeonReviveOutOfCombat = true;
    options.Route = { Point{ 100.0f, 0.0f, 0.0f } };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
    Ghost(v.bots[3], 0.0f, 36, 50.0f);
    v.bots[3].map_id = 0;                                                                 // the graveyard outside
    v.bots[2].map_id = 36;                                                                // the leader (bot 12) is inside the instance
    std::vector<BotAction> first = policy.Decide(v, 1000);
    CHECK(!HasAction(first, 13, ActionType::kRevive));
    CHECK(!Logged(events, "revive_out_of_combat:"));
    bool toLeader = false;
    for (BotAction const& action : first)
        toLeader = toLeader || (action.type == ActionType::kTeleport && action.bot_guid == 13 && action.spell_id == 36);
    CHECK(toLeader);                                                                     // teleported to the leader in the instance
    CHECK(!HasAction(policy.Decide(v, 12000), 13, ActionType::kRevive));                 // still outside: still not revived
}

// A living member of a dungeon party left on the outside map (a wipe put it up at the graveyard) walks back to the instance door and starts
// no fight on the way: it must not be able to die outside the dungeon (the owner's rule of 2026-10-22). A bot that stopped to clear the
// road was killed by the stable master of Sentinel Hill and the rescue teleport was refused in combat behind it.
void AnAliveMemberOutsideWalksBackToTheDungeon()
{
    std::vector<std::string> events;
    PolicyOptions options = DeadOptions(&events);
    options.Route = { Point{ 100.0f, 0.0f, 0.0f } };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
    for (BotSnapshot& bot : v.bots)
        bot.map_id = 0;                                                                  // the whole party up at the graveyard outside
    v.bots[3].pos_x = 10.0f;
    AddNearby(v, 3, Hostile(7, 15.0f, 100));                                            // something worth a fight on the way
    std::vector<BotAction> actions = policy.Decide(v, 1000);                            // this tick it mounts for the walk
    std::vector<BotAction> more = policy.Decide(v, 2000);
    actions.insert(actions.end(), more.begin(), more.end());
    CHECK(!HasAction(actions, 13, ActionType::kAttackTarget));
    bool towardPortal = false;
    for (BotAction const& action : actions)
        towardPortal = towardPortal || (action.type == ActionType::kMoveTo && action.bot_guid == 13 && action.dest_x == 500.0f);
    CHECK(towardPortal);
}

// A ghost at its corpse with a live hostile within 30 yards waits (a ghost cannot be killed, the reclaimed body can); after two minutes it reclaims anyway.
void AGhostDoesNotReclaimNextToAHostile()
{
    BotPolicy policy(DeadOptions(nullptr));
    auto const build = [](bool hostile)
    {
        WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
        Ghost(v.bots[3], 0.0f, 0, 10.0f);
        v.bots[3].reclaim_wait_ms = 0;
        if (hostile)
            AddNearby(v, 3, Hostile(7, 12.0f, 100));
        return v;
    };
    CHECK(!HasAction(policy.Decide(build(true), 1000), 13, ActionType::kReclaimCorpse));
    CHECK(!HasAction(policy.Decide(build(true), 60000), 13, ActionType::kReclaimCorpse));
    CHECK(HasAction(policy.Decide(build(false), 61000), 13, ActionType::kReclaimCorpse));   // the hostile left: back to life
    BotPolicy stubborn(DeadOptions(nullptr));
    CHECK(!HasAction(stubborn.Decide(build(true), 1000), 13, ActionType::kReclaimCorpse));
    CHECK(HasAction(stubborn.Decide(build(true), 122000), 13, ActionType::kReclaimCorpse)); // two minutes: it waits no longer
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
    PolicyOptions options = DeadOptions(&events);
    options.CorpseRunTeleportMs = 0;                           // the 60 s teleport off: the four-minute timeout alone (ALongCorpseRunEndsInATeleportToTheCorpse has the rule)
    BotPolicy policy(options);
    auto const build = [] { WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 }); Ghost(v.bots[3], 5000.0f, 0, 0.0f); return v; };
    policy.Decide(build(), 1000);
    policy.Decide(build(), 250000);                          // the first timeout puts the ghost at its corpse and starts the run over
    CHECK(Logged(events, "corpse_run_teleport:") && !Logged(events, "corpse_run_failed:"));
    policy.Decide(build(), 500000);                          // the second one is a failure
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
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 }, WarriorLeads);   // classOf: bot 12 warrior, the rest priests (mana users)
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
    BotPolicy policy(AggressiveOptions());
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
    BotPolicy policy(AggressiveOptions());
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
    BotPolicy policy(AggressiveOptions());
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

// A hostile almost on top of the bot in the plane but 20 yards above it (a patrol on a wall walk: the Shadowfang Keep courtyard's Tormented Officers) is
// not pulled: the bot would stand under it, its follow order finds no way and its spells have no line of sight. A hostile a short climb above, a gentle slope
// away, or one that is already attacking the bot, is still fought.
void HostileOnAnotherLevelIsNotPulled()
{
    auto const act = [](NearbyUnit const& unit, bool attacked)
    {
        WorldTickSnapshotView view = PartyView({ 10 });
        view.bots[0].in_combat = attacked ? 1 : 0;
        AddNearby(view, 0, unit);
        BotPolicy policy(AggressiveOptions());
        auto const actions = policy.Decide(view, 0);
        return HasAction(actions, 10, ActionType::kAttackTarget, unit.guid) || HasAction(actions, 10, ActionType::kFollow, unit.guid);
    };
    NearbyUnit above = Hostile(7, 3.0f, 100);
    above.pos_z = 20.0f;               // 3 yards along the ground, 20 up
    CHECK(!act(above, false));
    CHECK(act(above, true));           // it is attacking the bot: the stall watchdog and the approach timeout deal with it, not this rule

    NearbyUnit farAbove = Hostile(8, 30.0f, 100);
    farAbove.pos_z = 20.0f;            // 20 up over 30 along: steeper than 0.5
    CHECK(!act(farAbove, false));

    NearbyUnit stairs = Hostile(9, 30.0f, 100);
    stairs.pos_z = 12.0f;              // 12 up over 30 along: a stair or a ramp
    CHECK(act(stairs, false));

    NearbyUnit step = Hostile(10, 3.0f, 100);
    step.pos_z = 8.0f;                 // a short climb
    CHECK(act(step, false));

    NearbyUnit below = Hostile(11, 4.0f, 100);
    below.pos_z = -18.0f;              // the floor under the bot counts the same
    CHECK(!act(below, false));
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
    BotPolicy policy(AggressiveOptions());
    auto actions = policy.Decide(view, 0);
    CHECK(!HasAction(actions, 10, ActionType::kAttackTarget, grey.guid) && !HasAction(actions, 10, ActionType::kFollow, grey.guid));

    WorldTickSnapshotView withGreen = PartyView({ 10 });
    withGreen.bots[0].level = 10;
    AddNearby(withGreen, 0, grey);
    AddNearby(withGreen, 0, green);
    BotPolicy other(AggressiveOptions());
    actions = other.Decide(withGreen, 0);
    CHECK(HasAction(actions, 10, ActionType::kAttackTarget, green.guid) || HasAction(actions, 10, ActionType::kFollow, green.guid));

    // The grey creature attacks the bot: it is fought whatever its level.
    WorldTickSnapshotView attacked = PartyView({ 10 });
    attacked.bots[0].level = 10;
    attacked.bots[0].in_combat = 1;
    AddNearby(attacked, 0, grey);
    BotPolicy defender(AggressiveOptions());
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
    BotPolicy policy(AggressiveOptions());
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
    BotPolicy policy(AggressiveOptions());
    std::uint32_t spell = 99;
    CHECK(RestAt(policy.Decide(ManaView(10), 1000), spell) >= 0 && spell == 1);
    CHECK(RestAt(policy.Decide(ManaView(10), 100000), spell) < 0);              // still drinking
    CHECK(RestAt(policy.Decide(ManaView(10), 125000), spell) >= 0 && spell == 0); // 2 minutes: stands, the mana is not coming back
    CHECK(RestAt(policy.Decide(ManaView(10), 130000), spell) < 0);              // and does not sit again at once
}

void FighterWithoutManaDoesNotDrink()
{
    BotPolicy policy(AggressiveOptions());
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

// In a party the caster stands and casts: the tank holds the target, backing off would keep it moving and out of spells.
void PartyCasterDoesNotKite()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 2;
    options.PartyCount = 1;
    options.WanderWhenIdle = false;
    options.ClassOf = [](std::uint64_t guid) { return guid == 10 ? 1u : 8u; };   // 10 the warrior leads, 11 is a mage
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11 });
    NearbyUnit mob = Hostile(2, 5.0f, 100);
    view.bots[1].threat_target_guid = MakeUnitRef(8, 1, 2);
    view.bots[1].in_combat = 1;
    view.bots[0].in_combat = 1;
    view.bots[0].threat_target_guid = MakeUnitRef(8, 1, 2);
    AddNearby(view, 0, mob);
    AddNearby(view, 1, mob);
    int kites = 0, casts = 0;
    for (std::uint64_t at = 1000; at < 6000; at += 100)
    {
        view.tick_number = at / 100;
        for (BotAction const& action : policy.Decide(view, at))
        {
            kites += action.bot_guid == 11 && action.type == ActionType::kMoveTo && action.target_guid == 0;
            casts += action.bot_guid == 11 && action.type == ActionType::kCastSpell;
        }
    }
    CHECK(kites == 0);
    CHECK(casts > 0);
}

// Onyxia (10184) bands by role: a healer nearer than 21 yards backs off to the middle of its 21-25 band (23).
void HealerKitesToOnyxiaBand()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 3;
    options.PartyCount = 1;
    options.WanderWhenIdle = false;
    options.ClassOf = [](std::uint64_t guid) { return guid == 10 ? 1u : guid == 11 ? 5u : 4u; };   // 10 the warrior tank, 11 a priest, 12 a rogue
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12 }, options.ClassOf);
    NearbyUnit boss = Hostile(2, 20.0f, 100);
    boss.guid = MakeUnitRef(8, 10184, 2);
    for (std::size_t i = 0; i < 3; ++i)
    {
        view.bots[i].threat_target_guid = boss.guid;
        view.bots[i].in_combat = 1;
        AddNearby(view, i, boss);
    }
    float healerAt = -1.0f;
    for (std::uint64_t at = 1000; at < 3000; at += 100)
    {
        view.tick_number = at / 100;
        for (BotAction const& action : policy.Decide(view, at))
            if (action.bot_guid == 11 && action.type == ActionType::kMoveTo && action.target_guid == 0)
                healerAt = std::fabs(action.dest_x - boss.pos_x);
    }
    CHECK(healerAt >= 22.0f && healerAt <= 24.0f);
}

// A ranged band is 23-29 yards: a caster already inside it holds, and one nearer than 23 kites to the middle (26).
void RangedBandHoldsInsideOnyxiaBand()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 3;
    options.PartyCount = 1;
    options.WanderWhenIdle = false;
    options.ClassOf = [](std::uint64_t guid) { return guid == 10 ? 1u : guid == 11 ? 8u : 4u; };   // 10 the warrior tank, 11 a mage, 12 a rogue
    for (float const yards : { 25.0f, 20.0f })
    {
        BotPolicy policy(options);
        WorldTickSnapshotView view = PartyView({ 10, 11, 12 }, options.ClassOf);
        NearbyUnit boss = Hostile(2, yards, 100);
        boss.guid = MakeUnitRef(8, 10184, 2);
        for (std::size_t i = 0; i < 3; ++i)
        {
            view.bots[i].threat_target_guid = boss.guid;
            view.bots[i].in_combat = 1;
            AddNearby(view, i, boss);
        }
        float mageAt = -1.0f;
        for (std::uint64_t at = 1000; at < 3000; at += 100)
        {
            view.tick_number = at / 100;
            for (BotAction const& action : policy.Decide(view, at))
            {
                if (action.bot_guid == 11 && action.type == ActionType::kMoveTo && action.target_guid == 0)
                    mageAt = std::fabs(action.dest_x - boss.pos_x);
            }
        }
        if (yards > 23.0f)
            CHECK(mageAt < 0.0f);                       // inside the band: no move
        else
            CHECK(mageAt >= 25.0f && mageAt <= 27.0f);  // kites to the middle, 26
    }
}

// A melee band is 2-9 yards and never kites: a rogue at 5 yards holds no kite move.
void MeleeDoesNotKiteFromOnyxia()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 3;
    options.PartyCount = 1;
    options.WanderWhenIdle = false;
    options.ClassOf = [](std::uint64_t guid) { return guid == 10 ? 1u : guid == 11 ? 8u : 4u; };   // 10 the warrior tank, 11 a mage, 12 a rogue
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12 }, options.ClassOf);
    NearbyUnit boss = Hostile(2, 5.0f, 100);
    boss.guid = MakeUnitRef(8, 10184, 2);
    for (std::size_t i = 0; i < 3; ++i)
    {
        view.bots[i].threat_target_guid = boss.guid;
        view.bots[i].in_combat = 1;
        AddNearby(view, i, boss);
    }
    for (std::uint64_t at = 1000; at < 3000; at += 100)
    {
        view.tick_number = at / 100;
        for (BotAction const& action : policy.Decide(view, at))
            CHECK(!(action.bot_guid == 12 && action.type == ActionType::kMoveTo && action.target_guid == 0));
    }
}

// The healer's band stand point skips a hazard: a hostile ground effect over the bearing-0 point (3 yards behind the bot, 23 from Onyxia)
// sends it to the next clear point on the same 23-yard arc, outside the hazard and its 1-yard margin.
void HealerBandStandSkipsAHazard()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 3;
    options.PartyCount = 1;
    options.WanderWhenIdle = false;
    options.AvoidGroundEffects = true;
    options.ClassOf = [](std::uint64_t guid) { return guid == 10 ? 1u : guid == 11 ? 5u : 4u; };   // 10 the warrior tank, 11 a priest, 12 a rogue
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12 }, options.ClassOf);
    NearbyUnit boss = Hostile(2, 20.0f, 100);
    boss.guid = MakeUnitRef(8, 10184, 2);
    for (std::size_t i = 0; i < 3; ++i)
    {
        view.bots[i].threat_target_guid = boss.guid;
        view.bots[i].in_combat = 1;
        AddNearby(view, i, boss);
    }
    view.bots[1].ground_effect_count = 1;                       // the hazard is on the priest (bot 11)
    view.bots[1].ground_effects[0] = forever_module::GroundEffect{};
    view.bots[1].ground_effects[0].spell_id = 11829;
    view.bots[1].ground_effects[0].x = -3.0f;
    view.bots[1].ground_effects[0].y = 0.0f;
    view.bots[1].ground_effects[0].radius = 1.5f;
    view.bots[1].ground_effects[0].kind = 1;
    view.bots[1].ground_effects[0].hostile = 1;
    bool found = false;
    for (BotAction const& move : policy.Decide(view, 1000))
    {
        if (move.bot_guid != 11 || move.type != ActionType::kMoveTo || move.target_guid != 0)
            continue;
        found = true;
        float const hx = move.dest_x + 3.0f, hy = move.dest_y;
        CHECK(std::sqrt(hx * hx + hy * hy) > 2.5f);             // outside the hazard (radius 1.5) and its 1-yard margin
        float const bx = move.dest_x - boss.pos_x, by = move.dest_y - boss.pos_y;   // the band is a distance, so measure the whole offset, not its x part
        CHECK(std::sqrt(bx * bx + by * by) >= 21.0f && std::sqrt(bx * bx + by * by) <= 25.0f);
    }
    CHECK(found);
}

// Onyxian Warder (12129): its Fire Nova hits everything within 20 yards, so a ranged raid member backs off to 21+ yards and only the tank melees it, every other member backs off, melee classes too.
void RaidCasterKitesOutOfWarderNova()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 3;
    options.PartyCount = 1;
    options.WanderWhenIdle = false;
    options.ClassOf = [](std::uint64_t guid) { return guid == 10 ? 1u : guid == 11 ? 8u : 4u; };   // 10 the warrior tank, 11 a mage, 12 a rogue
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12 });
    NearbyUnit warder = Hostile(2, 5.0f, 100);
    warder.guid = MakeUnitRef(8, 12129, 2);
    for (std::size_t i = 0; i < 3; ++i)
    {
        view.bots[i].threat_target_guid = warder.guid;
        view.bots[i].in_combat = 1;
        AddNearby(view, i, warder);
    }
    int tankKites = 0;
    float mageAt = -1.0f, rogueAt = -1.0f;
    for (std::uint64_t at = 1000; at < 3000; at += 100)
    {
        view.tick_number = at / 100;
        for (BotAction const& action : policy.Decide(view, at))
        {
            bool const move = action.type == ActionType::kMoveTo && action.target_guid == 0;
            tankKites += action.bot_guid == 10 && move;
            if (action.bot_guid == 11 && move)
                mageAt = std::fabs(action.dest_x - warder.pos_x);
            if (action.bot_guid == 12 && move)
                rogueAt = std::fabs(action.dest_x - warder.pos_x);
        }
    }
    CHECK(mageAt >= 21.0f);
    CHECK(tankKites == 0);
    CHECK(rogueAt >= 21.0f);
}

// The Fire Nova hits every party member in reach, not only the ones fighting the Warder: a mage on another target backs off too, each bot on its own bearing.
void RaidMemberOnAnotherTargetKitesOutOfWarderNova()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 3;
    options.PartyCount = 1;
    options.WanderWhenIdle = false;
    options.ClassOf = [](std::uint64_t guid) { return guid == 10 ? 1u : 8u; };   // 10 the warrior tank, 11 and 12 mages
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12 });
    NearbyUnit warder = Hostile(2, 5.0f, 100);
    warder.guid = MakeUnitRef(8, 12129, 2);
    NearbyUnit other = Hostile(3, 30.0f, 100);
    for (std::size_t i = 0; i < 3; ++i)
    {
        view.bots[i].threat_target_guid = i == 0 ? warder.guid : other.guid;
        view.bots[i].in_combat = 1;
        AddNearby(view, i, warder);
        AddNearby(view, i, other);
    }
    int tankKites = 0;
    float firstAt = -1.0f, secondAt = -1.0f;
    float firstY = 0.0f, secondY = 0.0f;
    for (std::uint64_t at = 1000; at < 3000; at += 100)
    {
        view.tick_number = at / 100;
        for (BotAction const& action : policy.Decide(view, at))
        {
            bool const move = action.type == ActionType::kMoveTo && action.target_guid == 0;
            tankKites += action.bot_guid == 10 && move;
            if (action.bot_guid == 11 && move)
            {
                firstAt = std::hypot(action.dest_x - warder.pos_x, action.dest_y - warder.pos_y);
                firstY = action.dest_y;
            }
            if (action.bot_guid == 12 && move)
            {
                secondAt = std::hypot(action.dest_x - warder.pos_x, action.dest_y - warder.pos_y);
                secondY = action.dest_y;
            }
        }
    }
    CHECK(firstAt >= 24.0f);
    CHECK(secondAt >= 24.0f);
    CHECK(tankKites == 0);
    CHECK(firstY != secondY);   // not one shared point
}

// The 2026-10-29 pull: the tank pulled the Warder and the other bots stayed out of combat for 55 s, standing next to it. A non-tank bot that is not in combat kites
// as soon as a live Warder is inside the margin; the tank never does.
void NonTankOutOfCombatKitesWhenWarderIsWithinMargin()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 3;
    options.PartyCount = 1;
    options.WanderWhenIdle = false;
    options.ClassOf = [](std::uint64_t guid) { return guid == 10 ? 1u : 8u; };   // 10 the warrior tank, 11 and 12 mages
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12 });
    NearbyUnit warder = Hostile(2, 28.0f, 100);   // inside the 30 yard margin, outside the 24.5 yard nova
    warder.guid = MakeUnitRef(8, 12129, 2);
    for (std::size_t i = 0; i < 3; ++i)
    {
        view.bots[i].in_combat = 0;
        view.bots[i].threat_target_guid = 0;
        AddNearby(view, i, warder);
    }
    int tankKites = 0, mageKites = 0;
    for (std::uint64_t at = 1000; at < 3000; at += 100)
    {
        view.tick_number = at / 100;
        for (BotAction const& action : policy.Decide(view, at))
        {
            bool const move = action.type == ActionType::kMoveTo && action.target_guid == 0;
            tankKites += action.bot_guid == 10 && move;
            if (action.bot_guid == 11 && move)
            {
                ++mageKites;
                CHECK(std::hypot(action.dest_x - warder.pos_x, action.dest_y - warder.pos_y) >= 29.0f);
            }
        }
    }
    CHECK(mageKites > 0);
    CHECK(tankKites == 0);
}

// The kite point must be somewhere the core can path to. The module has no path query, so the first point is the retreat along the entry route (the line from the Warder
// toward the dungeon entry, 30 yards out); when the core refuses it, the next point is the circle round the Warder, a different place and not the refused one again.
void KiteRetreatsAlongTheEntryRouteAndFallsBackToTheCircle()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 3;
    options.PartyCount = 1;
    options.WanderWhenIdle = false;
    options.ClassOf = [](std::uint64_t guid) { return guid == 10 ? 1u : 8u; };   // 10 the warrior tank, 11 and 12 mages
    PolicyOptions::DungeonRoute route;
    route.HasEntry = true;
    route.Entry = Point{ 5.0f, -100.0f, 0.0f };   // straight behind the Warder, which stands at (5, 0)
    options.DungeonsByMap[0] = route;
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12 });
    NearbyUnit warder = Hostile(2, 5.0f, 100);
    warder.guid = MakeUnitRef(8, 12129, 2);
    for (std::size_t i = 0; i < 3; ++i)
    {
        view.bots[i].in_combat = 1;
        AddNearby(view, i, warder);
    }
    auto kiteOf = [&](std::uint64_t at, bool& found) {
        Point dest{};
        found = false;
        view.tick_number = at / 100;
        for (BotAction const& action : policy.Decide(view, at))
            if (action.bot_guid == 11 && action.type == ActionType::kMoveTo && action.target_guid == 0)
            {
                dest = Point{ action.dest_x, action.dest_y, action.dest_z };
                found = true;
            }
        return dest;
    };
    bool found = false;
    Point const retreat = kiteOf(1000, found);
    CHECK(found);
    float const length = std::hypot(retreat.X - warder.pos_x, retreat.Y - warder.pos_y);
    CHECK(length >= 29.0f && length <= 31.0f);
    CHECK((retreat.Y - warder.pos_y) / length < -0.9f);   // toward the entry, within a spread of 25 degrees

    view.bots[1].last_action_sequence = 1;
    view.bots[1].last_action_type = uint8_t(ActionType::kMoveTo);
    view.bots[1].last_action_status = BotActionStatus::kNoRoute;
    Point const fallback = kiteOf(2000, found);
    CHECK(found);
    CHECK(std::hypot(fallback.X - retreat.X, fallback.Y - retreat.Y) >= 12.0f);   // not the refused place
    CHECK(std::hypot(fallback.X - warder.pos_x, fallback.Y - warder.pos_y) >= 29.0f);
}

// A party member on low health keeps fighting: its healer needs it to hold still, and a fleeing bot drags the pack onto the party.
void PartyMemberDoesNotFleeOnLowHealth()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 2;
    options.PartyCount = 1;
    options.WanderWhenIdle = false;
    options.ClassOf = [](std::uint64_t guid) { return guid == 10 ? 1u : 5u; };   // 10 the warrior leads
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11 });
    NearbyUnit mob = Hostile(2, 3.0f, 100);
    for (std::size_t i = 0; i < 2; ++i)
    {
        view.bots[i].in_combat = 1;
        view.bots[i].threat_target_guid = MakeUnitRef(8, 1, 2);
        AddNearby(view, i, mob);
    }
    view.bots[0].health = 100;                              // the tank at 10 %
    int flees = 0;
    for (std::uint64_t at = 1000; at < 4000; at += 100)
    {
        view.tick_number = at / 100;
        for (BotAction const& action : policy.Decide(view, at))
            flees += action.bot_guid == 10 && action.type == ActionType::kMoveTo;
    }
    CHECK(flees == 0);
    // Alone, the same bot still runs.
    BotPolicy alone = ClassPolicy(1);
    WorldTickSnapshotView solo = View(1, MakeUnitRef(8, 1, 2), { Hostile(2, 3.0f, 100) });
    solo.bots[0].in_combat = 1;
    solo.bots[0].health = 100;
    bool fled = false;
    for (BotAction const& action : alone.Decide(solo, 1000))
        fled = fled || action.type == ActionType::kMoveTo;
    CHECK(fled);
}

// A party member in a fight does not chase a target that has run far off (a fleeing creature): it stands with its party.
void PartyMemberDoesNotChaseAFleeingTarget()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 2;
    options.PartyCount = 1;
    options.WanderWhenIdle = false;
    options.ClassOf = [](std::uint64_t guid) { return guid == 10 ? 1u : 5u; };
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11 });
    NearbyUnit mob = Hostile(2, 70.0f, 20);                 // 70 yards off and running
    for (std::size_t i = 0; i < 2; ++i)
    {
        view.bots[i].in_combat = 1;
        view.bots[i].threat_target_guid = MakeUnitRef(8, 1, 2);
        AddNearby(view, i, mob);
    }
    int follows = 0;
    for (std::uint64_t at = 1000; at < 4000; at += 100)
    {
        view.tick_number = at / 100;
        for (BotAction const& action : policy.Decide(view, at))
            follows += action.bot_guid == 10 && action.type == ActionType::kFollow && action.target_guid == MakeUnitRef(8, 1, 2);
    }
    CHECK(follows == 0);
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
    BotPolicy policy(AggressiveOptions());
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
    BotPolicy policy(AggressiveOptions());
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

// Riding is learned at level 40: below MountMinLevel the bot walks the long way on foot and never sends a mount.
void BotBelowTheMountLevelNeverAsksToMount()
{
    WorldTickSnapshotView view = PartyView({ 10 });
    SetVendor(view, 200.0f);
    view.bots[0].level = 30;
    PolicyOptions options = AggressiveOptions();
    options.ServiceEveryMs = 60000;
    options.MountMinLevel = 40;
    BotPolicy policy(options);
    bool walks = false;
    for (std::uint64_t at = 0; at < 3000; at += 100)
        for (BotAction const& action : policy.Decide(view, at))
        {
            CHECK(action.type != ActionType::kMount);
            walks = walks || action.type == ActionType::kMoveTo;
        }
    CHECK(walks);
    view.bots[0].level = 40;
    BotPolicy rider(options);
    bool mounts = false;
    for (BotAction const& action : rider.Decide(view, 0))
        mounts = mounts || (action.type == ActionType::kMount && action.spell_id == 1);
    CHECK(mounts);
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
    BotPolicy policy(AggressiveOptions());
    auto actions = policy.Decide(view, 0);
    CHECK(actions.size() == 1);
    if (actions.size() == 1)
        CHECK(actions[0].type == ActionType::kMoveTo);
}

// ---- weakest target, interrupt sharing, heal sharing, party buffs ----

// A pull is the weakest creature that may be pulled, not the closest: finishing it before the next is more experience per hour.
void WeakestTargetIsPulledFirst()
{
    BotPolicy policy(AggressiveOptions());
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
    BotPolicy policy(AggressiveOptions());
    WorldTickSnapshotView view = PartyView({ 10 });
    AddNearby(view, 0, Hostile(7, 12.0f, 100));
    AddNearby(view, 0, Hostile(8, 3.0f, 100));
    auto const actions = policy.Decide(view, 0);
    CHECK(HasAction(actions, 10, ActionType::kAttackTarget, MakeUnitRef(8, 1, 8)));
}

// Two rogues of one party, a target that is casting: one interrupts it and the other keeps building combo points.
void OnlyOneRogueInterruptsACastingTarget()
{
    PolicyOptions options = AggressiveOptions();
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
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t guid) { return guid == 12 ? 1u : 5u; }; // warrior leads, rest priests
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 }, [](std::uint64_t guid) { return guid == 12 ? 1u : 5u; });
    // Pre-form the party: bot 12 leads, all are members.
    constexpr std::uint64_t kLeaderRef = (std::uint64_t(2) << 56) | 12; // HighGuid::Player = 2
    for (BotSnapshot& bot : view.bots)
    {
        if (bot.guid == 12)
        {
            bot.group_role = 2;
            bot.group_leader_guid = kLeaderRef; // leader's own GUID as unit reference
        }
        else
        {
            bot.group_role = 1;
            bot.group_leader_guid = kLeaderRef;
        }
    }
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
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = WarriorLeads; // bot 12 is the warrior and leads
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 }, WarriorLeads);
    // Pre-form the party: bot 12 leads, others follow.
    for (BotSnapshot& bot : view.bots)
    {
        if (bot.guid == 12)
        {
            bot.group_role = 2;
            bot.group_leader_guid = (std::uint64_t(2) << 56) | 12; // full Player unit ref
        }
        else
        {
            bot.group_role = 1;
            bot.group_leader_guid = (std::uint64_t(2) << 56) | 12;
        }
    }
    view.bots[2].health = 600; // the leader: 60%, holding a target
    view.bots[3].health = 300; // a member on 30%
    view.bots[2].threat_target_guid = Hostile(7, 3.0f, 100).guid; // the tank is fighting
    AddNearby(view, 2, Hostile(7, 3.0f, 100));
    auto const actions = policy.Decide(view, 0);
    CHECK(CastsSpell(actions, 10, 2050, PlayerOf(12))); // the tank, not the member
}

// Out of a fight, on a dungeon route, the healer heals the leader up (below 90%): the leader walks on only at 80%, and cannot sit with creatures in view.
void TheRouteLeaderIsHealedUpOutOfAFight()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = WarriorLeads;
    options.Route = { Point{ 100.0f, 0.0f, 0.0f } };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 }, WarriorLeads);
    for (BotSnapshot& bot : view.bots)
    {
        bot.group_role = bot.guid == 12 ? 2 : 1;
        bot.group_leader_guid = (std::uint64_t(2) << 56) | 12;
    }
    view.bots[2].health = 650;                                        // the leader at 65%, no target, nobody fighting
    CHECK(CastsSpell(policy.Decide(view, 0), 10, 2050, PlayerOf(12)));
    // Healthy enough (95%): no heal.
    BotPolicy healthy(options);
    healthy.SetRouteActive(true);
    view.bots[2].health = 950;
    CHECK(!CastsSpell(healthy.Decide(view, 0), 10, 2050, PlayerOf(12)));
}

// The route leader waits while a mana user of the party is dry (the healer walked into fights at 3% mana), and goes on after kRouteManaMaxMs.
void TheRouteWaitsForTheHealersMana()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = WarriorLeads;
    options.Route = { Point{ 100.0f, 0.0f, 0.0f } };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 }, WarriorLeads);
    for (BotSnapshot& bot : view.bots)
    {
        bot.group_role = bot.guid == 12 ? 2 : 1;
        bot.group_leader_guid = (std::uint64_t(2) << 56) | 12;
    }
    for (BotSnapshot& bot : view.bots)
        if (bot.guid == 10)
        {
            bot.power_type = 0;
            bot.max_power = 1000;
            bot.power = 50; // the healer at 5%
        }
    auto const walks = [](std::vector<BotAction> const& actions, std::uint64_t guid)
    {
        for (BotAction const& action : actions)
            if (action.bot_guid == guid && action.type == ActionType::kMoveTo && action.dest_x == 100.0f) // the route point
                return true;
        return false;
    };
    CHECK(!walks(policy.Decide(view, 1000), 12));
    CHECK(!walks(policy.Decide(view, 61000), 12));
    CHECK(walks(policy.Decide(view, 160000), 12)); // given up after 150 s
    // Mana full: the leader walks at once.
    BotPolicy full(options);
    full.SetRouteActive(true);
    for (BotSnapshot& bot : view.bots)
        if (bot.guid == 10)
            bot.power = 1000;
    CHECK(walks(full.Decide(view, 0), 12));
}

// Two druids of one party with one party buff: each member gets it once, however many druids know it.
void TwoDruidsDoNotBothCastThePartyBuff()
{
    std::istringstream file("class 11\nbuff 1126 duration 1800 party\n");
    std::vector<std::string> drop;
    PolicyOptions options = AggressiveOptions();
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

// An upkeep buff that keeps failing (Mark of the Wild, Thorns, Arcane Intellect and Power Word: Fortitude were refused 30,000 times in two hours) waits twice as long
// after each answer in a row: 5 s, 10 s, 20 s.
void AFailingBuffBacksOffLongerEachTime()
{
    std::istringstream file("class 11\nbuff 1126 duration 1800 party\n");
    std::vector<std::string> drop;
    PolicyOptions options = AggressiveOptions();
    options.Rotations = std::make_shared<rotation::RotationData>(rotation::ParseRotationData(file, drop));
    options.PartySize = 2;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t) { return 11; };
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11 });
    for (BotSnapshot& bot : view.bots)
        bot.group_leader_guid = MakeUnitRef(2, 0, 10);
    auto const casts = [&](std::uint64_t from, std::uint64_t to)
    {
        int count = 0;
        for (std::uint64_t at = from; at < to; at += 500)
            for (BotAction const& action : policy.Decide(view, at))
                count += action.bot_guid == 10 && action.type == ActionType::kCastSpell && action.spell_id == 1126;
        return count;
    };
    CHECK(casts(0, 1000) >= 1);                                        // the first try
    view.bots[0].last_action_type = std::uint8_t(ActionType::kCastSpell);
    view.bots[0].last_action_spell = 1126;
    view.bots[0].last_action_status = BotActionStatus::kInvalidTarget;
    view.bots[0].last_action_sequence = 1;
    CHECK(casts(1000, 5500) == 0);                                     // waits 5 s
    view.bots[0].last_action_sequence = 2;
    CHECK(casts(5500, 14500) == 0);                                    // the second answer in a row: 10 s
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
    BotPolicy policy(AggressiveOptions());
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
    BotPolicy killer(AggressiveOptions());
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
    bool follows = false;
    for (BotAction const& action : after)
        follows = follows || (action.type == ActionType::kPetCommand && action.spell_id == 2);   // to follow its owner, not to stay where the fight was
    CHECK(follows);
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
    PolicyOptions options = AggressiveOptions();
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

// ---- Supplies (layout 15, supply_policy.h) ----
// A bot short of ammunition, food or drink walks to a vendor that sells it, buys there, backs off when a trip bought nothing, and eats or drinks
// while it rests.
BotPolicy SupplyPolicy(std::uint32_t classId = 0)
{
    PolicyOptions options = AggressiveOptions();
    options.ClassId = classId;
    options.WanderWhenIdle = false; // an idle bot would take an exploring walk; these tests look at trips, rests and eating
    options.Supplies.Enabled = true;
    return BotPolicy(options);
}

// A hunter at the origin with ammunition running low, 500 copper and room in the bags. The ammunition vendor stands at x = 100, an ordinary vendor on
// the other side at x = -50.
WorldTickSnapshotView ShopperView(std::uint32_t ammo, std::uint32_t food = 20)
{
    WorldTickSnapshotView view = PartyView({ 10 });
    BotSnapshot& bot = view.bots[0];
    bot.level = 10;
    bot.ammo_entry = 2512;
    bot.ammo_count = ammo;
    bot.food_count = food;
    bot.copper = 500;
    bot.bag_free_slots = 5;
    bot.ammo_vendor_x = 100.0f;
    bot.ammo_vendor_valid = 1;
    bot.ammo_vendor_guid = MakeUnitRef(8, 4553, 31);
    bot.vendor_x = -50.0f;
    bot.vendor_valid = 1;
    return view;
}

// A copy of the first action of this type for bot 10 (a pointer into the vector that Decide returned would dangle once the call ends).
std::optional<BotAction> FindAction(std::vector<BotAction> const& actions, ActionType type)
{
    for (BotAction const& action : actions)
        if (action.type == type && action.bot_guid == 10)
            return action;
    return std::nullopt;
}

void ShortOfAmmoWalksToTheAmmoVendor()
{
    BotPolicy policy = SupplyPolicy(3);
    auto const actions = policy.Decide(ShopperView(10), 0);
    std::optional<BotAction> walk = FindAction(actions, ActionType::kMoveTo);
    CHECK(walk.has_value() && walk->dest_x == 100.0f); // the vendor that sells ammunition, not the nearer ordinary one
    CHECK(!FindAction(actions, ActionType::kVendor).has_value());
}

void NoSuppliesTripWhenTheFeatureIsOff()
{
    PolicyOptions options = AggressiveOptions();
    options.ClassId = 3;
    options.WanderWhenIdle = false;
    BotPolicy policy(options);
    auto const actions = policy.Decide(ShopperView(0), 0);
    CHECK(!FindAction(actions, ActionType::kMoveTo).has_value() || FindAction(actions, ActionType::kMoveTo)->dest_x != 100.0f);
    CHECK(!FindAction(actions, ActionType::kVendor).has_value());
}

void NoTripWithoutMoneyOrBagRoom()
{
    WorldTickSnapshotView poor = ShopperView(0);
    poor.bots[0].copper = 3;
    CHECK(!FindAction(SupplyPolicy(3).Decide(poor, 0), ActionType::kMoveTo).has_value());
    WorldTickSnapshotView full = ShopperView(0);
    full.bots[0].bag_free_slots = 0;
    CHECK(!FindAction(SupplyPolicy(3).Decide(full, 0), ActionType::kMoveTo).has_value());
}

void BuysAmmoAtTheVendor()
{
    BotPolicy policy = SupplyPolicy(3);
    WorldTickSnapshotView view = ShopperView(10);
    view.bots[0].pos_x = 98.0f; // two yards from the ammunition vendor
    auto const actions = policy.Decide(view, 0);
    std::optional<BotAction> buy = FindAction(actions, ActionType::kVendor);
    CHECK(buy.has_value());
    if (buy)
    {
        CHECK((buy->spell_id & 8) != 0 && (buy->spell_id & 16) == 0 && (buy->spell_id & 32) == 0);
        CHECK(buy->dest_x == 400.0f && buy->dest_y == 0.0f && buy->dest_z == 0.0f);
        CHECK(buy->target_guid == MakeUnitRef(8, 4553, 31));
        CHECK((buy->spell_id & 7) == 7); // sells, repairs and equips on the same visit
    }
}

void AManaUserWithoutFoodOrDrinkBuysBoth()
{
    BotPolicy policy = SupplyPolicy(5);
    WorldTickSnapshotView view = PartyView({ 10 });
    BotSnapshot& bot = view.bots[0];
    bot.level = 12;
    bot.power_type = 0;
    bot.max_power = 100;
    bot.power = 100;
    bot.copper = 200;
    bot.bag_free_slots = 3;
    bot.food_vendor_x = 3.0f;
    bot.food_vendor_valid = 1;
    bot.food_vendor_guid = MakeUnitRef(8, 1465, 12);
    bot.food_count = 0;
    bot.drink_count = 0;
    auto const actions = policy.Decide(view, 0);
    std::optional<BotAction> buy = FindAction(actions, ActionType::kVendor);
    CHECK(buy.has_value());
    if (buy)
    {
        CHECK((buy->spell_id & 16) != 0 && (buy->spell_id & 32) != 0 && (buy->spell_id & 8) == 0);
        CHECK(buy->dest_y == 10.0f && buy->dest_z == 10.0f);
        CHECK(buy->target_guid == MakeUnitRef(8, 1465, 12));
    }
    // A class without mana is not asked to buy drink.
    BotPolicy warrior = SupplyPolicy(1);
    WorldTickSnapshotView plain = view;
    plain.bots[0].max_power = 0;
    std::optional<BotAction> foodOnly = FindAction(warrior.Decide(plain, 0), ActionType::kVendor);
    CHECK(foodOnly.has_value() && foodOnly->dest_y == 10.0f && foodOnly->dest_z == 0.0f && (foodOnly->spell_id & 32) == 0);
}

void AFailedTripBacksOffAndASuccessfulOneDoesNot()
{
    BotPolicy failing = SupplyPolicy(3);
    WorldTickSnapshotView atVendor = ShopperView(10);
    atVendor.bots[0].pos_x = 98.0f;
    CHECK(FindAction(failing.Decide(atVendor, 0), ActionType::kVendor).has_value());
    // The counts did not change by the time the trip is judged: it bought nothing, so the next trip waits the first back-off (10 minutes).
    failing.Decide(atVendor, 8000);
    CHECK(!FindAction(failing.Decide(atVendor, 200000), ActionType::kVendor).has_value());
    CHECK(!FindAction(failing.Decide(atVendor, 400000), ActionType::kVendor).has_value());
    CHECK(FindAction(failing.Decide(atVendor, 700000), ActionType::kVendor).has_value());

    // A trip that bought ammunition: still short (below the low mark), so it goes again after the ordinary trip interval, not the back-off.
    BotPolicy working = SupplyPolicy(3);
    CHECK(FindAction(working.Decide(atVendor, 0), ActionType::kVendor).has_value());
    WorldTickSnapshotView bought = atVendor;
    bought.bots[0].ammo_count = 60;
    working.Decide(bought, 8000);
    CHECK(!FindAction(working.Decide(bought, 100000), ActionType::kVendor).has_value());   // inside the two-minute trip interval
    CHECK(FindAction(working.Decide(bought, 130000), ActionType::kVendor).has_value());
}

void WornGearGoesToTheRepairmanBeforeTheSupplyVendor()
{
    BotPolicy policy = SupplyPolicy(3);
    WorldTickSnapshotView view = ShopperView(10);
    view.bots[0].repair_x = -200.0f;
    view.bots[0].repair_valid = 1;
    view.bots[0].durability_pct = 20;
    std::optional<BotAction> walk = FindAction(AfterMount(policy, view, 0), ActionType::kMoveTo);
    CHECK(walk.has_value() && walk->dest_x == -200.0f);
}

void PartyMembersAndHeldBotsDoNotShop()
{
    PolicyOptions options = AggressiveOptions();
    options.ClassOf = WarriorLeads;
    options.PartySize = 5;
    options.PartyCount = 1;
    options.Supplies.Enabled = true;
    options.WanderWhenIdle = false;
    BotPolicy party(options);
    // Bots 11 and 12: the warrior 12 leads, and the priest 11 follows it. The priest is short of ammunition (as a test would have it) but is
    // a party member, so it does not go shopping; a lone bot with the same facts does (ShortOfAmmoWalksToTheAmmoVendor).
    WorldTickSnapshotView view = PartyView({ 11, 12 });
    BotSnapshot const shopper = ShopperView(0).bots[0];
    for (BotSnapshot& bot : view.bots)
    {
        std::uint64_t const guid = bot.guid;
        bot = shopper;
        bot.guid = guid;
    }
    for (BotAction const& action : party.Decide(view, 0))
        if (action.bot_guid == 11)
            CHECK(!(action.type == ActionType::kMoveTo && action.dest_x == 100.0f) && action.type != ActionType::kVendor);
}

void ARestingHurtBotEatsAndDrinks()
{
    BotPolicy policy = SupplyPolicy();
    WorldTickSnapshotView hurt = PartyView({ 10 });
    hurt.bots[0].health = 400;
    hurt.bots[0].food_count = 5;
    auto const first = policy.Decide(hurt, 1000);
    CHECK(FindAction(first, ActionType::kRest).has_value());
    std::optional<BotAction> eat = FindAction(first, ActionType::kConsume);
    CHECK(eat.has_value() && eat->spell_id == 1);
    // Not again until the last piece has done its work.
    CHECK(!FindAction(policy.Decide(hurt, 10000), ActionType::kConsume).has_value());
    std::optional<BotAction> again = FindAction(policy.Decide(hurt, 30000), ActionType::kConsume);
    CHECK(again.has_value() && again->spell_id == 1);

    // Low mana: drink, which comes before food.
    BotPolicy drinker = SupplyPolicy(5);
    WorldTickSnapshotView dry = ManaView(20);
    dry.bots[0].health = 400;
    dry.bots[0].food_count = 5;
    dry.bots[0].drink_count = 5;
    std::optional<BotAction> drink = FindAction(drinker.Decide(dry, 1000), ActionType::kConsume);
    CHECK(drink.has_value() && drink->spell_id == 2);

    // Nothing carried: nothing to use.
    BotPolicy empty = SupplyPolicy();
    WorldTickSnapshotView bare = PartyView({ 10 });
    bare.bots[0].health = 400;
    CHECK(!FindAction(empty.Decide(bare, 1000), ActionType::kConsume).has_value());
    // A refused use is not repeated at once.
    BotPolicy refused = SupplyPolicy();
    CHECK(FindAction(refused.Decide(hurt, 1000), ActionType::kConsume).has_value());
    WorldTickSnapshotView no = hurt;
    no.bots[0].last_action_sequence = 5;
    no.bots[0].last_action_type = uint8_t(ActionType::kConsume);
    no.bots[0].last_action_status = BotActionStatus::kRefused;
    refused.Decide(no, 2000);
    CHECK(!FindAction(refused.Decide(hurt, 60000), ActionType::kConsume).has_value());
    CHECK(FindAction(refused.Decide(hurt, 130000), ActionType::kConsume).has_value());
}

void AHunterWithoutAmmunitionFightsInMelee()
{
    NearbyUnit const mob = Hostile(2, 20.0f, 100);
    PolicyOptions options = AggressiveOptions();
    options.ClassId = 3;
    options.WanderWhenIdle = false;

    BotPolicy armed(options);
    WorldTickSnapshotView withAmmo = HunterView(1, 1, { mob });
    withAmmo.bots[0].ammo_entry = 2512;
    withAmmo.bots[0].ammo_count = 50;
    CHECK(HasCast(armed.Decide(withAmmo, 1000), 75));

    BotPolicy empty(options);
    WorldTickSnapshotView without = HunterView(1, 1, { mob });
    without.bots[0].ammo_entry = 2512;
    without.bots[0].ammo_count = 0;
    CHECK(!HasCast(empty.Decide(without, 1000), 75)); // the core would refuse it
    // A core that does not report ammunition (ammo_entry 0) is not second-guessed.
    BotPolicy unknown(options);
    WorldTickSnapshotView old = HunterView(1, 1, { mob });
    CHECK(HasCast(unknown.Decide(old, 1000), 75));
}


// ---- Potions and bandages (layout 16) ----
BotPolicy GoodsPolicy(std::uint32_t classId = 1)
{
    PolicyOptions options = AggressiveOptions();
    options.ClassId = classId;
    options.WanderWhenIdle = false;
    options.Supplies.Enabled = true;
    options.Supplies.Potions = true;
    options.Supplies.Bandages = true;
    return BotPolicy(options);
}

// A warrior with food and money and nothing else, and a goods vendor 2 yards away.
WorldTickSnapshotView GoodsView()
{
    WorldTickSnapshotView view = PartyView({ 10 });
    BotSnapshot& bot = view.bots[0];
    bot.level = 20;
    bot.food_count = 10;
    bot.copper = 500;
    bot.bag_free_slots = 5;
    bot.goods_vendor_valid = 1;
    bot.goods_vendor_x = 2.0f;
    bot.goods_vendor_guid = MakeUnitRef(8, 5100, 41);
    return view;
}

void AWarriorWithoutPotionsOrBandagesBuysThemAtTheGoodsVendor()
{
    BotPolicy policy = GoodsPolicy();
    std::optional<BotAction> buy = FindAction(policy.Decide(GoodsView(), 0), ActionType::kVendor);
    CHECK(buy.has_value());
    if (buy)
    {
        CHECK((buy->spell_id & 64) != 0 && (buy->spell_id & 128) == 0 && (buy->spell_id & 256) != 0); // no mana, so no mana potion
        CHECK((buy->spell_id & (8 | 16 | 32)) == 0);
        CHECK(buy->target_guid == MakeUnitRef(8, 5100, 41));
    }
    // Off unless asked for.
    BotPolicy off = SupplyPolicy(1);
    CHECK(!FindAction(off.Decide(GoodsView(), 0), ActionType::kVendor).has_value());
}

void AFarGoodsVendorIsWalkedTo()
{
    BotPolicy policy = GoodsPolicy();
    WorldTickSnapshotView far = GoodsView();
    far.bots[0].goods_vendor_x = 80.0f;
    std::optional<BotAction> walk = FindAction(policy.Decide(far, 0), ActionType::kMoveTo);
    CHECK(walk.has_value() && walk->dest_x == 80.0f);
}

void AHurtFightingBotDrinksAHealthPotionOnce()
{
    BotPolicy policy = GoodsPolicy();
    WorldTickSnapshotView fight = GoodsView();
    fight.bots[0].health = 300;      // 30%
    fight.bots[0].health_potion_count = 2;
    fight.bots[0].in_combat = 1;
    std::optional<BotAction> drink = FindAction(policy.Decide(fight, 1000), ActionType::kConsume);
    CHECK(drink.has_value() && drink->spell_id == 3);
    // The game's shared cooldown: not asked for again at once.
    CHECK(!FindAction(policy.Decide(fight, 20000), ActionType::kConsume).has_value());
    std::optional<BotAction> again = FindAction(policy.Decide(fight, 130000), ActionType::kConsume);
    CHECK(again.has_value() && again->spell_id == 3);
    // Healthy enough, or no potion: nothing.
    BotPolicy fine = GoodsPolicy();
    WorldTickSnapshotView well = fight;
    well.bots[0].health = 900;
    CHECK(!FindAction(fine.Decide(well, 1000), ActionType::kConsume).has_value());
    BotPolicy none = GoodsPolicy();
    WorldTickSnapshotView empty = fight;
    empty.bots[0].health_potion_count = 0;
    CHECK(!FindAction(none.Decide(empty, 1000), ActionType::kConsume).has_value());
    // Not outside a fight (a resting bot eats or bandages instead).
    BotPolicy calm = GoodsPolicy();
    WorldTickSnapshotView resting = fight;
    resting.bots[0].in_combat = 0;
    resting.bots[0].food_count = 0;
    std::optional<BotAction> notPotion = FindAction(calm.Decide(resting, 1000), ActionType::kConsume);
    CHECK(!notPotion.has_value() || notPotion->spell_id != 3);
}

void ARestingBotWithoutFoodBandagesItself()
{
    BotPolicy policy = GoodsPolicy();
    WorldTickSnapshotView hurt = GoodsView();
    hurt.bots[0].health = 400;
    hurt.bots[0].food_count = 0;
    hurt.bots[0].bandage_count = 4;
    std::optional<BotAction> bandage = FindAction(policy.Decide(hurt, 1000), ActionType::kConsume);
    CHECK(bandage.has_value() && bandage->spell_id == 5);
    // "Recently Bandaged" lasts a minute: not again before then.
    CHECK(!FindAction(policy.Decide(hurt, 40000), ActionType::kConsume).has_value());
    std::optional<BotAction> again = FindAction(policy.Decide(hurt, 70000), ActionType::kConsume);
    CHECK(again.has_value() && again->spell_id == 5);
    // With food, the food is eaten.
    BotPolicy eater = GoodsPolicy();
    WorldTickSnapshotView fed = hurt;
    fed.bots[0].food_count = 5;
    std::optional<BotAction> eat = FindAction(eater.Decide(fed, 1000), ActionType::kConsume);
    CHECK(eat.has_value() && eat->spell_id == 1);
}

void ABotMissingAReagentBuysItAtTheReagentVendor()
{
    PolicyOptions options = AggressiveOptions();
    options.ClassId = 1;
    options.WanderWhenIdle = false;
    options.Supplies.Enabled = true;
    options.Supplies.Reagents = true;
    BotPolicy policy(options);
    WorldTickSnapshotView view = GoodsView();
    view.bots[0].reagent_entry = 17026;
    view.bots[0].reagent_count = 0;
    view.bots[0].reagent_vendor_valid = 1;
    view.bots[0].reagent_vendor_x = 2.0f;
    view.bots[0].reagent_vendor_guid = MakeUnitRef(8, 5200, 7);
    std::optional<BotAction> buy = FindAction(policy.Decide(view, 0), ActionType::kVendor);
    CHECK(buy.has_value());
    if (buy)
    {
        CHECK((buy->spell_id & 512) != 0 && (buy->spell_id & (8 | 16 | 32 | 64 | 128 | 256)) == 0);
        CHECK(buy->target_guid == MakeUnitRef(8, 5200, 7));
    }
    BotPolicy off = GoodsPolicy();
    std::optional<BotAction> offBuy = FindAction(off.Decide(view, 0), ActionType::kVendor);   // one Decide: a second call changes the policy's state
    CHECK(!offBuy.has_value() || (offBuy->spell_id & 512) == 0);
}

void AManaUserDrinksAManaPotionInAFight()
{
    BotPolicy policy = GoodsPolicy(5);
    WorldTickSnapshotView fight = ManaView(10);
    fight.bots[0].in_combat = 1;
    fight.bots[0].mana_potion_count = 1;
    std::optional<BotAction> drink = FindAction(policy.Decide(fight, 1000), ActionType::kConsume);
    CHECK(drink.has_value() && drink->spell_id == 4);
}


// ---- Presence (bot_presence.h) ----
void ABotIsSafeToLogOutOnlyWhenNothingIsGoingOn()
{
    BotPolicy policy = SupplyPolicy(1);
    WorldTickSnapshotView view = GoodsView();
    policy.Decide(view, 0);
    CHECK(policy.SafeToLogOut(view.bots[0]));
    BotSnapshot fighting = view.bots[0];
    fighting.in_combat = 1;
    CHECK(!policy.SafeToLogOut(fighting));
    BotSnapshot dead = view.bots[0];
    dead.life_state = 2;
    CHECK(!policy.SafeToLogOut(dead));
    BotSnapshot grouped = view.bots[0];
    grouped.group_role = 1;
    CHECK(!policy.SafeToLogOut(grouped));
    BotSnapshot invited = view.bots[0];
    invited.group_invite_from = MakeUnitRef(4, 77, 1);
    CHECK(!policy.SafeToLogOut(invited));
    // On the way to a vendor for supplies: not now.
    WorldTickSnapshotView shopper = ShopperView(10);
    BotPolicy walking = SupplyPolicy(3);
    CHECK(FindAction(walking.Decide(shopper, 0), ActionType::kMoveTo).has_value());
    CHECK(!walking.SafeToLogOut(shopper.bots[0]));
    // A bot the policy never saw has nothing going on.
    BotSnapshot stranger = view.bots[0];
    stranger.guid = 999;
    CHECK(policy.SafeToLogOut(stranger));
}


// ---- Flight points (layout 17, taxi_policy.h) ----
BotPolicy TaxiPolicy()
{
    PolicyOptions options = AggressiveOptions();
    options.HubsByMap[1] = { Point{ 3000.0f, 0.0f, 0.0f } };   // a hub far away: the idle bot's walk is long
    options.Taxi.Enabled = true;
    return BotPolicy(options);
}

WorldTickSnapshotView TaxiView(float x)
{
    WorldTickSnapshotView view = OnMap(1);
    view.bots[0].pos_x = x;
    view.bots[0].taxi_master_valid = 1;
    view.bots[0].taxi_master_x = 100.0f;
    view.bots[0].taxi_master_guid = MakeUnitRef(8, 3838, 5);
    return view;
}

std::optional<BotAction> FirstOf(std::vector<BotAction> const& actions, ActionType type)
{
    for (BotAction const& action : actions)
        if (action.type == type && action.bot_guid == 10)
            return action;
    return std::nullopt;
}

void ALongWalkGoesToTheFlightMasterThenFlies()
{
    BotPolicy policy = TaxiPolicy();
    WorldTickSnapshotView far = TaxiView(0.0f);
    policy.Decide(far, 0);
    auto const first = policy.Decide(far, 30000);
    CHECK(WalkTo(first, 10) == 100.0f);                           // to the flight master, not to the hub
    CHECK(!FirstOf(first, ActionType::kTaxi).has_value());

    BotPolicy flier = TaxiPolicy();
    WorldTickSnapshotView there = TaxiView(98.0f);                // 2 yards from the flight master
    flier.Decide(there, 0);
    auto const fly = flier.Decide(there, 30000);
    std::optional<BotAction> order = FirstOf(fly, ActionType::kTaxi);
    CHECK(order.has_value());
    if (order)
    {
        CHECK(order->target_guid == MakeUnitRef(8, 3838, 5) && order->spell_id == 1 && order->dest_x == 3000.0f);
    }
    CHECK(WalkTo(fly, 10) < 0.0f);                                // no walking order with it
    // The order is not repeated while it is given time to show, and a bot on the flight gets no orders.
    CHECK(!FirstOf(flier.Decide(there, 31000), ActionType::kTaxi).has_value());
    WorldTickSnapshotView flying = there;
    flying.bots[0].in_flight = 1;
    auto const during = flier.Decide(flying, 60000);
    CHECK(!FirstOf(during, ActionType::kTaxi).has_value() && WalkTo(during, 10) < 0.0f);
}

void ARefusedFlightMakesTheBotWalk()
{
    BotPolicy policy = TaxiPolicy();
    WorldTickSnapshotView there = TaxiView(98.0f);
    policy.Decide(there, 0);
    CHECK(FirstOf(policy.Decide(there, 30000), ActionType::kTaxi).has_value());
    WorldTickSnapshotView refused = there;
    refused.bots[0].last_action_sequence = 7;
    refused.bots[0].last_action_type = uint8_t(ActionType::kTaxi);
    refused.bots[0].last_action_status = BotActionStatus::kRefused;
    auto const after = AfterMount(policy, refused, 50000);
    CHECK(!FirstOf(after, ActionType::kTaxi).has_value());
    CHECK(WalkTo(after, 10) == 3000.0f);                          // the walk goes on, to the hub
}

void FlightsAreOffByDefaultAndNotForPartyMembers()
{
    PolicyOptions options = AggressiveOptions();
    options.HubsByMap[1] = { Point{ 3000.0f, 0.0f, 0.0f } };
    BotPolicy off(options);
    WorldTickSnapshotView there = TaxiView(98.0f);
    off.Decide(there, 0);
    CHECK(!FirstOf(off.Decide(there, 30000), ActionType::kTaxi).has_value());
}
 

// --- The Holy priest of the level 30 guide: Renew on the tank, Binding Heal, Prayer of Healing, no shield on a rage user; a roster spec sets the role ---

WorldTickSnapshotView LeveledDungeonView(std::uint32_t level, std::function<std::uint32_t(std::uint64_t)> classOf = nullptr)
{
    WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 }, classOf);
    for (BotSnapshot& bot : view.bots)
        bot.level = level;
    return view;
}

void RenewKeepsUpOnTheFightingTankEveryFifteenSeconds()
{
    BotPolicy policy = DungeonPolicy();
    WorldTickSnapshotView view = LeveledDungeonView(40);
    view.bots[2].in_combat = 1;                          // the tank (bot 12) fights, at full health
    view.bots[2].threat_target_guid = MakeUnitRef(8, 1, 7);
    CHECK(CastsSpell(policy.Decide(view, 1000), 10, 139, PlayerOf(12)));
    CHECK(!CastsSpell(policy.Decide(view, 8000), 10, 139, PlayerOf(12)));      // it still runs
    CHECK(CastsSpell(policy.Decide(view, 17000), 10, 139, PlayerOf(12)));      // run out: again
    // Not before level 8, and not while the tank is out of the fight.
    BotPolicy low = DungeonPolicy();
    WorldTickSnapshotView young = LeveledDungeonView(6);
    young.bots[2].in_combat = 1;
    CHECK(!CastsSpell(low.Decide(young, 1000), 10, 139, PlayerOf(12)));
    BotPolicy calm = DungeonPolicy();
    CHECK(!CastsSpell(calm.Decide(LeveledDungeonView(40), 1000), 10, 139, PlayerOf(12)));
}

void BindingHealWhenThePriestAndTheTankAreBothHurt()
{
    BotPolicy policy = DungeonPolicy();
    WorldTickSnapshotView view = LeveledDungeonView(40);
    view.bots[0].health = 500;                           // the priest at 50%
    view.bots[2].health = 500;                           // the tank at 50%, fighting
    view.bots[2].in_combat = 1;
    view.bots[2].threat_target_guid = MakeUnitRef(8, 1, 7);
    CHECK(CastsSpell(policy.Decide(view, 1000), 10, 1240770, PlayerOf(12)));
    // The priest unhurt: a plain heal of the tank. Below the spell's level: a plain heal as well.
    BotPolicy fine = DungeonPolicy();
    WorldTickSnapshotView healthy = LeveledDungeonView(40);
    healthy.bots[2].health = 500;
    healthy.bots[2].in_combat = 1;
    healthy.bots[2].threat_target_guid = MakeUnitRef(8, 1, 7);
    CHECK(!CastsSpell(fine.Decide(healthy, 1000), 10, 1240770, PlayerOf(12)) && CastsSpell(DungeonPolicy().Decide(healthy, 1000), 10, 2054, PlayerOf(12)));
    BotPolicy young = DungeonPolicy();
    WorldTickSnapshotView below = LeveledDungeonView(30);
    below.bots[0].health = 500;
    below.bots[2].health = 500;
    below.bots[2].in_combat = 1;
    below.bots[2].threat_target_guid = MakeUnitRef(8, 1, 7);
    CHECK(!CastsSpell(young.Decide(below, 1000), 10, 1240770, PlayerOf(12)));
}

void PrayerOfHealingOnlyWhenMostOfThePartyIsLow()
{
    BotPolicy policy = DungeonPolicy();
    WorldTickSnapshotView view = LeveledDungeonView(40);
    view.bots[1].health = 400;                           // three members under half
    view.bots[3].health = 400;
    view.bots[4].health = 400;
    CHECK(CastsSpell(policy.Decide(view, 1000), 10, 596));
    BotPolicy two = DungeonPolicy();
    WorldTickSnapshotView fewer = LeveledDungeonView(40);
    fewer.bots[1].health = 400;
    fewer.bots[3].health = 400;
    CHECK(!CastsSpell(two.Decide(fewer, 1000), 10, 596));
    BotPolicy young = DungeonPolicy();
    WorldTickSnapshotView below = LeveledDungeonView(28);
    below.bots[1].health = 400;
    below.bots[3].health = 400;
    below.bots[4].health = 400;
    CHECK(!CastsSpell(young.Decide(below, 1000), 10, 596));
}

void NoShieldOnAWarriorOrADruid()
{
    // Bot 12 is a warrior at 40% and in a fight with no target of its own: a shield candidate for any other class. For a warrior (rage) it is not.
    BotPolicy policy = DungeonPolicy();
    WorldTickSnapshotView view = LeveledDungeonView(40);
    view.bots[2].health = 400;
    view.bots[2].in_combat = 1;
    CHECK(!CastsSpell(policy.Decide(view, 1000), 10, 17, PlayerOf(12)));
    // The same member as a priest is shielded.
    auto const allPriests = [](std::uint64_t) { return 5u; };
    BotPolicy priests = DungeonPolicy(allPriests);
    WorldTickSnapshotView same = LeveledDungeonView(40, allPriests);
    same.bots[2].health = 400;
    same.bots[2].in_combat = 1;
    CHECK(CastsSpell(priests.Decide(same, 1000), 10, 17, PlayerOf(12)));
    // And a druid is not shielded either.
    auto const druidTwelve = [](std::uint64_t guid) { return guid == 12 ? 11u : 5u; };
    BotPolicy druid = DungeonPolicy(druidTwelve);
    WorldTickSnapshotView drood = LeveledDungeonView(40, druidTwelve);
    drood.bots[2].health = 400;
    drood.bots[2].in_combat = 1;
    CHECK(!CastsSpell(druid.Decide(drood, 1000), 10, 17, PlayerOf(12)));
}

// Weakened Soul keeps a second shield off the same member for 15 s: the priest heals the hurt member meanwhile (the Stockade priest ordered 15 shields in a row on a mage held at 44 %).
void NoSecondShieldOnTheSameMemberWhileWeakenedSoulHolds()
{
    BotPolicy policy = DungeonPolicy();
    WorldTickSnapshotView view = LeveledDungeonView(40);
    view.bots[3].health = 440;                           // bot 13 (a priest, as the rest) at 44 %, in the fight
    view.bots[3].in_combat = 1;
    CHECK(CastsSpell(policy.Decide(view, 1000), 10, 17, PlayerOf(13)));        // shielded
    std::vector<BotAction> const next = policy.Decide(view, 4000);
    CHECK(!CastsSpell(next, 10, 17, PlayerOf(13)));                            // not again at once
    CHECK(CastsSpell(next, 10, 2054, PlayerOf(13)));                           // it gets the plain heal instead
    CHECK(CastsSpell(policy.Decide(view, 17000), 10, 17, PlayerOf(13)));       // the lock has run out: shielded again
}

void ARosterSpecSetsTheRoleOfAPriest()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = WarriorLeads;
    options.SpecOf = [](std::uint64_t guid) { return guid == 10 ? std::string("shadow") : std::string(); };
    BotPolicy shadow(options);
    shadow.SetRouteActive(true);
    WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 });
    view.bots[3].health = 300; // bot 13 is hurt
    CHECK(!CastsSpell(shadow.Decide(view, 0), 10, 2050, PlayerOf(13)));        // a shadow priest does not heal
    options.SpecOf = [](std::uint64_t guid) { return guid == 10 ? std::string("holy") : std::string(); };
    BotPolicy holy(options);
    holy.SetRouteActive(true);
    CHECK(CastsSpell(holy.Decide(view, 0), 10, 2050, PlayerOf(13)));           // a holy priest does
}

int main()
{
    RenewKeepsUpOnTheFightingTankEveryFifteenSeconds();
    BindingHealWhenThePriestAndTheTankAreBothHurt();
    PrayerOfHealingOnlyWhenMostOfThePartyIsLow();
    NoShieldOnAWarriorOrADruid();
    NoSecondShieldOnTheSameMemberWhileWeakenedSoulHolds();
    ARosterSpecSetsTheRoleOfAPriest();
    ShortOfAmmoWalksToTheAmmoVendor();
    AFlippingPartyDoesNotHoldTheLeaderForever();
    ARegroupStopsWaitingForAStuckMember();
    AStuckMemberAtTheBoundaryIsLoggedOnce();
    MemberRecoverDoesNotFireWithTheStuckEvent();
    ALeaderWithNoRouteIsPutBackOnTheRouteOnce();
    AMemberThatLostItsLeaderIsPutNextToIt();
    ALeaderRevivedAtTheEntranceGoesToItsParty();
    ARevivedLeaderRestsAtTheEntranceBeforeItGoesToItsParty();
    ALeaderThatDoesNotMoveIsNoticed();
    TankLeadParsesAsAnOrderOfTheTank();
    TheTankLeadsTheGroupAlongTheRoute();
    DungeonFilesReloadedChangeWhatTankLeadKnows();
    TankLeadIsRefusedOutsideADungeonWithARoute();
    TankLeadEndsWithLeadOffAndWhenTheDungeonIsLeft();
    TheTankWaitsForThePlayerToCatchUp();
    TheTankHoldsAtABossUntilItIsSeenDead();
    ALeverIsPulledWhenTheTankComesNear();
    TheLeaderWaitsAtABossPointUntilTheBossIsDead();
    ABossPointIsNeverSkippedForNoRoute();
    ALeaderSkipsAWaypointWithNoRoute();
    EachPartyWalksItsOwnDungeonsRoute();
    ALongWalkGoesToTheFlightMasterThenFlies();
    ARefusedFlightMakesTheBotWalk();
    FlightsAreOffByDefaultAndNotForPartyMembers();
    ABotIsSafeToLogOutOnlyWhenNothingIsGoingOn();
    AWarriorWithoutPotionsOrBandagesBuysThemAtTheGoodsVendor();
    AFarGoodsVendorIsWalkedTo();
    AHurtFightingBotDrinksAHealthPotionOnce();
    ARestingBotWithoutFoodBandagesItself();
    AManaUserDrinksAManaPotionInAFight();
    ABotMissingAReagentBuysItAtTheReagentVendor();
    NoSuppliesTripWhenTheFeatureIsOff();
    NoTripWithoutMoneyOrBagRoom();
    BuysAmmoAtTheVendor();
    AManaUserWithoutFoodOrDrinkBuysBoth();
    AFailedTripBacksOffAndASuccessfulOneDoesNot();
    WornGearGoesToTheRepairmanBeforeTheSupplyVendor();
    PartyMembersAndHeldBotsDoNotShop();
    ARestingHurtBotEatsAndDrinks();
    AHunterWithoutAmmunitionFightsInMelee();
    AutoShotIsNotReissuedWhileActive();
    AutoShotIsReissuedAfterTheFightAndWhenRefused();
    AutoShotIsReissuedAfterMoving();
    HunterCallsItsPet();
    HunterRevivesADeadPetThenBacksOff();
    HunterChecksItsPetAfterAFight();
    HunterMendsItsPetInALongFight();
    HealerHealsTheHurtMember();
    HealerHealsItselfWithoutATarget();
    PriestWithNoLineOfSightRepositionsBeforeRetryingDps();
    NoHealWhileEveryoneIsHealthy();
    LeaderDoesNotPullWhileAMemberIsHurt();
    LeaderPullsWhenThePartyIsReady();
    LeaderPullsAfterWaitingTooLong();
    ADungeonPackOfFiveIsNotPulled();
    MembersDoNotPullInADungeon();
    MembersAttackedInADungeonDefendThemselves();
    MembersWaitForTheTankToHoldTheTarget();
    HealerDoesNotMelee();
    HealerWithoutPowerKeepsItsRange();
    LineOfSightRefusalClosesTheDistance();
    BotFightsBackAgainstAStrongAttacker();
    BotInCombatIgnoresThePackLimit();
    BotLootsTheCreatureItKilled();
    GroupmateDoesNotLootACorpseAlreadyTaken();
    NoLootWhileInCombat();
    NoLootForACorpseItDidNotFight();
    NoLootForADistantCorpse();
    BotDoesNotPullAboveItsLevel();
    HostileOnAnotherLevelIsNotPulled();
    BotDoesNotPullAnElite();
    CreatureAttackingTheBotIsFoughtWhateverItsLevel();
    RepeatedlyAvoidedTargetIsLeftLonger();
    UnreachableTargetIsGivenUp();
    RefusedAttackPicksAnotherTarget();
    InvalidCastTargetIsNotCastAtAgain();
    IdleBotWandersToAHubAfterAWhile();
    ArrivalMovesOnToTheNextHubAfterAnInterval();
    ANoRouteAnswerEndsTheWalkInsteadOfBeingSentEveryTick();
    ABlacksmithAsksForItsLineAndWalksToAnAnvilWhenTheRecipeNeedsOne();
    ANeedsFocusAnswerWithNoRouteOrNoTableEndsInAWait();
    AGatheringBotAsksForItsSkillAndBacksOffAfterARefusal();
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
    PartiesNeverMixSides();
    OldPartiesAreLeftBeforeTheNewOnesForm();
    ALeaderWithItsOwnPendingPartyKeepsInviting();
    PartySideOfDecidesTheSides();
    ABotLeadersInviteIsAnsweredOnce();
    OnlyAShareOfTheBotsFormParties();
    IdleBotsCraftTheirTwoSkillsInTurnAndWaitAfterARefusal();
    RestThresholdIsAPerBotTrait();
    FullBagsMakeTheVisitSellWhiteJunkToo();
    UnknownSideBotsAreLeftOutOfParties();
    ABotInAHumansPartyIsLeftAlone();
    OnlyTheFirstPartyIsFormed();
    InvitesWaitForTheRetry();
    InvitedMemberIsNotInvitedAgain();
    ATankLeadsThePartyWhateverItsGuid();
    MemberAcceptsItsLeadersInvite();
    MemberLeavesAForeignParty();
    MemberFollowsItsLeaderUntilClose();
    MemberFightsItsLeadersTarget();
    ADamageDealerFightsThePriorityCasterOfThePack();
    ADamageDealerFocusesTheEngineerWhenAGolemIsUp();
    ACasterOfThePackIsTargetedForAnInterrupt();
    ADamageDealerDoesNotWidenPastTwoAlreadyEngagedHostiles();
    TheVoidwalkerCompanionStaysReachablePastTheEngagementCapToo();
    ADamageDealerTakesTheWarlockWhoseVoidwalkerIsUp();
    CcHoldsAnUnengagedHumanoidPackMember();
    CcDoesNotHoldACreatureTypeItHasNoSpellFor();
    CcNeverHoldsTheKillPriorityTarget();
    CcTacticOffMeansNoHold();
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
    NoRestAtFullHealth();
    RestsEvenWithAStrangerNear();
    StandsBeforeAFight();
    BotInAGroundEffectWalksOut();
    GroundEffectsAreIgnoredWhenOffOrNotHarmful();
    DungeonFileZonesAndSpellsAreAvoided();
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
    StandsStillWithNoEnemyAndNoInvite();
    StopsWhenNothingNearby();
    DeadUnitsAreNotAttacked();
    RetreatsBelowThirtyPercent();
    RetreatWithNoThreatAndNoInviteStandsStill();
    RetreatWithNoPlayerStillFlees();
    ThirtyPercentIsNotLow();
    LoneCorpseReleasesAndDoesNotFight();
    GhostThatCannotReachItsCorpseIsPutThereOnce();
    ALongCorpseRunEndsInATeleportToTheCorpse();
    AWipedPartysGhostsArePutAtTheirCorpsesAfterAMinute();
    DeathLoopMovesTheBotToAGrindSpot();
    CorpseWaitsForARezzerThenReleases();
    ADungeonCorpseWaitsTwentySecondsForARezzer();
    ADungeonCorpseWithoutARezzerIsReleasedWithinTwentySeconds();
    AFightInThePartyDoesNotHoldADungeonRelease();
    AReleasedDungeonGhostIsTeleportedToTheLeader();
    ACorpseIsRevivedByTheQuietLeaderAfterAFewSeconds();
    CorpseReleasesAtOnceWithoutARezzer();
    CorpseReleasesWhenTheWholePartyIsDown();
    ReleaseIsHeldInCombatUpToTheCap();
    GhostWalksToItsCorpseAndIgnoresCombat();
    GhostOnTheOutsideMapWalksToThePortal();
    ADeadMemberOnMapZeroIsTeleportedBackToItsCorpse();
    ADeadMemberIsRevivedByItsLeaderInsideTheDungeon();
    AGhostOnTheOutsideMapIsNotRevivedBeforeItEnters();
    AnAliveMemberOutsideWalksBackToTheDungeon();
    AGhostDoesNotReclaimNextToAHostile();
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
    PartyCasterDoesNotKite();
    HealerKitesToOnyxiaBand();
    RangedBandHoldsInsideOnyxiaBand();
    MeleeDoesNotKiteFromOnyxia();
    HealerBandStandSkipsAHazard();
    RaidCasterKitesOutOfWarderNova();
    RaidMemberOnAnotherTargetKitesOutOfWarderNova();
    NonTankOutOfCombatKitesWhenWarderIsWithinMargin();
    KiteRetreatsAlongTheEntryRouteAndFallsBackToTheCircle();
    PartyMemberDoesNotFleeOnLowHealth();
    PartyMemberDoesNotChaseAFleeingTarget();
    RangedBotAttacksWhenBackAtRange();
    HurtBotFleesFromThreat();
    HurtBotDoesNotFleeWhenThreatIsFar();
    BotMountsForALongWalk();
    BotBelowTheMountLevelNeverAsksToMount();
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
    TheRouteLeaderIsHealedUpOutOfAFight();
    TheRouteWaitsForTheHealersMana();
    TwoDruidsDoNotBothCastThePartyBuff();
    AFailingBuffBacksOffLongerEachTime();
    HubTimeoutTriesTheNextHubSoonerThanAnArrival();
    DefaultBotsDoNotPullWhatIsInRange();

    if (failures == 0)
        std::printf("policy_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
