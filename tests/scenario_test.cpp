// Unit tests for the scenario director, the leader's route and auto gear (doc 19). No core needed.
#include "bot_policy.h"
#include "bot_settings.h"
#include "scenario.h"

#include <cstdio>
#include <string>

using forever_module::ActionType;
using forever_module::BotAction;
using forever_module::BotSnapshot;
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

std::uint32_t ClassOf(std::uint64_t guid)
{
    return guid == 1 ? 1u : 5u; // bot 1 is the warrior, the rest priests
}

WorldTickSnapshotView View(std::uint64_t tick, std::vector<BotSnapshot> const& bots)
{
    WorldTickSnapshotView view;
    view.tick_number = tick;
    view.max_nearby_units = 1;
    view.bots = bots;
    view.nearby.resize(bots.size());
    return view;
}

BotSnapshot Bot(std::uint64_t guid, std::uint32_t map, float x, float y, std::uint8_t role = 0, std::uint32_t size = 0)
{
    BotSnapshot bot;
    bot.guid = guid;
    bot.health = 1000;
    bot.max_health = 1000;
    bot.map_id = map;
    bot.pos_x = x;
    bot.pos_y = y;
    bot.group_role = role;
    bot.group_size = size;
    return bot;
}

ScenarioPlan Plan()
{
    ScenarioPlan plan;
    plan.Name = "t";
    plan.RosterSize = 3;
    plan.EntryMap = 36;
    plan.Entry = { -14.0f, -385.0f, 62.0f };
    plan.Route = { { 0.0f, 100.0f, 0.0f }, { 0.0f, 200.0f, 0.0f } };
    plan.DurationMs = 100000;
    plan.ClassOf = ClassOf;
    return plan;
}

int Count(std::vector<BotAction> const& actions, ActionType type, std::uint64_t bot = 0)
{
    int n = 0;
    for (BotAction const& action : actions)
        n += action.type == type && (bot == 0 || action.bot_guid == bot);
    return n;
}

int GrantCount(std::vector<BotAction> const& actions, std::uint64_t bot, std::uint32_t spellId)
{
    int n = 0;
    for (BotAction const& action : actions)
        n += action.type == ActionType::kGrant && action.bot_guid == bot && action.spell_id == spellId;
    return n;
}

void WaitsForTheWholeRoster()
{
    ScenarioDirector director(Plan());
    std::vector<BotAction> actions;
    director.Decide(View(1, { Bot(1, 0, 0, 0), Bot(2, 0, 0, 0) }), 1000, actions);
    CHECK(director.Phase() == ScenarioPhase::Waiting);
    director.Decide(View(2, { Bot(1, 0, 0, 0), Bot(2, 0, 0, 0), Bot(3, 0, 0, 0) }), 2000, actions);
    CHECK(director.Phase() == ScenarioPhase::Dressing);
}

void DressesThenGroupsThenEnters()
{
    ScenarioDirector director(Plan());
    std::vector<BotAction> actions;
    std::uint64_t t = 1000, tick = 1;
    auto step = [&](std::vector<BotSnapshot> const& bots, std::uint64_t advance = 100)
    {
        actions.clear();
        t += advance;
        director.Decide(View(++tick, bots), t, actions);
    };
    std::vector<BotSnapshot> bots = { Bot(1, 0, 0, 0), Bot(2, 0, 0, 0), Bot(3, 0, 0, 0) };
    step(bots);                                            // roster complete
    CHECK(director.Phase() == ScenarioPhase::Dressing);
    step(bots);                                            // auto gear for everyone, the warrior as a tank
    CHECK(Count(actions, ActionType::kAutoGear) == 3);
    for (BotAction const& action : actions)
        CHECK(action.spell_id == (action.bot_guid == 1 ? 1u : 0u));
    step(bots, kDressPauseMs);                             // the equip service
    CHECK(Count(actions, ActionType::kVendor) == 3);
    step(bots, kEquipPauseMs);
    CHECK(director.Phase() == ScenarioPhase::Grouping);

    // Not grouped yet: nothing happens. Grouped: the leader (role 2) teleports.
    step(bots);
    CHECK(director.Phase() == ScenarioPhase::Grouping);
    bots = { Bot(1, 0, 0, 0, 2, 3), Bot(2, 0, 0, 0, 1, 3), Bot(3, 0, 0, 0, 1, 3) };
    step(bots);
    CHECK(director.Phase() == ScenarioPhase::Entering);
    step(bots);
    // Nobody is in combat: the whole party is sent in together, the leader and the members alike.
    CHECK(Count(actions, ActionType::kTeleport, 1) == 1 && Count(actions, ActionType::kTeleport, 2) == 1 && Count(actions, ActionType::kTeleport, 3) == 1);

    // The leader arrives: the members follow after the delay.
    bots[0].map_id = 36;
    step(bots);
    CHECK(Count(actions, ActionType::kTeleport) == 0);
    step(bots, kMembersDelayMs);
    CHECK(Count(actions, ActionType::kTeleport, 2) == 1 && Count(actions, ActionType::kTeleport, 3) == 1);

    // Everyone there: running.
    for (BotSnapshot& bot : bots)
        bot.map_id = 36;
    step(bots);
    CHECK(director.Phase() == ScenarioPhase::Running && director.RouteActive());
}

void PartyRestsAtStagingBeforeEntering()
{
    ScenarioDirector director(Plan());
    std::vector<BotAction> actions;
    std::uint64_t t = 1000, tick = 1;
    auto step = [&](std::vector<BotSnapshot> const& bots, std::uint64_t advance = 100)
    {
        actions.clear();
        t += advance;
        director.Decide(View(++tick, bots), t, actions);
    };
    std::vector<BotSnapshot> bots = { Bot(1, 0, 0, 0, 2, 3), Bot(2, 0, 0, 0, 1, 3), Bot(3, 0, 0, 0, 1, 3) };
    for (int i = 0; i < 4; ++i)
        step(bots, 3000);                                  // waiting, dressing, equip
    for (int i = 0; i < 6 && director.Phase() != ScenarioPhase::Entering; ++i)
        step(bots, 3000);
    CHECK(director.Phase() == ScenarioPhase::Entering);
    // Gear raised the maximum health: the party stands at 40 %. Nothing is sent while it rests.
    for (BotSnapshot& bot : bots)
        bot.health = 400;
    step(bots, 5000);
    CHECK(Count(actions, ActionType::kTeleport) == 0);
    bots[1].health = 1000;
    bots[2].health = 1000;                                  // two are healed, the tank is not
    step(bots, 5000);
    CHECK(Count(actions, ActionType::kTeleport) == 0);
    bots[0].health = 950;                                   // 95 %: ready
    bots[0].max_power = 1000;
    bots[0].power = 300;                                    // a mana user at 30 % mana still has to drink
    step(bots, 5000);
    CHECK(Count(actions, ActionType::kTeleport) == 0);
    bots[0].power = 900;
    step(bots, 5000);
    CHECK(Count(actions, ActionType::kTeleport, 1) == 1);   // the leader enters
}

