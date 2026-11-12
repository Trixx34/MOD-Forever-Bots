// SPDX-License-Identifier: GPL-2.0-or-later
// Policy tests, part 3 of 8 (split out of policy_test.cpp; the helpers are in policy_test_support.h).
#include "policy_test_support.h"

namespace {

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

void ALowTankPullsNothingUntilItsHealthIsBack()
{
    std::vector<std::string> events;
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = WarriorLeads;
    options.Route = { Point{ 100.0f, 0.0f, 0.0f } };
    options.OnEvent = [&events](std::string const& text) { events.push_back(text); };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 }, WarriorLeads);
    NearbyUnit const hostile = Hostile(7, 15.0f, 100);
    for (std::size_t i = 0; i < view.bots.size(); ++i)
    {
        view.bots[i].group_role = view.bots[i].guid == 12 ? 2 : 1;
        view.bots[i].group_leader_guid = (std::uint64_t(2) << 56) | 12;
        view.bots[i].max_power = 0;
        AddNearby(view, i, hostile);
    }
    view.bots[2].health = 490;   // the leader (12) at 49 %
    auto const pulls = [&](std::vector<BotAction> const& actions)
    {
        return HasAction(actions, 12, ActionType::kFollow, hostile.guid) || HasAction(actions, 12, ActionType::kAttackTarget, hostile.guid);
    };
    bool early = false;
    for (std::uint64_t at = 1000; at <= 170000; at += 1000)
    {
        view.tick_number = at / 100;
        early = early || pulls(policy.Decide(view, at));
    }
    CHECK(!early);
    CHECK(Logged(events, "tank_rest: leader=12"));
    view.bots[2].health = 1000;   // healed
    bool later = false;
    for (std::uint64_t at = 171000; at <= 200000 && !later; at += 1000)
    {
        view.tick_number = at / 100;
        later = pulls(policy.Decide(view, at));
    }
    CHECK(later);
}

// 0.44.14: the waits give up 45 s (pull hold) / 60 s (route) after the party's health plus mana last rose, and at 180 s at the latest while it keeps rising.
void ThePullHoldEndsAfterNoRiseAndAtTheAbsoluteCap()
{
    PolicyOptions base = AggressiveOptions();
    base.PartySize = 5;
    base.PartyCount = 1;
    base.ClassOf = WarriorLeads;
    base.Route = { Point{ 100.0f, 0.0f, 0.0f } };
    // Runs the hold with a mana user (bot 10) whose mana is `power(step)` at each step; returns the time the leader first walks on, 0 if it never does.
    auto const heldFor = [&](std::function<std::uint32_t(std::uint64_t)> const& power, std::uint64_t stepMs, std::uint64_t untilMs)
    {
        PolicyOptions options = base;
        BotPolicy policy(options);
        policy.SetRouteActive(true);
        WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 }, WarriorLeads);
        for (BotSnapshot& bot : view.bots)
        {
            bot.group_role = bot.guid == 12 ? 2 : 1;
            bot.group_leader_guid = (std::uint64_t(2) << 56) | 12;
            bot.max_power = 0;
        }
        for (std::uint64_t at = 1000; at <= untilMs; at += stepMs)
        {
            for (BotSnapshot& bot : view.bots)
                if (bot.guid == 10)
                {
                    bot.power_type = 0;
                    bot.max_power = 100000;
                    bot.power = power(at);
                }
            for (BotAction const& action : policy.Decide(view, at))
                if (action.bot_guid == 12 && action.type == ActionType::kMoveTo && action.dest_x == 100.0f)
                    return at;
        }
        return std::uint64_t(0);
    };
    // Flat mana: the leader walks on after about 40 s.
    std::uint64_t const flat = heldFor([](std::uint64_t) { return 5000u; }, 5000, 120000);
    CHECK(flat >= 30000 && flat <= 50000);   // the 35 s route wait lapses by 36 s, the 45 s pull hold by 46 s
    // Mana rising 8.5 % of the maximum every 5 s and falling back every fourth step: the hold goes on past 45 s and is cut off at 180 s.
    std::uint64_t const rising = heldFor([](std::uint64_t at) { return std::uint32_t(5000 + ((at / 5000) % 4) * 8500); }, 5000, 300000);
    CHECK(rising >= 180000 && rising <= 200000);
}

