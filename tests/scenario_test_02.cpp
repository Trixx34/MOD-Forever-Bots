// SPDX-License-Identifier: GPL-2.0-or-later
// Scenario tests, part 2 of 4 (split out of scenario_test.cpp; the helpers are in scenario_test_support.h).
#include "scenario_test_support.h"

namespace {

BotPolicy RoutePolicy()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 2;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t guid) { return guid == 12 ? 1u : 5u; };
    options.Route = { { 0.0f, 100.0f, 0.0f }, { 0.0f, 200.0f, 0.0f } };
    return BotPolicy(options);
}

void LeaderWalksTheRouteWhileTheRouteIsActive()
{
    BotPolicy policy = RoutePolicy();
    std::vector<BotSnapshot> bots = { Bot(12, 36, 0, 0, 2, 2), Bot(13, 36, 5, 0, 1, 2) };
    auto const idle = policy.Decide(View(1, bots), 1000);
    CHECK(Count(idle, ActionType::kMoveTo, 12) == 0);

    policy.SetRouteActive(true);
    auto const walking = policy.Decide(View(2, bots), 2000);
    bool toFirst = false;
    for (BotAction const& action : walking)
        if (action.type == ActionType::kMoveTo && action.bot_guid == 12)
            toFirst = action.dest_y == 100.0f;
    CHECK(toFirst);
}

// Parallel parties (ScenarioParties): the route is on per party. Party 1 (bots 12, 13) is inside and running, party 2 (14, 15) is still at its staging place: only
// the leader of party 1 walks, and a boss dead in one party's instance is not dead in the other's.
// The director of one party sees its own bots with their own nearby units: the nearby slots are stored per bot position, so a subset must copy them along.
void ASubsetViewKeepsTheNearbyUnitsOfItsBots()
{
    WorldTickSnapshotView view;
    view.tick_number = 1;
    view.max_nearby_units = 2;
    view.bots = { Bot(10, 36, 0, 0), Bot(11, 36, 1, 0), Bot(12, 36, 2, 0) };
    view.bots[0].nearby_count = 1;
    view.bots[1].nearby_count = 1;
    view.bots[2].nearby_count = 2;
    view.nearby.resize(6);
    view.nearby[0].guid = 100;                            // bot 10's unit
    view.nearby[2].guid = 110;                            // bot 11's unit
    view.nearby[4].guid = 120;
    view.nearby[5].guid = 121;                            // bot 12's two units
    WorldTickSnapshotView const part = SubsetView(view, [](std::uint64_t guid) { return guid == 11 || guid == 12; });
    CHECK(part.bots.size() == 2);
    CHECK(part.NearbyOf(0).size() == 1 && part.NearbyOf(0)[0].guid == 110);
    CHECK(part.NearbyOf(1).size() == 2 && part.NearbyOf(1)[0].guid == 120 && part.NearbyOf(1)[1].guid == 121);
}

void TwoPartiesRunTheirRoutesApartFromEachOther()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 2;
    options.PartyCount = 2;
    options.ClassOf = [](std::uint64_t guid) { return (guid == 12 || guid == 14) ? 1u : 5u; };
    options.Route = { { 0.0f, 100.0f, 0.0f }, { 0.0f, 200.0f, 0.0f } };
    options.RouteBossAt[1] = 4242;
    BotPolicy policy(options);
    bool partyOneRuns = true;
    std::set<std::uint32_t> const killed = { 4242 };
    std::set<std::uint32_t> const none;
    policy.SetRouteActiveFor([&partyOneRuns](std::uint64_t guid) { return partyOneRuns && (guid == 12 || guid == 13); });
    policy.SetKilledBossesFor([&killed, &none](std::uint64_t guid) -> std::set<std::uint32_t> const& { return (guid == 12 || guid == 13) ? killed : none; });
    std::vector<BotSnapshot> bots = { Bot(12, 36, 0, 0, 2, 2), Bot(13, 36, 5, 0, 1, 2), Bot(14, 0, 0, 0, 2, 2), Bot(15, 0, 5, 0, 1, 2) };
    bool oneWalks = false;
    int twoWalks = 0;
    for (std::uint64_t at = 1000; at < 4000; at += 500)
        for (BotAction const& action : policy.Decide(View(at / 500, bots), at))
        {
            oneWalks = oneWalks || (action.type == ActionType::kMoveTo && action.bot_guid == 12 && action.dest_y == 100.0f);
            twoWalks += action.type == ActionType::kMoveTo && action.bot_guid == 14 && action.dest_y == 100.0f;
        }
    CHECK(oneWalks);
    CHECK(twoWalks == 0);
}

