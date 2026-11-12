// SPDX-License-Identifier: GPL-2.0-or-later
// Policy tests, part 1 of 8 (split out of policy_test.cpp; the helpers are in policy_test_support.h).
#include "policy_test_support.h"

namespace {

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

void RaidRulesAvoidSpellGivesTheRadius()
{
    // A spell of the raid rule table's avoid rows is harmful even when its caster is not known hostile, with the rule's radius (a 12-yard circle here, not the snapshot's 2).
    PolicyOptions options = AggressiveOptions();
    options.AvoidGroundEffects = true;
    std::vector<std::string> errors;
    options.RaidRules = std::make_shared<raid_rules::Table>(raid_rules::Parse("avoid|249|spell|18392|12|Fireball splash\n", errors));
    CHECK(errors.empty());
    BotPolicy policy(options);
    WorldTickSnapshotView view = View(1, 0, {});
    view.bots[0].map_id = 249;
    SetGroundEffect(view, 18392, 2.0f, 0.0f, 2.0f, 0);
    auto const moves = GroundMoves(policy.Decide(view, 1000));
    CHECK(moves.size() == 1);
    if (moves.size() == 1)
    {
        float const dx = moves[0].dest_x - 2.0f, dy = moves[0].dest_y;
        CHECK(std::sqrt(dx * dx + dy * dy) >= 15.0f - 0.01f);        // the rule's 12 yards plus the 3-yard margin
    }
    // the same spell with no rule table: not hostile, no walk
    PolicyOptions plain = AggressiveOptions();
    plain.AvoidGroundEffects = true;
    BotPolicy without(plain);
    CHECK(GroundMoves(without.Decide(view, 1000)).empty());
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

} // namespace

void RunPolicyTestsPart1()
{
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
    RaidRulesAvoidSpellGivesTheRadius();
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
    OnlyChangesArePublished();
}