// 0.44.10: in a Deadmines run a cast result is logged with the bot's level and what the highest rank of the spell costs at that level (-1 when unpriced).
void ACastResultInTheDeadminesLogsTheWantedCost()
{
    PolicyOptions options = AggressiveOptions();
    std::istringstream costs("1126\t1\t0\t200\t0\t0\t0\tMark\n");
    std::vector<std::string> warnings;
    options.SpellCosts = std::make_shared<spell_cost::SpellCostData>(spell_cost::ParseSpellCosts(costs, warnings));
    std::vector<std::string> events;
    options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
    PolicyOptions::ScenarioRoute deadmines;
    deadmines.Route = { Point{ 100.0f, 0.0f, 0.0f } };
    deadmines.RouteMap = 36;
    BotPolicy policy(options);
    policy.SetRouteActiveFor([](std::uint64_t) { return true; });
    policy.SetScenarioRouteFor([&](std::uint64_t) -> PolicyOptions::ScenarioRoute const* { return &deadmines; });
    auto const resultFor = [&](std::uint32_t spell, std::uint32_t sequence, std::uint64_t at)
    {
        WorldTickSnapshotView view = PartyView({ 10 });
        view.bots[0].level = 23;
        view.bots[0].last_action_sequence = sequence;
        view.bots[0].last_action_type = uint8_t(ActionType::kCastSpell);
        view.bots[0].last_action_spell = spell;
        view.bots[0].last_action_status = BotActionStatus::kApplied;
        policy.Decide(view, at);
    };
    resultFor(1126, 1, 1000);
    resultFor(99999, 2, 2000);
    bool priced = false, unpriced = false;
    for (std::string const& line : events)
    {
        if (line.find("cast_result:") == std::string::npos)
            continue;
        if (line.find(" spell=1126 level=23 want_cost=200 ") != std::string::npos)
            priced = true;
        if (line.find(" spell=99999 level=23 want_cost=-1 ") != std::string::npos)
            unpriced = true;
    }
    CHECK(priced);
    CHECK(unpriced);
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

// 0.45.3: a boss killed away from its point (Targorr the Dread, the Stockade's point 0, pulled down the corridor): a leader already by the next point drops the boss
// point as it drops any point it has passed. While the boss lives the boss point is never dropped.
void ABossPointWhoseBossDiedElsewhereIsPassed()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.ClassOf = WarriorLeads;
    options.Route = { Point{ 160.0f, 0.0f, 0.0f }, Point{ 146.0f, 0.0f, 0.0f }, Point{ 100.0f, 0.0f, 0.0f } };   // a dead end at 160, then back west
    options.RouteBossAt[0] = 644;
    std::vector<std::string> events;
    options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
    auto const at = [](float x)
    {
        WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
        for (BotSnapshot& bot : view.bots)
            bot.pos_x = x;
        return view;
    };
    // The boss alive: the leader 22 yards short of the boss point (and 8 from the next one) still goes to the boss point.
    BotPolicy alive(options);
    alive.SetRouteActive(true);
    CHECK(LeaderWalk(alive.Decide(at(138.0f), 0)) == 160.0f);
    // The boss dead (killed at x = 138): the boss point is passed, on to the next one.
    BotPolicy dead(options);
    dead.SetRouteActive(true);
    dead.SetKilledBosses({ 644 });
    CHECK(LeaderWalk(dead.Decide(at(138.0f), 0)) == 146.0f);
    bool passed = false;
    for (std::string const& line : events)
        passed = passed || line.rfind("route_passed: bot=12 point=0/3", 0) == 0;
    CHECK(passed);
}