void LeaderDoesNotWalkTheDungeonRouteOutsideTheDungeon()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 2;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t guid) { return guid == 12 ? 1u : 5u; };
    options.Route = { { 0.0f, 100.0f, 0.0f }, { 0.0f, 200.0f, 0.0f } };
    options.RouteMap = 36;
    options.DungeonTeleport = true;
    options.WanderWhenIdle = false;
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    // Revived at the graveyard on map 0: the route's points are coordinates of map 36, so there is nothing to walk to (and nothing to teleport to).
    std::vector<BotSnapshot> outside = { Bot(12, 0, -10500, 1200, 2, 2), Bot(13, 0, -10500, 1203, 1, 2) };
    int orders = 0;
    for (std::uint64_t at = 1000; at < 90000; at += 500)
        for (BotAction const& action : policy.Decide(View(at / 500, outside), at))
            orders += (action.type == ActionType::kMoveTo || action.type == ActionType::kTeleport) && action.bot_guid == 12;
    CHECK(orders == 0);
    // Inside the dungeon it walks.
    std::vector<BotSnapshot> inside = { Bot(12, 36, 0, 0, 2, 2), Bot(13, 36, 5, 0, 1, 2) };
    bool toFirst = false;
    for (BotAction const& action : policy.Decide(View(1000, inside), 100000))
        toFirst = toFirst || (action.type == ActionType::kMoveTo && action.bot_guid == 12 && action.dest_y == 100.0f);
    CHECK(toFirst);
}

void LeaderDropsARoutePointItHasAlreadyPassed()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 2;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t guid) { return guid == 12 ? 1u : 5u; };
    options.Route = { { 0.0f, 100.0f, 0.0f }, { 0.0f, 200.0f, 0.0f }, { 0.0f, 300.0f, 0.0f } };
    options.WanderWhenIdle = false;
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    // The fights drew the leader to y = 185: the first point (100 yards behind) is dropped, it walks to the second.
    std::vector<BotSnapshot> bots = { Bot(12, 36, 0, 185, 2, 2), Bot(13, 36, 5, 185, 1, 2) };
    float to = -1.0f;
    for (BotAction const& action : policy.Decide(View(1, bots), 1000))
        if (action.type == ActionType::kMoveTo && action.bot_guid == 12)
            to = action.dest_y;
    CHECK(to == 200.0f);
    // A leader that is farther than the skip distance from the next point still goes to the first one.
    BotPolicy slow(options);
    slow.SetRouteActive(true);
    std::vector<BotSnapshot> start = { Bot(12, 36, 0, 40, 2, 2), Bot(13, 36, 5, 40, 1, 2) };
    to = -1.0f;
    for (BotAction const& action : slow.Decide(View(1, start), 1000))
        if (action.type == ActionType::kMoveTo && action.bot_guid == 12)
            to = action.dest_y;
    CHECK(to == 100.0f);
}

void LeaderWaitsForAMemberWhoFellBehind()
{
    BotPolicy policy = RoutePolicy();
    policy.SetRouteActive(true);
    std::vector<BotSnapshot> bots = { Bot(12, 36, 0, 0, 2, 2), Bot(13, 36, 0, -80, 1, 2) }; // the member is 80 yards back
    auto const actions = policy.Decide(View(1, bots), 1000);
    CHECK(Count(actions, ActionType::kMoveTo, 12) == 0);
}

void LeaderMovesOnAfterTheFirstPoint()
{
    BotPolicy policy = RoutePolicy();
    policy.SetRouteActive(true);
    std::vector<BotSnapshot> bots = { Bot(12, 36, 0, 100, 2, 2), Bot(13, 36, 0, 95, 1, 2) }; // at the first point
    auto const actions = policy.Decide(View(1, bots), 1000);
    bool toSecond = false;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kMoveTo && action.bot_guid == 12)
            toSecond = action.dest_y == 200.0f;
    CHECK(toSecond);
}

// ---- dungeon pulls ----