void PartyEntersAfterTheRestCapEvenWhenHurt()
{
    ScenarioDirector director(Plan());
    std::vector<BotAction> actions;
    std::uint64_t t = 1000, tick = 1;
    std::vector<BotSnapshot> bots = { Bot(1, 0, 0, 0, 2, 3), Bot(2, 0, 0, 0, 1, 3), Bot(3, 0, 0, 0, 1, 3) };
    for (BotSnapshot& bot : bots)
        bot.health = 300;
    for (int i = 0; i < 30 && director.Phase() != ScenarioPhase::Entering; ++i)
        director.Decide(View(++tick, bots), t += 3000, actions);
    CHECK(director.Phase() == ScenarioPhase::Entering);
    std::uint64_t const enteredAt = t;
    actions.clear();
    director.Decide(View(++tick, bots), enteredAt + kRecoverMaxMs - 5000, actions);
    CHECK(Count(actions, ActionType::kTeleport) == 0);       // still resting
    director.Decide(View(++tick, bots), enteredAt + kRecoverMaxMs + 1000, actions);
    CHECK(Count(actions, ActionType::kTeleport, 1) == 1);    // the cap is over: it goes
}

ScenarioDirector RunningDirector(std::uint64_t& t, std::uint64_t& tick, std::vector<BotSnapshot>& bots, ScenarioPlan plan = Plan())
{
    ScenarioDirector director(plan);
    std::vector<BotAction> actions;
    bots = { Bot(1, 36, 0, 0, 2, 3), Bot(2, 36, 0, 0, 1, 3), Bot(3, 36, 0, 0, 1, 3) };
    t = 1000;
    tick = 1;
    for (int i = 0; i < 4; ++i)
        director.Decide(View(++tick, bots), t += 3000, actions); // waiting -> dressing -> equip
    for (int i = 0; i < 6 && director.Phase() != ScenarioPhase::Running; ++i)
        director.Decide(View(++tick, bots), t += 3000, actions);
    return director;
}

void CompletesWhenTheLeaderReachesTheLastPoint()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioDirector director = RunningDirector(t, tick, bots);
    CHECK(director.Phase() == ScenarioPhase::Running);
    std::vector<BotAction> actions;
    bots[0].pos_y = 100.0f;
    director.Decide(View(++tick, bots), t += 1000, actions);
    CHECK(director.Phase() == ScenarioPhase::Running);
    bots[0].pos_y = 200.0f;
    director.Decide(View(++tick, bots), t += 1000, actions);
    auto const result = director.TakeResult();
    CHECK(result && result->Outcome == "complete" && result->Furthest == 2 && result->RouteSize == 2);
    CHECK(!director.TakeResult()); // only once
}

// Shadowfang Keep's own bug (2026-10-27): a route that climbs a tower can put a later waypoint almost over an earlier one in X/Y, a storey
// up. A bot standing under it (fighting the first pack, nowhere near the stairs) must not be credited with "reached" a waypoint that is
// really a storey above it. The numbers here are the file's own (route points 4 and 36: 4.5 yards apart in X/Y, 9.3 yards in Z) -- the
// closest real pair in 33-shadowfang-keep.json, and why kRouteReachMaxZYards is 8, not 10 (which this pair would still have passed).
void AWaypointAboveIsNotReachedByStandingBelowIt()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioPlan plan = Plan();
    plan.Route = { { 0.0f, 100.0f, 0.0f }, { 2.0f, 101.0f, 40.0f } }; // same spot in X/Y, 40 yards up: another floor
    ScenarioDirector director = RunningDirector(t, tick, bots, plan);
    std::vector<BotAction> actions;
    bots[0].pos_y = 100.0f; // the leader reaches the first point, on the ground
    bots[0].pos_z = 0.0f;
    director.Decide(View(++tick, bots), t += 1000, actions);
    director.Decide(View(++tick, bots), t += 1000, actions); // still standing there, still on the ground: no stair climbed
    CHECK(director.Phase() == ScenarioPhase::Running);       // not "complete": the upper point is not reached
    auto const result = director.TakeResult();
    CHECK(!result);
}

// The same bug, but with the tight real-world numbers that actually caused it (Shadowfang Keep's own route points 4 and 36): only 4.5
// yards apart in X/Y and 9.3 in Z, just under the old kRouteReachMaxZYards of 10, which this pair would still have falsely "reached".
void AWaypointAStoreyUpByTheRealShadowfangKeepGapIsNotReached()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioPlan plan = Plan();
    plan.Route = { { -233.1f, 2148.8f, 90.8f }, { -236.7f, 2146.1f, 100.1f } }; // route points 4 and 36 of 33-shadowfang-keep.json
    ScenarioDirector director = RunningDirector(t, tick, bots, plan);
    std::vector<BotAction> actions;
    bots[0].pos_x = -233.1f;
    bots[0].pos_y = 2148.8f; // the leader reaches the first point, at the courtyard gate
    bots[0].pos_z = 90.8f;
    director.Decide(View(++tick, bots), t += 1000, actions);
    director.Decide(View(++tick, bots), t += 1000, actions); // still at the gate, nowhere near the tower roof
    CHECK(director.Phase() == ScenarioPhase::Running);       // not "complete": the roof point (a storey up) is not reached
    auto const result = director.TakeResult();
    CHECK(!result);
}

void WipeEndsTheRunAndCountsDeaths()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioDirector director = RunningDirector(t, tick, bots);
    std::vector<BotAction> actions;
    bots[1].health = 0;
    bots[1].life_state = 1;
    director.Decide(View(++tick, bots), t += 1000, actions);
    CHECK(director.Phase() == ScenarioPhase::Running);
    for (BotSnapshot& bot : bots)
    {
        bot.health = 0;
        bot.life_state = 1;
    }
    director.Decide(View(++tick, bots), t += 1000, actions);
    auto const result = director.TakeResult();
    CHECK(result && result->Outcome == "wipe" && result->Deaths == 3 && result->Alive == 0);
}

void TimeoutEndsTheRun()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioDirector director = RunningDirector(t, tick, bots);
    std::vector<BotAction> actions;
    director.Decide(View(++tick, bots), t += 200000, actions);
    auto const result = director.TakeResult();
    CHECK(result && result->Outcome == "timeout");
    CHECK(FormatScenarioResult(*result).find("outcome=timeout") != std::string::npos);
}

// 40-man raid support (doc 108): a raid plan's result carries Raid/Groups through to the log line; a non-raid plan's does not.
void RaidPlanResultCarriesRaidAndGroups()
{
    ScenarioPlan raidPlan = Plan();
    raidPlan.Raid = true;
    raidPlan.GroupSubgroups = 8;
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioDirector director = RunningDirector(t, tick, bots, raidPlan);
    std::vector<BotAction> actions;
    director.Decide(View(++tick, bots), t += 200000, actions);
    auto const result = director.TakeResult();
    CHECK(result && result->Raid && result->Groups == 8);
    CHECK(FormatScenarioResult(*result).find("raid=1 groups=8") != std::string::npos);
}

