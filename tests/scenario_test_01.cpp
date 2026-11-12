// SPDX-License-Identifier: GPL-2.0-or-later
// Scenario tests, part 1 of 4 (split out of scenario_test.cpp; the helpers are in scenario_test_support.h).
#include "scenario_test_support.h"

namespace {

void WaitsForTheWholeRoster()
{
    ScenarioDirector director(Plan());
    std::vector<BotAction> actions;
    director.Decide(View(1, { Bot(1, 0, 0, 0), Bot(2, 0, 0, 0) }), 1000, actions);
    CHECK(director.Phase() == ScenarioPhase::Waiting);
    director.Decide(View(2, { Bot(1, 0, 0, 0), Bot(2, 0, 0, 0), Bot(3, 0, 0, 0) }), 2000, actions);
    CHECK(director.Phase() == ScenarioPhase::Dressing);
}

void ADeadBotDoesNotHoldTheRosterForGood()
{
    ScenarioDirector director(Plan());
    std::vector<BotAction> actions;
    std::vector<BotSnapshot> bots = { Bot(1, 0, 0, 0), Bot(2, 0, 0, 0), Bot(3, 0, 0, 0) };
    bots[0].life_state = 1; // died before the roster was complete
    director.Decide(View(1, bots), 1000, actions);
    CHECK(director.Phase() == ScenarioPhase::Waiting);
    director.Decide(View(2, bots), 1000 + kRosterDeadGraceMs - 1, actions);
    CHECK(director.Phase() == ScenarioPhase::Waiting);
    director.Decide(View(3, bots), 1000 + kRosterDeadGraceMs, actions);
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

void NobodyEntersUntilTheWholePartyIsAtStaging()
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
    for (int i = 0; i < 10 && director.Phase() != ScenarioPhase::Entering; ++i)
        step(bots, 3000);
    CHECK(director.Phase() == ScenarioPhase::Entering);
    // A member is still on its way (another map, not fighting): nobody is sent in, not even the leader.
    bots[2].map_id = 1;
    step(bots, kRecoverMaxMs);
    step(bots);
    CHECK(Count(actions, ActionType::kTeleport) == 0);
    // It is at staging and out of combat: the leader and both members go in on the same tick.
    bots[2].map_id = 0;
    step(bots);
    CHECK(Count(actions, ActionType::kTeleport, 1) == 1 && Count(actions, ActionType::kTeleport, 2) == 1 && Count(actions, ActionType::kTeleport, 3) == 1);
}

void AFightAtStagingHoldsThePartyOnlyForTheGrace()
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
    for (int i = 0; i < 10 && director.Phase() != ScenarioPhase::Entering; ++i)
        step(bots, 3000);
    CHECK(director.Phase() == ScenarioPhase::Entering);
    bots[2].in_combat = 1; // one member keeps fighting at staging
    step(bots);
    step(bots);
    CHECK(Count(actions, ActionType::kTeleport) == 0);
    step(bots, kEnterCombatGraceMs);
    // The grace is over: the leader and the free member go in, the fighting one does not (it would be refused).
    CHECK(Count(actions, ActionType::kTeleport, 1) == 1 && Count(actions, ActionType::kTeleport, 2) == 1 && Count(actions, ActionType::kTeleport, 3) == 0);
}

void ADeadMemberOutsideDoesNotHoldTheRun()
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
    for (int i = 0; i < 10 && director.Phase() != ScenarioPhase::Entering; ++i)
        step(bots, 3000);
    CHECK(director.Phase() == ScenarioPhase::Entering);
    bots[0].map_id = 36;
    bots[1].map_id = 36;
    bots[2].life_state = 2; // a ghost at staging
    bots[2].health = 1;
    step(bots);
    CHECK(director.Phase() == ScenarioPhase::Entering);
    step(bots, kEnterStragglersMs);
    CHECK(director.Phase() == ScenarioPhase::Running);
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