void TheRoutePartyPullsElitesButGrindingBotsDoNot()
{
    auto const eliteView = []
    {
        WorldTickSnapshotView view;
        view.tick_number = 1;
        view.max_nearby_units = 1;
        BotSnapshot bot = Bot(12, 36, 0, 0, 2, 2);
        bot.level = 18;
        bot.nearby_count = 1;
        view.bots = { bot };
        view.nearby.resize(1);
        view.nearby[0].guid = forever_module::MakeUnitRef(8, 644, 1);
        view.nearby[0].pos_x = 15.0f;
        view.nearby[0].health = 100;
        view.nearby[0].max_health = 100;
        view.nearby[0].is_hostile = true;
        view.nearby[0].level = 21; // three levels above, and elite
        view.nearby[0].elite = 1;
        return view;
    };
    BotPolicy grinding = RoutePolicy();
    CHECK(Count(grinding.Decide(eliteView(), 1000), ActionType::kAttackTarget) == 0); // not worth it for a lone bot
    BotPolicy dungeon = RoutePolicy();
    dungeon.SetRouteActive(true);
    auto const actions = dungeon.Decide(eliteView(), 1000);
    CHECK(Count(actions, ActionType::kAttackTarget) + Count(actions, ActionType::kFollow) >= 1);
}

void AStagingBotDoesNotPullWhatDoesNotAttackIt()
{
    auto const mobView = [](bool inCombat)
    {
        WorldTickSnapshotView view;
        view.tick_number = 1;
        view.max_nearby_units = 1;
        BotSnapshot bot = Bot(12, 1, 0, 0);
        bot.level = 60;
        bot.in_combat = inCombat ? 1 : 0;
        bot.nearby_count = 1;
        view.bots = { bot };
        view.nearby.resize(1);
        view.nearby[0].guid = forever_module::MakeUnitRef(8, 4328, 1);
        view.nearby[0].pos_x = 15.0f;
        view.nearby[0].health = 100;
        view.nearby[0].max_health = 100;
        view.nearby[0].is_hostile = true;
        view.nearby[0].level = 58; // worth a pull for a free bot
        return view;
    };
    auto const engaged = [](std::vector<BotAction> const& actions)
    {
        return Count(actions, ActionType::kAttackTarget) + Count(actions, ActionType::kFollow);
    };
    BotPolicy free = RoutePolicy();
    CHECK(engaged(free.Decide(mobView(false), 1000)) >= 1);
    BotPolicy held = RoutePolicy();
    held.SetStagingHoldFor([](std::uint64_t) { return true; });
    CHECK(engaged(held.Decide(mobView(false), 1000)) == 0); // waits at staging
    BotPolicy attacked = RoutePolicy();
    attacked.SetStagingHoldFor([](std::uint64_t) { return true; });
    CHECK(engaged(attacked.Decide(mobView(true), 1000)) >= 1); // defends itself
}

// ---- auto gear (policy) ----

void AutoGearingDressesEachBotOnceAndAgainAfterLevels()
{
    PolicyOptions options = AggressiveOptions();
    options.AutoGearing = true;
    options.AutoGearEveryLevels = 5;
    BotPolicy policy(options);
    std::vector<BotSnapshot> bots = { Bot(10, 0, 0, 0) };
    bots[0].level = 3;
    CHECK(Count(policy.Decide(View(1, bots), 1000), ActionType::kAutoGear, 10) == 1);
    CHECK(Count(policy.Decide(View(2, bots), 20000), ActionType::kAutoGear, 10) == 0);
    bots[0].level = 7;
    CHECK(Count(policy.Decide(View(3, bots), 40000), ActionType::kAutoGear, 10) == 0); // 4 levels: not yet
    bots[0].level = 8;
    CHECK(Count(policy.Decide(View(4, bots), 60000), ActionType::kAutoGear, 10) == 1);
}

void WithoutAutoGearingOnlyTheCommandDressesABot()
{
    BotPolicy policy(AggressiveOptions());
    std::vector<BotSnapshot> bots = { Bot(10, 0, 0, 0) };
    CHECK(Count(policy.Decide(View(1, bots), 1000), ActionType::kAutoGear) == 0);
    policy.RequestAutoGear(10);
    CHECK(Count(policy.Decide(View(2, bots), 2000), ActionType::kAutoGear, 10) == 1);
    CHECK(Count(policy.Decide(View(3, bots), 30000), ActionType::kAutoGear, 10) == 0); // once per request
}

// ---- gear from the bags (policy) ----