void EnteringTimesOut()
{
    ScenarioDirector director(Plan());
    std::vector<BotAction> actions;
    std::vector<BotSnapshot> bots = { Bot(1, 0, 0, 0, 2, 3), Bot(2, 0, 0, 0, 1, 3), Bot(3, 0, 0, 0, 1, 3) };
    std::uint64_t t = 1000, tick = 1;
    for (int i = 0; i < 6; ++i)
        director.Decide(View(++tick, bots), t += 3000, actions);
    CHECK(director.Phase() == ScenarioPhase::Entering);
    director.Decide(View(++tick, bots), t += kEnterTimeoutMs + 1000, actions);
    auto const result = director.TakeResult();
    CHECK(result && result->Outcome == "enter_failed");
}

// ---- the leader's route (policy) ----

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

WorldTickSnapshotView MageFacingAMob(BotSnapshot bot)
{
    WorldTickSnapshotView view;
    view.tick_number = 1;
    view.max_nearby_units = 2;
    bot.nearby_count = 1;
    bot.threat_target_guid = forever_module::MakeUnitRef(8, 6, 1);
    view.bots.push_back(bot);
    view.nearby.resize(2);
    view.nearby[0].guid = forever_module::MakeUnitRef(8, 6, 1);
    view.nearby[0].pos_x = 20.0f;
    view.nearby[0].health = 100;
    view.nearby[0].max_health = 100;
    view.nearby[0].is_hostile = true;
    return view;
}

BotPolicy MagePolicy()
{
    PolicyOptions options = AggressiveOptions();
    options.ClassId = 8;
    return BotPolicy(options);
}

void NoCastWhileAnotherCastRuns()
{
    BotSnapshot bot = Bot(10, 0, 0, 0);
    bot.power = bot.max_power = 1000;
    BotPolicy policy = MagePolicy();
    CHECK(Count(policy.Decide(MageFacingAMob(bot), 1000), ActionType::kCastSpell, 10) == 1); // standing and idle: it casts
    BotPolicy busy = MagePolicy();
    bot.casting_spell = 133;
    bot.cast_remaining_ms = 1500;
    CHECK(Count(busy.Decide(MageFacingAMob(bot), 1000), ActionType::kCastSpell, 10) == 0);
}

void StopsBeforeASpellWithACastTimeWhileMoving()
{
    BotSnapshot bot = Bot(10, 0, 0, 0);
    bot.power = bot.max_power = 1000;
    bot.moving = 1;
    BotPolicy policy = MagePolicy();
    auto const actions = policy.Decide(MageFacingAMob(bot), 1000);
    CHECK(Count(actions, ActionType::kCastSpell, 10) == 0);
    CHECK(Count(actions, ActionType::kStop, 10) >= 1);
    bot.moving = 0;
    CHECK(Count(policy.Decide(MageFacingAMob(bot), 2600), ActionType::kCastSpell, 10) == 1); // standing: the cast follows
}

void RefusedAsMovingIsLearnedAndRetriedSoon()
{
    BotSnapshot bot = Bot(10, 0, 0, 0);
    bot.power = bot.max_power = 1000;
    BotPolicy policy = MagePolicy();
    auto first = policy.Decide(MageFacingAMob(bot), 1000);
    std::uint32_t spell = 0;
    for (BotAction const& action : first)
        if (action.type == ActionType::kCastSpell)
            spell = action.spell_id;
    CHECK(spell != 0);

    bot.last_action_type = std::uint8_t(ActionType::kCastSpell);
    bot.last_action_status = forever_module::BotActionStatus::kBusy;
    bot.last_action_spell = spell;
    bot.last_action_sequence = 1;
    CHECK(Count(policy.Decide(MageFacingAMob(bot), 1100), ActionType::kCastSpell, 10) == 0); // retry waits 300 ms
    bot.last_action_sequence = 1;
    // The global cooldown (1.5 s) has passed too, and the 300 ms busy retry long ago: the cast is tried again.
    CHECK(Count(policy.Decide(MageFacingAMob(bot), 3000), ActionType::kCastSpell, 10) == 1);
}

// ---- settings ----

void SettingsReadAScenario()
{
    std::unordered_map<std::string, std::string> config = {
        { "Scenario", "dm" },           { "ScenarioRoster", "1:1,1:5,7:8" }, { "ScenarioLevel", "18" },
        { "ScenarioStaging", "0 -8913 554 94" }, { "ScenarioEntry", "36 -14.5 -385.4 62.4" }, { "ScenarioDurationSec", "600" },
        { "ScenarioRoute2", "0 200 0" }, { "ScenarioRoute1", "0 100 0" }, { "ScenarioGear5", "2092, 1234" },
        { "AutoGearing", "1" },
    };
    std::vector<std::string> warnings;
    BotSettings const settings = ParseBotSettings(config, warnings);
    CHECK(settings.AutoGearing);
    ScenarioSettings const& s = settings.Scenario;
    CHECK(s.Name == "dm" && s.Roster.size() == 3 && s.Roster[2][0] == 7 && s.Roster[2][1] == 8);
    CHECK(s.Level == 18 && s.DurationSec == 600 && s.HasStaging && s.HasEntry);
    CHECK(s.Entry[0] == 36.0f && s.Staging[0] == 0.0f);
    CHECK(s.Route.size() == 2 && s.Route[0][1] == 100.0f && s.Route[1][1] == 200.0f); // in the order of n
    CHECK(s.Gear.count(5) == 1);
}

void SettingsDropAScenarioWithoutARoster()
{
    std::unordered_map<std::string, std::string> config = { { "Scenario", "dm" } };
    std::vector<std::string> warnings;
    BotSettings const settings = ParseBotSettings(config, warnings);
    CHECK(settings.Scenario.Name.empty());
    CHECK(!warnings.empty());
}

} // namespace


void CountsBossesSeenDead()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioPlan plan = Plan();
    plan.Bosses = { { 644, "Rhahk'Zor" }, { 646, "Mr. Smite" } };
    plan.RaceOf = [](std::uint64_t guid) { return guid == 1 ? 2u : 5u; };
    ScenarioDirector director(plan);
    std::vector<BotAction> actions;
    bots = { Bot(1, 36, 0, 0, 2, 3), Bot(2, 36, 0, 0, 1, 3), Bot(3, 36, 0, 0, 1, 3) };
    t = 1000;
    tick = 1;
    for (int i = 0; i < 12 && director.Phase() != ScenarioPhase::Running; ++i)
        director.Decide(View(++tick, bots), t += 3000, actions);
    CHECK(director.Phase() == ScenarioPhase::Running);

    // Rhahk'Zor's corpse is in bot 2's view (health 0); a living Mr. Smite does not count.
    WorldTickSnapshotView view = View(++tick, bots);
    view.bots[1].nearby_count = 1;
    view.bots[2].nearby_count = 1;
    view.nearby[1].guid = forever_module::MakeUnitRef(8, 644, 5);
    view.nearby[1].health = 0;
    view.nearby[2].guid = forever_module::MakeUnitRef(8, 646, 6);
    view.nearby[2].health = 500;
    director.Decide(view, t += 1000, actions);
    auto const fresh = director.TakeNewBossKills();
    CHECK(fresh.size() == 1 && fresh[0] == "Rhahk'Zor");
    CHECK(director.TakeNewBossKills().empty());

    bots[0].health = bots[1].health = bots[2].health = 0;
    bots[0].life_state = bots[1].life_state = bots[2].life_state = 1; // a wipe ends it
    director.Decide(View(++tick, bots), t += 1000, actions);
    auto const result = director.TakeResult();
    CHECK(result && result->BossesTotal == 2 && result->BossesKilled.size() == 1);
    CHECK(FormatScenarioResult(*result).find("bosses=1/2 killed=Rhahk'Zor") != std::string::npos);
    CHECK(FormatScenarioResult(*result).find("killed=Rhahk'Zor | roster=2:1,5:5,5:5 classes=1,5,5 combo=1-5-5") != std::string::npos);
}