// 2026-11-12 (Shadowfang Keep): a leader on a floor above the boss point (z 100 over Rethilgore's room at z 82, 5 yards away on the map) has not reached it: it does not
// pass the points before it, does not stand under the boss for kBossHoldMs, and walks on to the point. On the point's own floor it holds as before.
void ALeaderOnAnotherFloorHasNotReachedTheBossPoint()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.ClassOf = WarriorLeads;
    options.Route = { Point{ 100.0f, 0.0f, 82.0f }, Point{ 104.0f, 0.0f, 82.0f }, Point{ 180.0f, 0.0f, 82.0f } };
    options.RouteBossAt[1] = 644;
    std::vector<std::string> events;
    options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
    auto const at = [](float z)
    {
        WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
        for (BotSnapshot& bot : view.bots)
        {
            bot.pos_x = 104.0f;
            bot.pos_z = z;
        }
        return view;
    };
    BotPolicy above(options);
    above.SetRouteActive(true);
    CHECK(LeaderWalk(above.Decide(at(100.0f), 0)) == 100.0f); // on the roof: walks to point 0, nothing passed
    for (std::string const& line : events)
        CHECK(line.rfind("route_passed", 0) != 0);
    BotPolicy below(options);
    below.SetRouteActive(true);
    below.Decide(at(83.0f), 0);
    CHECK(LeaderWalk(below.Decide(at(83.0f), 1000)) < 0.0f); // on the floor: it is at the boss point and waits for the boss
}

// 0.44.17: past the last route point with a boss of the route not dead (Stockade: bosses_missing at 23/23), the leader walks back to that boss point, waits there, and
// after the sweep (kBossSweepMaxMs) or the kill it stands at the end.
void TheLeaderSweepsBackToAMissingBoss()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.ClassOf = WarriorLeads;
    options.Route = { Point{ 100.0f, 0.0f, 0.0f }, Point{ 140.0f, 0.0f, 0.0f }, Point{ 180.0f, 0.0f, 0.0f } };
    options.RouteBossAt[1] = 644;
    std::vector<std::string> events;
    options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
    auto const at = [](float x)
    {
        WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
        for (BotSnapshot& bot : view.bots)
            bot.pos_x = x;
        return view;
    };
    auto const walkTo = [&](BotPolicy& policy, float x, std::uint64_t now) { return LeaderWalk(policy.Decide(at(x), now)); };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    walkTo(policy, 0.0f, 0);
    walkTo(policy, 95.0f, 1000);
    CHECK(walkTo(policy, 138.0f, 2000) < 0.0f);                    // at the boss point, the boss alive
    CHECK(walkTo(policy, 138.0f, 190000) == 180.0f);               // boss_missed after kBossHoldMs: on to the last point
    CHECK(walkTo(policy, 178.0f, 200000) < 0.0f);                  // the end: arrived (the next tick sweeps)
    walkTo(policy, 178.0f, 201000);                                // the sweep picks the boss point
    CHECK(walkTo(policy, 178.0f, 202000) == 140.0f);               // back to the boss point
    CHECK(walkTo(policy, 138.0f, 230000) < 0.0f);                  // and waits there
    bool swept = false;
    for (std::string const& line : events)
        swept = swept || line.rfind("boss_sweep: bot=12 point=1/3 boss=644", 0) == 0;
    CHECK(swept);
    policy.SetKilledBosses({ 644 });                               // killed during the wait: back to the end, then it stands
    walkTo(policy, 138.0f, 240000);
    walkTo(policy, 178.0f, 250000);
    CHECK(walkTo(policy, 178.0f, 252000) < 0.0f);

    // A boss that never comes: the sweep gives up after kBossSweepMaxMs and the leader stays at the end.
    BotPolicy patient(options);
    patient.SetRouteActive(true);
    walkTo(patient, 0.0f, 0);
    walkTo(patient, 95.0f, 1000);
    walkTo(patient, 138.0f, 2000);
    walkTo(patient, 138.0f, 190000);
    walkTo(patient, 178.0f, 200000);
    walkTo(patient, 178.0f, 201000);                               // the sweep starts at 201 s
    CHECK(walkTo(patient, 178.0f, 202000) == 140.0f);
    CHECK(walkTo(patient, 138.0f, 203000) < 0.0f);
    CHECK(walkTo(patient, 138.0f, 400000) < 0.0f);                 // still waiting (199 s into the sweep)
    walkTo(patient, 138.0f, 502000);                               // 301 s: the hold ends, the leader goes to the end
    walkTo(patient, 178.0f, 503000);
    CHECK(walkTo(patient, 178.0f, 504000) < 0.0f);                 // and does not go back again
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