// A route that comes back the way it went (2026-11-07 sim: wc parties at point 33 reported furthest=70/96, the Stockade's point 17 is its point 5): a
// point far down the route that lies by an early one is not reached by standing at the early one. Only the next kRouteProgressWindow points count.
ScenarioPlan ThereAndBackPlan()
{
    ScenarioPlan plan = Plan();
    plan.Route.clear();
    for (int i = 0; i < 10; ++i)
        plan.Route.push_back({ 0.0f, 100.0f + 100.0f * float(i), 0.0f }); // out: y 100 .. 1000
    for (int i = 0; i < 10; ++i)
        plan.Route.push_back({ 50.0f, 1000.0f - 100.0f * float(i), 0.0f }); // back, 50 yards to the side: y 1000 .. 100 (the last one by the first)
    plan.Route.back().X = 5.0f;
    return plan;
}

void AnEarlyPointDoesNotReachALaterPointBesideIt()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioDirector director = RunningDirector(t, tick, bots, ThereAndBackPlan());
    std::vector<BotAction> actions;
    bots[0].pos_x = 0.0f;
    bots[0].pos_y = 100.0f; // the leader at the first point, 5 yards from the last one
    director.Decide(View(++tick, bots), t += 1000, actions);
    director.Decide(View(++tick, bots), t += 1000, actions);
    CHECK(director.Phase() == ScenarioPhase::Running); // not "complete" (nor, with bosses, the grace that ends as bosses_missing)
    CHECK(!director.TakeResult());
    for (int i = 1; i < 10; ++i) // out ...
    {
        bots[0].pos_y = 100.0f + 100.0f * float(i);
        director.Decide(View(++tick, bots), t += 1000, actions);
    }
    for (int i = 0; i < 10; ++i) // ... and back
    {
        bots[0].pos_x = i == 9 ? 5.0f : 50.0f;
        bots[0].pos_y = 1000.0f - 100.0f * float(i);
        director.Decide(View(++tick, bots), t += 1000, actions);
    }
    auto const result = director.TakeResult();
    CHECK(result && result->Outcome == "complete" && result->Furthest == 20);
}

// While the leader is up only the leader counts: a member that chases a pack onto a later point does not move the progress.
void AMemberAheadOfALiveLeaderDoesNotMoveTheProgress()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioDirector director = RunningDirector(t, tick, bots);
    std::vector<BotAction> actions;
    bots[1].pos_y = 100.0f;
    director.Decide(View(++tick, bots), t += 1000, actions);
    bots[1].pos_y = 200.0f; // the member reaches the last point; the leader still stands at the entry
    director.Decide(View(++tick, bots), t += 1000, actions);
    CHECK(director.Phase() == ScenarioPhase::Running);
    CHECK(!director.TakeResult());
    bots[0].pos_y = 100.0f;
    director.Decide(View(++tick, bots), t += 1000, actions);
    bots[0].pos_y = 200.0f;
    director.Decide(View(++tick, bots), t += 1000, actions);
    auto const result = director.TakeResult();
    CHECK(result && result->Outcome == "complete" && result->Furthest == 2);
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

} // namespace

void RunScenarioTestsPart1()
{
    WaitsForTheWholeRoster();
    ADeadBotDoesNotHoldTheRosterForGood();
    DressesThenGroupsThenEnters();
    NobodyEntersUntilTheWholePartyIsAtStaging();
    AFightAtStagingHoldsThePartyOnlyForTheGrace();
    ADeadMemberOutsideDoesNotHoldTheRun();
    PartyRestsAtStagingBeforeEntering();
    PartyEntersAfterTheRestCapEvenWhenHurt();
    CompletesWhenTheLeaderReachesTheLastPoint();
    AWaypointAboveIsNotReachedByStandingBelowIt();
    AWaypointAStoreyUpByTheRealShadowfangKeepGapIsNotReached();
    AnEarlyPointDoesNotReachALaterPointBesideIt();
    AMemberAheadOfALiveLeaderDoesNotMoveTheProgress();
    WipeEndsTheRunAndCountsDeaths();
    TimeoutEndsTheRun();
    RaidPlanResultCarriesRaidAndGroups();
    EnteringTimesOut();
}