void Down(BotSnapshot& bot, std::uint8_t life = 1)
{
    bot.life_state = life;
    bot.health = life == 2 ? 1 : 0; // a ghost has health 1
}

void Up(BotSnapshot& bot)
{
    bot.life_state = 0;
    bot.health = 1000;
}

void RevivedPartyOutsideIsPutBackAtTheEntry()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioDirector director = RunningDirector(t, tick, bots);
    std::vector<BotAction> actions;
    // After a wipe the leader and one member are up at the graveyard on map 0; one member is still a ghost there.
    for (BotSnapshot& bot : bots)
        bot.map_id = 0;
    Down(bots[2], 2);
    director.Decide(View(++tick, bots), t += 1000, actions);
    CHECK(Count(actions, ActionType::kTeleport) == 0);                 // not at once: they get up first
    actions.clear();
    director.Decide(View(++tick, bots), t += kReenterDelayMs, actions);
    CHECK(Count(actions, ActionType::kTeleport, 1) == 1 && Count(actions, ActionType::kTeleport, 2) == 1);
    CHECK(Count(actions, ActionType::kTeleport, 3) == 0);              // the ghost is not teleported
    for (BotAction const& action : actions)
        CHECK(action.spell_id == 36 && action.dest_x == -14.0f);
    actions.clear();
    director.Decide(View(++tick, bots), t += 1000, actions);
    CHECK(Count(actions, ActionType::kTeleport) == 0);                 // and not again before the retry time
    bots[0].map_id = 36;
    bots[1].map_id = 36;
    actions.clear();
    director.Decide(View(++tick, bots), t += kEnterRetryMs, actions);
    CHECK(Count(actions, ActionType::kTeleport) == 0);                 // they are back inside
}

void GhostCountsAsNotAlive()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioDirector director = RunningDirector(t, tick, bots);
    std::vector<BotAction> actions;
    for (BotSnapshot& bot : bots)
        Down(bot, 2);
    director.Decide(View(++tick, bots), t += 1000, actions);
    auto const result = director.TakeResult();
    CHECK(result && result->Outcome == "wipe" && result->Alive == 0 && result->Ghosts == 3);
}

void SingleDeathKeepsTheRunGoing()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioDirector director = RunningDirector(t, tick, bots);
    std::vector<BotAction> actions;
    Down(bots[1]);
    director.Decide(View(++tick, bots), t += 1000, actions);
    CHECK(director.Phase() == ScenarioPhase::Running);
}

void DeadLeaderDoesNotFreezeFurthest()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioDirector director = RunningDirector(t, tick, bots);
    std::vector<BotAction> actions;
    Down(bots[0]);
    bots[1].pos_y = 100.0f; // a living member reaches the first point
    director.Decide(View(++tick, bots), t += 1000, actions);
    bots[1].pos_y = 200.0f; // and the last
    director.Decide(View(++tick, bots), t += 1000, actions);
    auto const result = director.TakeResult();
    CHECK(result && result->Outcome == "complete");
}

void DeathEventsCountARepeatDeathTwice()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioDirector director = RunningDirector(t, tick, bots);
    std::vector<BotAction> actions;
    Down(bots[1]);
    director.Decide(View(++tick, bots), t += 1000, actions);
    Up(bots[1]);
    director.Decide(View(++tick, bots), t += 1000, actions);
    Down(bots[1], 2);
    director.Decide(View(++tick, bots), t += 1000, actions);
    Up(bots[1]);
    director.Decide(View(++tick, bots), t += 1000, actions);
    bots[0].pos_y = 200.0f;
    director.Decide(View(++tick, bots), t += 1000, actions);
    auto const result = director.TakeResult();
    CHECK(result && result->DeathEvents == 2 && result->Revived == 2 && result->Deaths == 1); // Deaths: bots that died at least once
}

void RevivedBotIsCountedOnceInDeaths()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioDirector director = RunningDirector(t, tick, bots);
    std::vector<BotAction> actions;
    Down(bots[2]);
    director.Decide(View(++tick, bots), t += 1000, actions);
    Up(bots[2]);
    bots[0].pos_y = 200.0f;
    director.Decide(View(++tick, bots), t += 1000, actions);
    auto const result = director.TakeResult();
    CHECK(result && result->Deaths == 1 && result->Alive == 3);
}

ScenarioPlan RecoveryPlan()
{
    ScenarioPlan plan = Plan();
    plan.WipeRecovery = true;
    plan.WipeRecoveryMs = 60000;
    return plan;
}

void WipeRecoveredByAReclaimingGhost()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioDirector director = RunningDirector(t, tick, bots, RecoveryPlan());
    std::vector<BotAction> actions;
    for (BotSnapshot& bot : bots)
        Down(bot, 2);
    director.Decide(View(++tick, bots), t += 1000, actions);
    CHECK(director.Phase() == ScenarioPhase::Running); // not over yet
    director.Decide(View(++tick, bots), t += 30000, actions);
    CHECK(director.Phase() == ScenarioPhase::Running);
    Up(bots[1]);
    director.Decide(View(++tick, bots), t += 1000, actions);
    Up(bots[0]);
    bots[0].pos_y = 200.0f;
    director.Decide(View(++tick, bots), t += 1000, actions);
    auto const result = director.TakeResult();
    CHECK(result && result->Outcome == "complete" && result->Wipes == 1);
}

void WipeRecoveryTimesOutToWipe()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioDirector director = RunningDirector(t, tick, bots, RecoveryPlan());
    std::vector<BotAction> actions;
    for (BotSnapshot& bot : bots)
        Down(bot, 2);
    director.Decide(View(++tick, bots), t += 1000, actions);
    director.Decide(View(++tick, bots), t += 61000, actions);
    auto const result = director.TakeResult();
    CHECK(result && result->Outcome == "wipe" && result->Wipes == 0);
}