// A teleport event moves the route index: the bridge to a boss on a cut-off island lands on the point before the boss, so the walk goes on to the boss, not back to the
// point the leader was teleported away from (Blackfathom Deeps, Lady Sarevess).
void ATeleportEventSnapsTheRouteIndexToThePointItLandsOn()
{
    PolicyOptions options = LeadOptions();
    options.DungeonTeleport = true;
    PolicyOptions::DungeonRoute route;
    route.Route = { Point{ 100.0f, 0.0f, 0.0f }, Point{ 300.0f, 0.0f, 0.0f }, Point{ 340.0f, 0.0f, 0.0f }, Point{ 100.0f, 0.0f, 0.0f } };
    route.BossAt[0] = 644;
    route.BossAt[2] = 645;
    std::vector<std::string> warnings;
    route.Events = dungeon_events::Parse("event 36 bridge Bridge\nafter_boss 644\nteleport 300 0 0\nwait 5\nend\n", warnings);
    CHECK(warnings.empty());
    options.DungeonsByMap[36] = route;
    std::vector<std::string> events;
    options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
    BotPolicy policy(options);
    WorldTickSnapshotView view = PlayerGroupView(98.0f);
    policy.Decide(view, 0);
    policy.ApplyChat(view, FromPlayer(), TankLead(), 1000);
    NearbyUnit corpse;
    corpse.guid = MakeUnitRef(8, 644, 55);
    corpse.health = 0;
    AddNearby(view, 3, corpse);                                        // the first boss is dead
    std::optional<BotAction> jump = OfTank(policy.Decide(view, 2000), ActionType::kTeleport);
    CHECK(jump.has_value() && jump->dest_x == 300.0f);
    for (BotSnapshot& bot : view.bots)
        bot.pos_x = 300.0f;                                            // the party is on the island
    view.bots[2].nearby_count = 0;
    NearbyUnit player;
    player.guid = PlayerOf(900);
    player.pos_x = 300.0f;
    AddNearby(view, 2, player);
    AddNearby(view, 3, corpse);
    policy.Decide(view, 3000);
    CHECK(WalkTo(policy.Decide(view, 12000), 12) == 340.0f);           // on to the boss on the island, not back to the first point
    bool snapped = false;
    for (std::string const& line : events)
        snapped = snapped || line.find("route_teleport_snap") != std::string::npos;
    CHECK(snapped);
}