std::size_t BagGear(std::vector<BotAction> const& actions, std::uint64_t guid)
{
    std::size_t n = 0;
    for (BotAction const& a : actions)
        n += a.type == ActionType::kAutoGear && a.bot_guid == guid && a.spell_id == 2;
    return n;
}

void BotsWearBetterGearFromTheirBagsOnASchedule()
{
    BotPolicy policy; // AutoGearing is off: the bag pass does not depend on it
    std::vector<BotSnapshot> bots = { Bot(10, 0, 0, 0) };
    CHECK(BagGear(policy.Decide(View(1, bots), 1000), 10) == 0);       // the first look only sets the schedule
    CHECK(BagGear(policy.Decide(View(2, bots), 200000), 10) == 0);     // not due yet
    CHECK(BagGear(policy.Decide(View(3, bots), 400000), 10) == 1);     // 5 to 6 minutes after the first look
    CHECK(BagGear(policy.Decide(View(4, bots), 405000), 10) == 0);     // once per period
    CHECK(BagGear(policy.Decide(View(5, bots), 800000), 10) == 1);     // and again
}

void BagGearIsNotAskedForAltsTheDeadOrBotsInACombat()
{
    PolicyOptions options = AggressiveOptions();
    options.IsAlt = [](std::uint64_t guid) { return guid == 11; };
    BotPolicy policy(options);
    std::vector<BotSnapshot> bots = { Bot(10, 0, 0, 0), Bot(11, 0, 0, 0), Bot(12, 0, 0, 0) };
    bots[2].in_combat = 1;
    policy.Decide(View(1, bots), 1000);
    auto const later = policy.Decide(View(2, bots), 400000);
    CHECK(BagGear(later, 10) == 1);
    CHECK(BagGear(later, 11) == 0); // an alt is the player's own character
    CHECK(BagGear(later, 12) == 0); // fighting
    bots[2].in_combat = 0;
    bots[2].life_state = 1;         // dead
    CHECK(BagGear(policy.Decide(View(3, bots), 500000), 12) == 0);
    bots[2].life_state = 0;
    CHECK(BagGear(policy.Decide(View(4, bots), 520000), 12) == 0); // the first look out of combat only sets its schedule
    CHECK(BagGear(policy.Decide(View(5, bots), 900000), 12) == 1); // then it asks like the others
}

void ARequestAsksAtOnceAndOnlyOnce()
{
    BotPolicy policy(AggressiveOptions());
    std::vector<BotSnapshot> bots = { Bot(10, 0, 0, 0) };
    policy.Decide(View(1, bots), 1000);
    policy.RequestBagGear(10);
    CHECK(BagGear(policy.Decide(View(2, bots), 2000), 10) == 1);
    CHECK(BagGear(policy.Decide(View(3, bots), 30000), 10) == 0);
}

void AFullGearPassAndABagPassNeverShareATick()
{
    PolicyOptions options = AggressiveOptions();
    options.AutoGearing = true;
    BotPolicy policy(options);
    std::vector<BotSnapshot> bots = { Bot(10, 0, 0, 0) };
    policy.RequestBagGear(10);
    auto const actions = policy.Decide(View(1, bots), 1000);   // the full pass is due now (a new bot)
    CHECK(Count(actions, ActionType::kAutoGear, 10) == 1);
    CHECK(BagGear(actions, 10) == 0);
}

// ---- casts that cannot work (layout 10) ----

} // namespace

void RunScenarioTestsPart2()
{
    LeaderWalksTheRouteWhileTheRouteIsActive();
    ASubsetViewKeepsTheNearbyUnitsOfItsBots();
    TwoPartiesRunTheirRoutesApartFromEachOther();
    LeaderDoesNotWalkTheDungeonRouteOutsideTheDungeon();
    LeaderDropsARoutePointItHasAlreadyPassed();
    LeaderWaitsForAMemberWhoFellBehind();
    LeaderMovesOnAfterTheFirstPoint();
    TheRoutePartyPullsElitesButGrindingBotsDoNot();
    AStagingBotDoesNotPullWhatDoesNotAttackIt();
    AutoGearingDressesEachBotOnceAndAgainAfterLevels();
    WithoutAutoGearingOnlyTheCommandDressesABot();
    BotsWearBetterGearFromTheirBagsOnASchedule();
    BagGearIsNotAskedForAltsTheDeadOrBotsInACombat();
    ARequestAsksAtOnceAndOnlyOnce();
    AFullGearPassAndABagPassNeverShareATick();
}