void UnderstrengthEndsTheRun()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioPlan plan = Plan();
    plan.MinAlive = 2;
    plan.DurationMs = 10000000;
    ScenarioDirector director = RunningDirector(t, tick, bots, plan);
    std::vector<BotAction> actions;
    Down(bots[1]);
    Down(bots[2], 2);
    director.Decide(View(++tick, bots), t += 1000, actions);
    CHECK(director.Phase() == ScenarioPhase::Running);
    director.Decide(View(++tick, bots), t += kUnderstrengthGraceMs - 1000, actions);
    CHECK(director.Phase() == ScenarioPhase::Running);
    director.Decide(View(++tick, bots), t += 2000, actions);
    auto const result = director.TakeResult();
    CHECK(result && result->Outcome == "understrength");
}

void DeadLeaderAbandonsAfterTheCap()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioPlan plan = Plan();
    plan.LeaderDeadAbandonMs = 120000;
    plan.DurationMs = 10000000;
    ScenarioDirector director = RunningDirector(t, tick, bots, plan);
    std::vector<BotAction> actions;
    Down(bots[0], 2);
    director.Decide(View(++tick, bots), t += 1000, actions);
    director.Decide(View(++tick, bots), t += 100000, actions);
    CHECK(director.Phase() == ScenarioPhase::Running);
    director.Decide(View(++tick, bots), t += 30000, actions);
    auto const result = director.TakeResult();
    CHECK(result && result->Outcome == "abandoned_dead_leader");
}

void EnteringSkipsGhostsAndCorpses()
{
    ScenarioDirector director(Plan());
    std::vector<BotAction> actions;
    std::vector<BotSnapshot> bots = { Bot(1, 36, 0, 0, 2, 3), Bot(2, 0, 0, 0, 1, 3), Bot(3, 0, 0, 0, 1, 3) };
    Down(bots[1], 2);
    std::uint64_t t = 1000, tick = 1;
    for (int i = 0; i < 20; ++i)
        director.Decide(View(++tick, bots), t += 3000, actions);
    for (BotAction const& action : actions)
        CHECK(!(action.type == ActionType::kTeleport && action.bot_guid == 2));
}

void FormatShowsEventsRevivedWipes()
{
    ScenarioResult result;
    result.DeathEvents = 4;
    result.Revived = 2;
    result.Wipes = 1;
    result.Ghosts = 1;
    std::string const text = FormatScenarioResult(result);
    CHECK(text.find("events=4") != std::string::npos && text.find("revived=2") != std::string::npos);
    CHECK(text.find("wipes=1") != std::string::npos && text.find("ghosts=1") != std::string::npos);
}

// 40-man raid support (doc 108): the result line carries "raid=1 groups=8" for a raid scenario, and nothing extra (dungeon
// result lines unchanged) when the scenario is not a raid.
void FormatShowsRaidAndGroupsOnlyForARaidScenario()
{
    ScenarioResult result;
    result.Outcome = "complete";
    result.Raid = true;
    result.Groups = 8;
    std::string const raidText = FormatScenarioResult(result);
    CHECK(raidText.find("raid=1 groups=8") != std::string::npos);

    ScenarioResult dungeon;
    dungeon.Outcome = "complete";
    std::string const dungeonText = FormatScenarioResult(dungeon);
    CHECK(dungeonText.find("raid=") == std::string::npos && dungeonText.find("groups=") == std::string::npos);
}


// --- Ammunition, class supplies, class quests and the Taming phase ---

std::uint32_t HunterWarlockOrPriest(std::uint64_t guid)
{
    return guid == 1 ? 3u : guid == 2 ? 9u : 5u; // bot 1 is a hunter, bot 2 a warlock, bot 3 a priest
}

void Reach(ScenarioDirector& director, std::vector<BotSnapshot>& bots, std::uint64_t& t, std::uint64_t& tick, std::vector<BotAction>& actions)
{
    auto const step = [&](std::uint64_t advance)
    {
        actions.clear();
        t += advance;
        director.Decide(View(++tick, bots), t, actions);
    };
    step(100);   // the roster is complete
    step(100);   // the dressing: auto gear
}

void AHunterWithoutAmmunitionIsGivenSome()
{
    ScenarioPlan plan = Plan();
    plan.ClassOf = HunterWarlockOrPriest;
    ScenarioDirector director(plan);
    std::vector<BotAction> actions;
    std::uint64_t t = 1000, tick = 1;
    std::vector<BotSnapshot> bots = { Bot(1, 0, 0, 0), Bot(2, 0, 0, 0), Bot(3, 0, 0, 0) };
    bots[0].ammo_entry = 2515;   // the core's recommended arrow for its bow; none carried
    bots[0].ammo_count = 0;
    Reach(director, bots, t, tick, actions);
    int grants = 0;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kGrant && action.bot_guid == 1 && action.spell_id == 2515 && action.dest_x == 200.0f)
            ++grants;
    CHECK(grants == 1);
    // Not again at once (the snapshot has not shown it yet), again after the retry gap, never once it carries enough.
    actions.clear();
    director.Decide(View(++tick, bots), t += 100, actions);
    CHECK(Count(actions, ActionType::kGrant, 1) == 0);
    actions.clear();
    director.Decide(View(++tick, bots), t += kAmmoRetryMs, actions);
    CHECK(Count(actions, ActionType::kGrant, 1) == 1);
    bots[0].ammo_count = 1000;
    actions.clear();
    director.Decide(View(++tick, bots), t += kAmmoRetryMs, actions);
    CHECK(Count(actions, ActionType::kGrant, 1) == 0);
    // A plan with no ammunition count: nothing.
    ScenarioPlan none = plan;
    none.AmmoCount = 0;
    ScenarioDirector quiet(none);
    std::vector<BotSnapshot> hungry = { Bot(1, 0, 0, 0), Bot(2, 0, 0, 0), Bot(3, 0, 0, 0) };
    hungry[0].ammo_entry = 2515;
    std::uint64_t t2 = 1000, tick2 = 1;
    Reach(quiet, hungry, t2, tick2, actions);
    CHECK(Count(actions, ActionType::kGrant, 1) == 0);
    // A bot whose weapon takes no ammunition (ammo_entry 0) is given none.
    ScenarioDirector melee(plan);
    std::vector<BotSnapshot> plain = { Bot(1, 0, 0, 0), Bot(2, 0, 0, 0), Bot(3, 0, 0, 0) };
    std::uint64_t t3 = 1000, tick3 = 1;
    Reach(melee, plain, t3, tick3, actions);
    CHECK(Count(actions, ActionType::kGrant) == 0);
}