// The snap stops at a boss still alive (Shadowfang Keep, 2026-11-07: the courtyard-door teleport lands on route point 31, and a leader at point 1-3 was
// snapped there past four living bosses). A teleport that lands past a living boss leaves the route index where it was.
void ATeleportEventDoesNotSnapPastALivingBoss()
{
    PolicyOptions options = LeadOptions();
    options.DungeonTeleport = true;
    PolicyOptions::DungeonRoute route;
    route.Route = { Point{ 100.0f, 0.0f, 0.0f }, Point{ 200.0f, 0.0f, 0.0f }, Point{ 300.0f, 0.0f, 0.0f }, Point{ 340.0f, 0.0f, 0.0f } };
    route.BossAt[0] = 644;
    route.BossAt[1] = 646; // alive, between the leader and the point the teleport lands on
    route.BossAt[3] = 645;
    std::vector<std::string> warnings;
    route.Events = dungeon_events::Parse("event 36 door Door\nafter_boss 644\nteleport 300 0 0\nwait 5\nend\n", warnings);
    CHECK(warnings.empty());
    options.DungeonsByMap[36] = route;
    std::vector<std::string> events;
    options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
    BotPolicy policy(options);
    WorldTickSnapshotView view = PlayerGroupView(98.0f);
    policy.Decide(view, 0);
    policy.ApplyChat(view, FromPlayer(), TankLead(), 1000);
    NearbyUnit corpse;
    corpse.guid = MakeUnitRef(8, 644, 55);
    corpse.health = 0;
    AddNearby(view, 3, corpse);
    std::optional<BotAction> jump = OfTank(policy.Decide(view, 2000), ActionType::kTeleport);
    CHECK(jump.has_value() && jump->dest_x == 300.0f);
    bool snapped = false;
    for (std::string const& line : events)
        snapped = snapped || line.find("route_teleport_snap") != std::string::npos;
    CHECK(!snapped);
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

// A leader far from the point it gets no route to (revived at the entrance) is rewound to the nearest point, not skipped past it.
void AFarLeaderWithNoRouteIsRewoundNotSkipped()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.ClassOf = WarriorLeads;
    options.DungeonTeleport = true;
    options.Route = { Point{ 100.0f, 0.0f, 0.0f }, Point{ 600.0f, 0.0f, 0.0f }, Point{ 1000.0f, 0.0f, 0.0f } };
    std::vector<std::string> events;
    options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    for (BotSnapshot& bot : view.bots)
        bot.pos_x = 95.0f;
    policy.Decide(view, 0);                                           // at the first point: on to the second
    for (BotSnapshot& bot : view.bots)
        bot.pos_x = 0.0f;                                             // killed and revived at the entrance
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
    CHECK(teleport.has_value() && teleport->dest_x == 100.0f);
    bool rewound = false;
    bool skipped = false;
    for (std::string const& line : events)
    {
        rewound = rewound || line.rfind("route_rewind: bot=12 from=1 to=0", 0) == 0;
        skipped = skipped || line.rfind("route_skip:", 0) == 0;
    }
    CHECK(rewound);
    CHECK(!skipped);
}

} // namespace

void RunPolicyTestsPart3()
{
    AFarLeaderWithNoRouteIsRewoundNotSkipped();
    TankLeadParsesAsAnOrderOfTheTank();
    TheTankLeadsTheGroupAlongTheRoute();
    DungeonFilesReloadedChangeWhatTankLeadKnows();
    TankLeadIsRefusedOutsideADungeonWithARoute();
    TankLeadEndsWithLeadOffAndWhenTheDungeonIsLeft();
    TheTankWaitsForThePlayerToCatchUp();
    TheTankHoldsAtABossUntilItIsSeenDead();
    ALeverIsPulledWhenTheTankComesNear();
    ATeleportEventSnapsTheRouteIndexToThePointItLandsOn();
    ATeleportEventDoesNotSnapPastALivingBoss();
    TheLeaderWaitsAtABossPointUntilTheBossIsDead();
    ABossPointWhoseBossDiedElsewhereIsPassed();
    ALeaderOnAnotherFloorHasNotReachedTheBossPoint();
    TheLeaderSweepsBackToAMissingBoss();
    ABossPointIsNeverSkippedForNoRoute();
    ALeaderSkipsAWaypointWithNoRoute();
    EachPartyWalksItsOwnDungeonsRoute();
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
    ACastResultInTheDeadminesLogsTheWantedCost();
    ThePullHoldEndsAfterNoRiseAndAtTheAbsoluteCap();
    ALowTankPullsNothingUntilItsHealthIsBack();
}