void AnAmmunitionGrantIsOneStackAtMostAndStopsWhenItDoesNothing()
{
    // The core refuses a grant above the item's stack size (200 for arrows and shot): 0.33.0 asked for 1000 and 164 grants were refused.
    ScenarioPlan plan = Plan();
    plan.ClassOf = HunterWarlockOrPriest;
    plan.AmmoCount = 1000;
    ScenarioDirector director(plan);
    std::vector<BotAction> actions;
    std::uint64_t t = 1000, tick = 1;
    std::vector<BotSnapshot> bots = { Bot(1, 0, 0, 0), Bot(2, 0, 0, 0), Bot(3, 0, 0, 0) };
    bots[0].ammo_entry = 2516;
    bots[0].ammo_count = 0;
    Reach(director, bots, t, tick, actions);
    bool stack = false;
    for (BotAction const& action : actions)
        stack = stack || (action.type == ActionType::kGrant && action.bot_guid == 1 && action.spell_id == 2516 && action.dest_x == 200.0f);
    CHECK(stack);
    // Refused every time (the count never rises): three grants, then the bot is left alone.
    int grants = 1;
    for (int i = 0; i < 12; ++i)
    {
        actions.clear();
        director.Decide(View(++tick, bots), t += kAmmoRetryMs, actions);
        grants += Count(actions, ActionType::kGrant, 1);
    }
    CHECK(grants >= 3 && grants <= 4);
    // A grant that works keeps the hunter supplied: the count rises, so the next low count is topped up again.
    ScenarioDirector working(plan);
    std::vector<BotSnapshot> good = { Bot(1, 0, 0, 0), Bot(2, 0, 0, 0), Bot(3, 0, 0, 0) };
    good[0].ammo_entry = 2516;
    std::uint64_t t2 = 1000, tick2 = 1;
    Reach(working, good, t2, tick2, actions);
    int given = Count(actions, ActionType::kGrant, 1);
    for (int i = 0; i < 6; ++i)
    {
        good[0].ammo_count = 200;                       // the grant landed
        actions.clear();
        working.Decide(View(++tick2, good), t2 += kAmmoRetryMs, actions);
        given += Count(actions, ActionType::kGrant, 1);
        good[0].ammo_count = 20;                        // and the hunter shot most of it
        actions.clear();
        working.Decide(View(++tick2, good), t2 += kAmmoRetryMs, actions);
        given += Count(actions, ActionType::kGrant, 1);
    }
    CHECK(given >= 6);
}

// --- Supplies (food index 0, drink index 1): the dressing's first grant, then kept topped up like ammunition ---

void ASupplyBelowHalfItsCountIsRegrantedOneAboveItIsNot()
{
    ScenarioPlan plan = Plan();
    plan.Supplies = { { 4540, 10 }, { 1205, 6 } }; // food (Tough Jerky), drink (Ice Cold Milk), by their doc order
    ScenarioDirector director(plan);
    std::vector<BotAction> actions;
    std::uint64_t t = 1000, tick = 1;
    std::vector<BotSnapshot> bots = { Bot(1, 0, 0, 0), Bot(2, 0, 0, 0), Bot(3, 0, 0, 0) };
    Reach(director, bots, t, tick, actions);
    // The dressing's first grant: both items, at their configured count.
    bool food = false, drink = false;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kGrant && action.bot_guid == 1)
        {
            food = food || (action.spell_id == 4540 && action.dest_x == 10.0f);
            drink = drink || (action.spell_id == 1205 && action.dest_x == 6.0f);
        }
    CHECK(food && drink);
    // Plenty of food (10, half of 10 is 5: not low), almost no drink (1, half of 6 is 3: low): only the drink is topped up again.
    bots[0].food_count = 10;
    bots[0].drink_count = 1;
    actions.clear();
    director.Decide(View(++tick, bots), t += kSupplyRetryMs, actions);
    CHECK(GrantCount(actions, 1, 4540) == 0);
    CHECK(GrantCount(actions, 1, 1205) == 1);
    // Flipped: almost no food, plenty of drink: only the food is topped up.
    bots[0].food_count = 2;
    bots[0].drink_count = 6;
    actions.clear();
    director.Decide(View(++tick, bots), t += kSupplyRetryMs, actions);
    CHECK(GrantCount(actions, 1, 4540) == 1);
    CHECK(GrantCount(actions, 1, 1205) == 0);
    // Plenty of both: neither.
    bots[0].food_count = 10;
    bots[0].drink_count = 6;
    actions.clear();
    director.Decide(View(++tick, bots), t += kSupplyRetryMs, actions);
    CHECK(Count(actions, ActionType::kGrant, 1) == 0);
}

void ASupplyGrantThatNeverRaisesItsCountStopsAfterTheUselessCapAndDoesNotAffectTheOtherItem()
{
    // Food never rises (the core keeps refusing it: a full bag of it, say): it stops being retried after kSupplyUselessMax in a row, the
    // same way ammunition does. Drink, granted alongside it on the same bot, rises each time and falls again (as if drunk) and keeps being
    // topped up the whole time: food's cap does not touch it.
    ScenarioPlan plan = Plan();
    plan.Supplies = { { 4540, 10 }, { 1205, 6 } };
    ScenarioDirector director(plan);
    std::vector<BotAction> actions;
    std::uint64_t t = 1000, tick = 1;
    std::vector<BotSnapshot> bots = { Bot(1, 0, 0, 0), Bot(2, 0, 0, 0), Bot(3, 0, 0, 0) };
    Reach(director, bots, t, tick, actions);
    int food = GrantCount(actions, 1, 4540);
    int drink = GrantCount(actions, 1, 1205);
    CHECK(food == 1 && drink == 1);
    for (int i = 0; i < 6; ++i)
    {
        bots[0].drink_count = 6; // the previous grant landed: not low, nothing sent this tick
        actions.clear();
        director.Decide(View(++tick, bots), t += kSupplyRetryMs, actions);
        food += GrantCount(actions, 1, 4540);
        drink += GrantCount(actions, 1, 1205);
        bots[0].drink_count = 1; // and most of it is drunk again: low, topped back up
        actions.clear();
        director.Decide(View(++tick, bots), t += kSupplyRetryMs, actions);
        food += GrantCount(actions, 1, 4540);
        drink += GrantCount(actions, 1, 1205);
    }
    CHECK(food == 3);  // the dressing's grant plus two retries, then the useless cap leaves it alone for the rest of the run
    CHECK(drink == 7); // the dressing's grant plus once per loop, never stopped
}

void ClassSuppliesAndClassQuestsAreSentOnceEach()
{
    ScenarioPlan plan = Plan();
    plan.ClassOf = HunterWarlockOrPriest;
    plan.ClassSupplies[9] = { { 6265, 10 } };          // Soul Shards for the warlock
    plan.ClassQuests[9] = { 1504, 1474 };               // the Voidwalker and Succubus quests
    ScenarioDirector director(plan);
    std::vector<BotAction> actions;
    std::uint64_t t = 1000, tick = 1;
    std::vector<BotSnapshot> bots = { Bot(1, 0, 0, 0), Bot(2, 0, 0, 0), Bot(3, 0, 0, 0) };
    int shards = 0, quest1504 = 0, quest1474 = 0, other = 0;
    for (int i = 0; i < 8; ++i)
    {
        actions.clear();
        director.Decide(View(++tick, bots), t += 100, actions);
        for (BotAction const& action : actions)
        {
            if (action.type == ActionType::kGrant && action.bot_guid == 2 && action.spell_id == 6265 && action.dest_x == 10.0f)
                ++shards;
            else if (action.type == ActionType::kQuest && action.bot_guid == 2 && action.spell_id == ((4u << 24) | 1504u))
                ++quest1504;
            else if (action.type == ActionType::kQuest && action.bot_guid == 2 && action.spell_id == ((4u << 24) | 1474u))
                ++quest1474;
            else if ((action.type == ActionType::kGrant || action.type == ActionType::kQuest) && action.bot_guid != 2)
                ++other;
        }
    }
    CHECK(shards == 1 && quest1504 == 1 && quest1474 == 1 && other == 0);
}

ScenarioPlan TamingPlan()
{
    ScenarioPlan plan = Plan();
    plan.ClassOf = HunterWarlockOrPriest;
    plan.TameEnabled = true;
    plan.TameMap = 0;
    plan.TameSpot = { -10439.0f, 929.0f, 39.0f };
    return plan;
}

void IntoTaming(ScenarioDirector& director, std::vector<BotSnapshot>& bots, std::uint64_t& t, std::uint64_t& tick, std::vector<BotAction>& actions)
{
    for (int i = 0; i < 12 && director.Phase() != ScenarioPhase::Taming && director.Phase() != ScenarioPhase::Grouping; ++i)
    {
        actions.clear();
        director.Decide(View(++tick, bots), t += 3000, actions);
    }
}

void HuntersWithoutAPetAreTakenToTameOneAndBack()
{
    ScenarioDirector director(TamingPlan());
    std::vector<BotAction> actions;
    std::uint64_t t = 1000, tick = 1;
    std::vector<BotSnapshot> bots = { Bot(1, 0, -8913, 554), Bot(2, 0, -8913, 554), Bot(3, 0, -8913, 554) };
    for (BotSnapshot& bot : bots)
        bot.level = 20;
    IntoTaming(director, bots, t, tick, actions);
    CHECK(director.Phase() == ScenarioPhase::Taming && director.Taming());
    // The hunter is sent to the spot, the others stay.
    actions.clear();
    director.Decide(View(++tick, bots), t += 100, actions);
    int away = 0;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kTeleport)
            away += action.bot_guid == 1 && action.spell_id == 0 && action.dest_x == -10439.0f && action.dest_y == 929.0f;
    CHECK(away == 1 && Count(actions, ActionType::kTeleport) == 1);
    // It arrives, spends a while; nothing more is sent, the phase stays.
    bots[0].pos_x = -10439.0f;
    bots[0].pos_y = 929.0f;
    actions.clear();
    director.Decide(View(++tick, bots), t += 30000, actions);
    CHECK(director.Phase() == ScenarioPhase::Taming && Count(actions, ActionType::kTeleport) == 0);
    // It has a pet: back to where it stood, and then the grouping.
    bots[0].pet_state = 1;
    actions.clear();
    director.Decide(View(++tick, bots), t += 1000, actions);
    int back = 0;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kTeleport)
            back += action.bot_guid == 1 && action.dest_x == -8913.0f && action.dest_y == 554.0f;
    CHECK(back == 1);
    bots[0].pos_x = -8913.0f;
    bots[0].pos_y = 554.0f;
    actions.clear();
    director.Decide(View(++tick, bots), t += 1000, actions);
    CHECK(director.Phase() == ScenarioPhase::Grouping);
}

void TheTamingPhaseGivesUpAtTheTimeout()
{
    ScenarioDirector director(TamingPlan());
    std::vector<BotAction> actions;
    std::uint64_t t = 1000, tick = 1;
    std::vector<BotSnapshot> bots = { Bot(1, 0, -8913, 554), Bot(2, 0, -8913, 554), Bot(3, 0, -8913, 554) };
    for (BotSnapshot& bot : bots)
        bot.level = 20;
    IntoTaming(director, bots, t, tick, actions);
    CHECK(director.Phase() == ScenarioPhase::Taming);
    bots[0].pos_x = -10439.0f;                  // there, and never gets a pet
    bots[0].pos_y = 929.0f;
    actions.clear();
    director.Decide(View(++tick, bots), t += kTameTimeoutMs - 5000, actions);
    CHECK(director.Phase() == ScenarioPhase::Taming);
    actions.clear();
    director.Decide(View(++tick, bots), t += 7000, actions);      // the timeout: sent back
    CHECK(Count(actions, ActionType::kTeleport, 1) == 1);
    actions.clear();
    director.Decide(View(++tick, bots), t += kTameBackMs, actions);   // not back yet, but the grouping goes on
    CHECK(director.Phase() == ScenarioPhase::Grouping);
}

void NoTamingWithoutAHunterThatNeedsAPetOrWithoutASpot()
{
    // A hunter that has a pet: straight to the grouping.
    {
        ScenarioDirector director(TamingPlan());
        std::vector<BotAction> actions;
        std::uint64_t t = 1000, tick = 1;
        std::vector<BotSnapshot> bots = { Bot(1, 0, 0, 0), Bot(2, 0, 0, 0), Bot(3, 0, 0, 0) };
        for (BotSnapshot& bot : bots)
            bot.level = 20;
        bots[0].pet_state = 1;
        IntoTaming(director, bots, t, tick, actions);
        CHECK(director.Phase() == ScenarioPhase::Grouping);
    }
    // No tame spot in the plan: a hunter with no pet is not sent anywhere.
    {
        ScenarioPlan plan = TamingPlan();
        plan.TameEnabled = false;
        ScenarioDirector director(plan);
        std::vector<BotAction> actions;
        std::uint64_t t = 1000, tick = 1;
        std::vector<BotSnapshot> bots = { Bot(1, 0, 0, 0), Bot(2, 0, 0, 0), Bot(3, 0, 0, 0) };
        for (BotSnapshot& bot : bots)
            bot.level = 20;
        IntoTaming(director, bots, t, tick, actions);
        CHECK(director.Phase() == ScenarioPhase::Grouping);
    }
    // A hunter below the pet level is not sent either.
    {
        ScenarioDirector director(TamingPlan());
        std::vector<BotAction> actions;
        std::uint64_t t = 1000, tick = 1;
        std::vector<BotSnapshot> bots = { Bot(1, 0, 0, 0), Bot(2, 0, 0, 0), Bot(3, 0, 0, 0) };
        for (BotSnapshot& bot : bots)
            bot.level = 9;
        IntoTaming(director, bots, t, tick, actions);
        CHECK(director.Phase() == ScenarioPhase::Grouping);
    }
}

void SettingsReadRosterSpecsClassSuppliesAndTheTameSpot()
{
    std::unordered_map<std::string, std::string> config = {
        { "Scenario", "dm" }, { "ScenarioRoster", "1:1:prot,1:5,3:3:bm,7:9:demonology" }, { "ScenarioTameSpot", "0 -10439 929 39" }, { "ScenarioEntry", "36 -14.5 -385.4 62.4" },
        { "ScenarioClassSupplies", "9:6265:10,5:17026:5" }, { "ScenarioClassQuests", "9:1504,3:6061" }, { "ScenarioAmmo", "600" },
    };
    std::vector<std::string> warnings;
    BotSettings const settings = ParseBotSettings(config, warnings);
    ScenarioSettings const& s = settings.Scenario;
    CHECK(s.Roster.size() == 4 && s.RosterSpec.size() == 4);
    CHECK(s.RosterSpec[0] == "prot" && s.RosterSpec[1].empty() && s.RosterSpec[2] == "bm" && s.RosterSpec[3] == "demonology");
    CHECK(s.Roster[2][0] == 3 && s.Roster[2][1] == 3 && s.Roster[3][1] == 9);
    CHECK(s.HasTameSpot && s.TameSpot[0] == 0.0f && s.TameSpot[1] == -10439.0f && s.TameSpot[3] == 39.0f);
    CHECK(s.ClassSupplies.size() == 2 && s.ClassSupplies.at(9)[0].first == 6265 && s.ClassSupplies.at(9)[0].second == 10 && s.ClassSupplies.at(5)[0].first == 17026);
    CHECK(s.ClassQuests.size() == 2 && s.ClassQuests.at(9)[0] == 1504 && s.ClassQuests.at(3)[0] == 6061);
    CHECK(s.Ammo == 600);
    CHECK(warnings.empty());

    // A broken spec is dropped with a warning; the defaults give a warlock shards and send the pet quests; a default BotSettings loads data/rotations.txt.
    std::unordered_map<std::string, std::string> broken = { { "Scenario", "dm" }, { "ScenarioRoster", "1:1:Prot!,1:5" }, { "ScenarioEntry", "36 -14.5 -385.4 62.4" } };
    std::vector<std::string> brokenWarnings;
    BotSettings const second = ParseBotSettings(broken, brokenWarnings);
    CHECK(second.Scenario.Roster.size() == 1 && brokenWarnings.size() == 1);
    CHECK(second.Scenario.ClassSupplies.count(9) == 1 && second.Scenario.ClassSupplies.at(9)[0].first == 6265);
    CHECK(second.Scenario.ClassQuests.empty() && second.Scenario.Ammo == 200);   // class quests are opt-in: the core refuses them until it adds the quest first
    CHECK(second.RotationFile == "data/rotations.txt");
    std::unordered_map<std::string, std::string> off = { { "RotationFile", "" } };
    std::vector<std::string> offWarnings;
    CHECK(ParseBotSettings(off, offWarnings).RotationFile.empty());

    ScenarioSlot const slot = ScenarioSlotOf({ s }, 2);
    CHECK(slot.Valid && slot.Class == 3 && slot.Spec == "bm");
    CHECK(ScenarioSlotOf({ s }, 1).Spec.empty());
}

void FormatAppendsTheRosterAfterTheBosses()
{
    ScenarioResult result;
    result.Name = "rfc#2";
    result.Outcome = "timeout";
    result.BossesTotal = 4;
    result.BossesKilled = { "Oggleflint", "Taragaman the Hungerer" };
    result.Party = { { 2, 1 }, { 5, 5 }, { 8, 8 }, { 2, 9 }, { 5, 4 } };
    std::string const text = FormatScenarioResult(result);
    CHECK(text.find("killed=Oggleflint,Taragaman the Hungerer | roster=2:1,5:5,8:8,2:9,5:4 classes=1,4,5,8,9 combo=1-4-5-8-9") != std::string::npos);
    CHECK(text.find(" | ") == text.rfind(" | "));
    result.Party.clear();
    CHECK(FormatScenarioResult(result).find(" | ") == std::string::npos);
}

int main()
{
    AHunterWithoutAmmunitionIsGivenSome();
    AnAmmunitionGrantIsOneStackAtMostAndStopsWhenItDoesNothing();
    ASupplyBelowHalfItsCountIsRegrantedOneAboveItIsNot();
    ASupplyGrantThatNeverRaisesItsCountStopsAfterTheUselessCapAndDoesNotAffectTheOtherItem();
    ClassSuppliesAndClassQuestsAreSentOnceEach();
    HuntersWithoutAPetAreTakenToTameOneAndBack();
    TheTamingPhaseGivesUpAtTheTimeout();
    NoTamingWithoutAHunterThatNeedsAPetOrWithoutASpot();
    SettingsReadRosterSpecsClassSuppliesAndTheTameSpot();
    CountsBossesSeenDead();
    WaitsForTheWholeRoster();
    DressesThenGroupsThenEnters();
    CompletesWhenTheLeaderReachesTheLastPoint();
    AWaypointAboveIsNotReachedByStandingBelowIt();
    AWaypointAStoreyUpByTheRealShadowfangKeepGapIsNotReached();
    WipeEndsTheRunAndCountsDeaths();
    GhostCountsAsNotAlive();
    SingleDeathKeepsTheRunGoing();
    DeadLeaderDoesNotFreezeFurthest();
    DeathEventsCountARepeatDeathTwice();
    RevivedBotIsCountedOnceInDeaths();
    WipeRecoveredByAReclaimingGhost();
    WipeRecoveryTimesOutToWipe();
    UnderstrengthEndsTheRun();
    DeadLeaderAbandonsAfterTheCap();
    EnteringSkipsGhostsAndCorpses();
    FormatShowsEventsRevivedWipes();
    FormatShowsRaidAndGroupsOnlyForARaidScenario();
    RaidPlanResultCarriesRaidAndGroups();
    FormatAppendsTheRosterAfterTheBosses();
    TimeoutEndsTheRun();
    EnteringTimesOut();
    PartyRestsAtStagingBeforeEntering();
    ASubsetViewKeepsTheNearbyUnitsOfItsBots();
    TwoPartiesRunTheirRoutesApartFromEachOther();
    LeaderDoesNotWalkTheDungeonRouteOutsideTheDungeon();
    LeaderDropsARoutePointItHasAlreadyPassed();
    RevivedPartyOutsideIsPutBackAtTheEntry();
    PartyEntersAfterTheRestCapEvenWhenHurt();
    LeaderWalksTheRouteWhileTheRouteIsActive();
    LeaderWaitsForAMemberWhoFellBehind();
    LeaderMovesOnAfterTheFirstPoint();
    TheRoutePartyPullsElitesButGrindingBotsDoNot();
    AutoGearingDressesEachBotOnceAndAgainAfterLevels();
    WithoutAutoGearingOnlyTheCommandDressesABot();
    BotsWearBetterGearFromTheirBagsOnASchedule();
    BagGearIsNotAskedForAltsTheDeadOrBotsInACombat();
    ARequestAsksAtOnceAndOnlyOnce();
    AFullGearPassAndABagPassNeverShareATick();
    NoCastWhileAnotherCastRuns();
    StopsBeforeASpellWithACastTimeWhileMoving();
    RefusedAsMovingIsLearnedAndRetriedSoon();
    SettingsReadAScenario();
    SettingsDropAScenarioWithoutARoster();
    if (failures == 0)
        std::printf("scenario_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
