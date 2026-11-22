// SPDX-License-Identifier: GPL-2.0-or-later
// 2026-11-20 (doc/belowmap-noroute-20261120.md): the below-map false positives (the closest floor, the still bot, the places needed, the storm of distinct
// destinations above the bot) and the per-(bot, destination) no-route backoff. Synthetic positions; the Teldrassil, Ironforge and Skyborne ones are the
// samples of arm C.
#include "policy_test_support.h"
#include "below_map.h"
#include "bot_settings.h"
#include "no_route_backoff.h"
#include "route_triage.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace {

using below_map::Verdict;

below_map::Options On()
{
    below_map::Options options;
    options.Enabled = true;
    return options;
}

// A bot standing still at `z` with two known places near it at `groundZ` (one floor).
below_map::Facts Under(float z, float groundZ)
{
    below_map::Facts facts;
    facts.Z = z;
    facts.HasGround = true;
    facts.GroundZ = groundZ;
    facts.NearestDz = std::fabs(groundZ - z);
    facts.Places = 2;
    facts.Floors = 1;
    return facts;
}

std::vector<Verdict> Every(below_map::Options const& options, below_map::Watch& watch, below_map::Facts const& facts, std::uint64_t fromMs, std::uint64_t toMs,
                           std::uint64_t stepMs = 2000)
{
    std::vector<Verdict> verdicts;
    for (std::uint64_t now = fromMs; now <= toMs; now += stepMs)
        verdicts.push_back(below_map::Check(options, watch, facts, now));
    return verdicts;
}

std::size_t Count(std::vector<Verdict> const& verdicts, Verdict verdict)
{
    return std::size_t(std::count(verdicts.begin(), verdicts.end(), verdict));
}

// --- The ground: the closest floor, not the highest -----------------------------------------------------------------------------------------------------

void TheGroundIsTheClosestFloorToTheBot()
{
    // Teldrassil: a giver up the tree (1326) and a place on the ground (1140) beside a bot at 1133: the ground is 1140, two floors.
    below_map::Ground const tree = below_map::FindGround({ 1326.0f, 1140.0f }, 1133.0f, 10.0f);
    CHECK(tree.HasGround && tree.GroundZ == 1140.0f && tree.Index == 1 && tree.NearestDz == 7.0f && tree.Places == 2 && tree.Floors == 2 &&
          tree.LowZ == 1140.0f && tree.HighZ == 1326.0f);
    // A city with three floors (470, 505, 520) and a bot on the lowest: it stands on a floor, not 50 yards under the highest one.
    below_map::Ground const city = below_map::FindGround({ 505.0f, 520.0f, 470.0f }, 471.0f, 10.0f);
    CHECK(city.GroundZ == 470.0f && city.NearestDz == 1.0f && city.Floors == 3);
    // ... and a bot on the top floor is not "above" anything wrong either: the closest is the top one.
    CHECK(below_map::FindGround({ 505.0f, 520.0f, 470.0f }, 522.0f, 10.0f).GroundZ == 520.0f);
    // A bot between two floors at the same distance: the lower one (the floor under it).
    CHECK(below_map::FindGround({ 120.0f, 100.0f }, 110.0f, 10.0f).GroundZ == 100.0f);
    // Two places five yards apart in height are one floor.
    CHECK(below_map::FindGround({ 1326.0f, 1330.0f, 1322.0f }, 1133.0f, 10.0f).Floors == 1);
    // All above (a real fall): the closest floor is the lowest of them, never the highest.
    below_map::Ground const fell = below_map::FindGround({ 505.0f, 560.0f }, 19.0f, 10.0f);
    CHECK(fell.GroundZ == 505.0f && fell.HighZ == 560.0f);
    CHECK(!below_map::FindGround({}, 0.0f, 10.0f).HasGround);
}

// --- z_below: the drop must persist for ten checks of a still bot ---------------------------------------------------------------------------------------

void TheDefaultNeedsTenChecksInARow()
{
    below_map::Options const options = On();
    CHECK(options.BelowChecks == 10 && options.MinPlaces == 2 && options.RequireStill && options.StormDistinct && options.StormRiseYards == 30.0f);
    below_map::Facts const ironforge = Under(19.0f, 505.0f);
    // Every 2 s: the 11th check (20 s after the first) sends.
    below_map::Watch fast;
    std::vector<Verdict> const every2 = Every(options, fast, ironforge, 0, 30000);
    CHECK(every2[9] == Verdict::kWaiting && every2[10] == Verdict::kSendBelow && Count(every2, Verdict::kSendBelow) == 1);
    CHECK(fast.LastChecks == 11);
    // Every 5 s: 20 s are over at the 5th check, the 10th (45 s) sends.
    below_map::Watch slow;
    std::vector<Verdict> const every5 = Every(options, slow, ironforge, 0, 60000, 5000);
    CHECK(every5[8] == Verdict::kWaiting && every5[9] == Verdict::kSendBelow);
    // One check on the floor in between starts it over.
    below_map::Watch broken;
    for (std::uint64_t now = 0; now <= 16000; now += 2000)
        below_map::Check(options, broken, ironforge, now);
    CHECK(below_map::Check(options, broken, Under(500.0f, 505.0f), 18000) == Verdict::kFine && broken.BelowChecks == 0);
    CHECK(below_map::Check(options, broken, ironforge, 20000) == Verdict::kWaiting && broken.BelowChecks == 1);
    // The key: BelowMapChecks 3 with 20 s still needed.
    below_map::Options three = On();
    three.BelowChecks = 3;
    below_map::Watch few;
    std::vector<Verdict> const every10 = Every(three, few, ironforge, 0, 30000, 10000);
    CHECK(every10[1] == Verdict::kWaiting && every10[2] == Verdict::kSendBelow);
}

void AWalkingBotIsNotJudged()
{
    below_map::Options const options = On();
    // A straight-line walk through a hill (the core's fallback with no navmesh path): 80 yards under the places for a minute, walking all the time.
    below_map::Facts walking = Under(1246.0f, 1326.0f);
    walking.Moving = true;
    below_map::Watch watch;
    std::vector<Verdict> const through = Every(options, watch, walking, 0, 60000);
    CHECK(Count(through, Verdict::kSendBelow) == 0 && Count(through, Verdict::kSkipMoving) == through.size());
    CHECK(std::string(below_map::VerdictName(Verdict::kSkipMoving)) == "moving" && below_map::IsSkip(Verdict::kSkipMoving) && watch.LastSkipBelow);
    // It stopped under the ground: from then on the drop must hold 20 s and ten checks.
    below_map::Facts stopped = walking;
    stopped.Moving = false;
    std::vector<Verdict> const after = Every(options, watch, stopped, 62000, 90000);
    CHECK(after[9] == Verdict::kWaiting && after[10] == Verdict::kSendBelow);
    // BelowMapRequireStill 0: a moving bot counts as before.
    below_map::Options counted = On();
    counted.RequireStill = false;
    below_map::Watch old;
    CHECK(Count(Every(counted, old, walking, 0, 30000), Verdict::kSendBelow) == 1);
}

void FlyingFallingAndOnePlaceAreSkipped()
{
    below_map::Options const options = On();
    below_map::Facts const ironforge = Under(19.0f, 505.0f);
    below_map::Facts flying = ironforge;
    flying.Flying = true;
    below_map::Watch air;
    std::vector<Verdict> const flown = Every(options, air, flying, 0, 60000);
    CHECK(Count(flown, Verdict::kSkipFlying) == flown.size() && std::string(below_map::VerdictName(Verdict::kSkipFlying)) == "flying");
    // One giver on a tower within 60 yards is no floor (BelowMapMinPlaces 2); with the key at 1 it counts.
    below_map::Facts lone = ironforge;
    lone.Places = 1;
    below_map::Watch tower;
    std::vector<Verdict> const towers = Every(options, tower, lone, 0, 60000);
    CHECK(Count(towers, Verdict::kSkipFewPlaces) == towers.size() && std::string(below_map::VerdictName(Verdict::kSkipFewPlaces)) == "few_places");
    below_map::Options one = On();
    one.MinPlaces = 1;
    below_map::Watch counted;
    CHECK(Count(Every(one, counted, lone, 0, 30000), Verdict::kSendBelow) == 1);
    // A fall: skipped for 30 s; a fall that lasts (the void under the map) is still recovered, 20 s after the grace.
    below_map::Facts falling = ironforge;
    falling.Falling = true;
    falling.Moving = true;
    below_map::Watch fall;
    std::vector<Verdict> const fell = Every(options, fall, falling, 0, 60000);
    CHECK(fell[0] == Verdict::kSkipFalling && fell[14] == Verdict::kSkipFalling && fell[15] == Verdict::kWaiting && fell[25] == Verdict::kSendBelow);
    // A short fall that lands on the floor is never sent.
    below_map::Watch hop;
    CHECK(Count(Every(options, hop, falling, 0, 10000), Verdict::kSkipFalling) == 6);
    CHECK(below_map::Check(options, hop, Under(500.0f, 505.0f), 12000) == Verdict::kFine && !hop.Falling);
}

void ARealFallIsStillRecovered()
{
    below_map::Options const options = On();
    // The Ironforge sample: z 19 under the auction house at 505, standing still, two places near.
    below_map::Watch watch;
    std::vector<Verdict> const fell = Every(options, watch, Under(19.0f, 505.0f), 0, 900000);
    CHECK(Count(fell, Verdict::kSendBelow) == 2 && fell[10] == Verdict::kSendBelow && Count(fell, Verdict::kCooldown) > 0);
    // On a lower floor of a city with places on it: never.
    below_map::Watch floor;
    CHECK(Count(Every(options, floor, Under(471.0f, 470.0f), 0, 900000), Verdict::kSendBelow) == 0);
}

// --- The storm: distinct destinations above the bot -----------------------------------------------------------------------------------------------------

void TheStormCountsDistinctDestinationsAboveTheBot()
{
    below_map::Options const options = On();
    below_map::Facts open;   // no place near
    // The same refused point retried eight times: one answer.
    below_map::Watch retry;
    for (int i = 0; i < 8; ++i)
        below_map::NoteNoRoute(options, retry, 40.0f, 100.0f, 0.0f, 600.0f, 500.0f, 1000 + std::uint64_t(i) * 1000);
    CHECK(retry.NoRoutes.size() == 1 && below_map::Check(options, retry, open, 9000) == Verdict::kFine);
    below_map::Options any = On();
    any.StormDistinct = false;
    below_map::Watch counted;
    for (int i = 0; i < 8; ++i)
        below_map::NoteNoRoute(any, counted, 40.0f, 100.0f, 0.0f, 600.0f, 500.0f, 1000 + std::uint64_t(i) * 1000);
    CHECK(below_map::Check(any, counted, open, 9000) == Verdict::kSendStorm);
    // Eight explore points at the bot's own height (the explore walk keeps the bot's z): nothing about a fall.
    below_map::Watch explore;
    for (int i = 0; i < 8; ++i)
        below_map::NoteNoRoute(options, explore, 60.0f, float(i) * 30.0f, 0.0f, 500.0f, 500.0f, 1000 + std::uint64_t(i) * 1000);
    CHECK(explore.NoRoutes.empty() && below_map::Check(options, explore, open, 9000) == Verdict::kFine);
    below_map::Options flat = On();
    flat.StormRiseYards = 0.0f;
    below_map::Watch level;
    for (int i = 0; i < 8; ++i)
        below_map::NoteNoRoute(flat, level, 60.0f, float(i) * 30.0f, 0.0f, 500.0f, 500.0f, 1000 + std::uint64_t(i) * 1000);
    CHECK(below_map::Check(flat, level, open, 9000) == Verdict::kSendStorm);
    // A bot under the ground sent to eight places above it: a storm.
    below_map::Watch under;
    for (int i = 0; i < 8; ++i)
        below_map::NoteNoRoute(options, under, 50.0f, float(i) * 30.0f, 0.0f, 505.0f, 19.0f, 1000 + std::uint64_t(i) * 1000);
    CHECK(below_map::Check(options, under, open, 9000) == Verdict::kSendStorm);
}

// --- The policy: the fields of the lines ----------------------------------------------------------------------------------------------------------------

std::shared_ptr<GrindData> MapOneSpots()
{
    std::vector<std::string> warnings;
    std::istringstream spots("spot 1 10311 832 1326 1 6 0 10\nspot 1 10335 870 1330 1 6 0 10\nspot 1 10300 880 1140 1 6 0 10\n"
                             "spot 1 0 0 0 1 6 0 10\nspot 1 100 0 0 1 6 0 10\nspot 1 200 0 0 1 6 0 10\nspot 1 300 0 0 1 6 0 10\n"
                             "spot 1 400 0 0 1 6 0 10\nspot 1 500 0 0 1 6 0 10\nspot 1 600 0 0 1 6 0 10\n");
    return std::make_shared<GrindData>(ParseGrindData(spots, warnings));
}

void ThePolicyLinesCarryTheFloors()
{
    std::vector<std::string> events;
    PolicyOptions options = AggressiveOptions();
    options.Grind = MapOneSpots();
    options.WanderWhenIdle = false;
    options.OnEvent = [&events](std::string const& text) { events.push_back(text); };
    options.BelowMap.Enabled = true;
    BotPolicy policy(options);
    // Under the tree at 1100: the closest floor is the place on the ground (1140), 40 yards up, under the 60 of the drop: nothing happens.
    WorldTickSnapshotView view = View(1, 0, {});
    view.bots[0].map_id = 1;
    view.bots[0].level = 5;
    view.bots[0].pos_x = 10320.0f;
    view.bots[0].pos_y = 860.0f;
    view.bots[0].pos_z = 1100.0f;
    for (std::uint64_t now = 1000; now <= 60000; now += 1000)
        CHECK(!TeleportOf(policy.Decide(view, now), kBot));
    CHECK(events.empty() || std::none_of(events.begin(), events.end(), [](std::string const& e) { return e.rfind("below_map", 0) == 0; }));
    // Walking 150 yards under it (the straight line through the hill): a skip line with the fields, never a teleport.
    view.bots[0].pos_z = 990.0f;
    view.bots[0].moving = 1;
    for (std::uint64_t now = 61000; now <= 200000; now += 1000)
        CHECK(!TeleportOf(policy.Decide(view, now), kBot));
    CHECK(Logged(events, "below_map_skip: bot=42 reason=moving sign=z_below map=1 pos=10320,860,990 ground_z=1140 z=990 drop=150 places=3 floors=2 "
                         "low_z=1140 high_z=1330 checks=0 moving=1 movement_flags=0"));
    // Stopped there: recovered to the closest floor (the ground place), not the tree.
    view.bots[0].moving = 0;
    std::optional<BotAction> up;
    for (std::uint64_t now = 201000; now <= 240000 && !up; now += 1000)
        up = TeleportOf(policy.Decide(view, now), kBot);
    CHECK(up && up->dest_x == 10300.0f && up->dest_z == 1140.0f);
    CHECK(Logged(events, "below_map_recover: bot=42 reason=z_below map=1 pos=10320,860,990 ground_z=1140 z=990 drop=150 places=3 floors=2 low_z=1140 "
                         "high_z=1330 checks=11 moving=0 to=10300,880,1140"));
    // A map the known world does not cover (an instance): never judged.
    std::vector<std::string> none;
    PolicyOptions instance = options;
    instance.OnEvent = [&none](std::string const& text) { none.push_back(text); };
    BotPolicy inside(instance);
    WorldTickSnapshotView dungeon = view;
    dungeon.bots[0].map_id = 36;
    for (std::uint64_t now = 1000; now <= 120000; now += 1000)
        CHECK(!TeleportOf(inside.Decide(dungeon, now), kBot));
    CHECK(std::none_of(none.begin(), none.end(), [](std::string const& e) { return e.rfind("below_map", 0) == 0; }));
}

// --- The no-route backoff -------------------------------------------------------------------------------------------------------------------------------

void TheBackoffDoublesPerPlace()
{
    no_route_backoff::Options options;
    options.Enabled = true;
    CHECK(no_route_backoff::WaitMs(options, 0) == 0 && no_route_backoff::WaitMs(options, 1) == 60000 && no_route_backoff::WaitMs(options, 2) == 120000 &&
          no_route_backoff::WaitMs(options, 5) == 960000 && no_route_backoff::WaitMs(options, 6) == 1800000 && no_route_backoff::WaitMs(options, 60) == 1800000);
    no_route_backoff::Book book;
    no_route_backoff::Entry const* first = no_route_backoff::NoteRefused(options, book, 0, 100.0f, 200.0f, 30.0f, 1000);
    CHECK(first && first->Fails == 1 && first->UntilMs == 61000);
    CHECK(no_route_backoff::Blocked(options, book, 0, 105.0f, 204.0f, 30000));           // the same place, 12 yards wide
    CHECK(!no_route_backoff::Blocked(options, book, 0, 130.0f, 200.0f, 30000));          // another place
    CHECK(!no_route_backoff::Blocked(options, book, 1, 100.0f, 200.0f, 30000));          // another map
    CHECK(!no_route_backoff::Blocked(options, book, 0, 100.0f, 200.0f, 61000));          // the wait is over
    // Two places taken in turn each keep their own count (the single-slot pause started again at 5 s for each).
    no_route_backoff::NoteRefused(options, book, 0, 500.0f, 0.0f, 0.0f, 62000);
    no_route_backoff::Entry const* again = no_route_backoff::NoteRefused(options, book, 0, 100.0f, 200.0f, 30.0f, 63000);
    CHECK(again->Fails == 2 && again->UntilMs == 63000 + 120000 && book.Entries.size() == 2);
    // An applied walk there clears it.
    no_route_backoff::NoteReached(options, book, 0, 101.0f, 199.0f);
    CHECK(!no_route_backoff::Blocked(options, book, 0, 100.0f, 200.0f, 64000) && book.Entries.size() == 1);
    // Forgotten after an hour without a refusal.
    CHECK(no_route_backoff::Blocked(options, book, 0, 500.0f, 0.0f, 100000));
    no_route_backoff::Blocked(options, book, 0, 0.0f, 0.0f, 62000 + 3600001);
    CHECK(book.Entries.empty());
    // At most MaxEntries places per bot, the oldest refusal goes.
    for (int i = 0; i < 20; ++i)
        no_route_backoff::NoteRefused(options, book, 0, float(i) * 100.0f, 0.0f, 0.0f, 1000 + std::uint64_t(i));
    CHECK(book.Entries.size() == 16 && !no_route_backoff::Blocked(options, book, 0, 0.0f, 0.0f, 2000) && no_route_backoff::Blocked(options, book, 0, 1900.0f, 0.0f, 2000));
    // Off: nothing kept, nothing held.
    no_route_backoff::Options off;
    no_route_backoff::Book none;
    CHECK(!no_route_backoff::NoteRefused(off, none, 0, 1.0f, 1.0f, 1.0f, 1) && none.Entries.empty() && !no_route_backoff::Blocked(off, book, 0, 1900.0f, 0.0f, 2000));
}

void TheCallerComesFromTheMoveReason()
{
    CHECK(std::string(route_triage::Caller("state.GrindTarget")) == "grind" && std::string(route_triage::Caller("state.ExploreTarget")) == "explore");
    CHECK(std::string(route_triage::Caller("list[state.NextHub % list.size()]")) == "hub" && std::string(route_triage::Caller("quest_hunt")) == "quest");
    CHECK(std::string(route_triage::Caller("order.Dest")) == "errand" && std::string(route_triage::Caller("dead_walk")) == "death");
    CHECK(std::string(route_triage::Caller("")) == "other" && std::string(route_triage::Caller(nullptr)) == "other" && std::string(route_triage::Caller("new")) == "other");
}

// A bot walking between two hubs it cannot reach: every walk the policy sends is answered "no route" (as the core would).
struct HubRun
{
    int toFirstHub = 0;
    std::vector<std::string> events;
};

HubRun RunUnreachableHubs(bool backoff)
{
    HubRun run;
    PolicyOptions options = AggressiveOptions();
    options.HubsByMap[1] = { Point{ 300.0f, 0.0f, 0.0f } };
    options.NoRouteBackoff.Enabled = backoff;
    options.OnEvent = [&run](std::string const& text) { run.events.push_back(text); };
    BotPolicy policy(options);
    WorldTickSnapshotView view = OnMap(1);
    view.bots[0].pos_x = 200.0f;
    std::uint32_t sequence = 0;
    for (std::uint64_t now = 0; now <= 600000; now += 500)
    {
        std::vector<BotAction> const actions = policy.Decide(view, now);
        float const to = WalkTo(actions, 10);
        if (to < 0.0f)
            continue;
        run.toFirstHub += to == 300.0f;
        view.bots[0].last_action_type = std::uint8_t(ActionType::kMoveTo);
        view.bots[0].last_action_status = BotActionStatus::kNoRoute;
        view.bots[0].last_action_sequence = ++sequence;
    }
    return run;
}

void TheBackoffHoldsARefusedHub()
{
    HubRun const plain = RunUnreachableHubs(false);
    HubRun const held = RunUnreachableHubs(true);
    CHECK(plain.toFirstHub >= 2);
    CHECK(held.toFirstHub >= 1 && held.toFirstHub < plain.toFirstHub);
    bool armed = false;
    for (std::string const& event : held.events)
        armed = armed || (event.rfind("no_route_backoff: bot=10 stage=armed why=", 0) == 0 &&
                          event.find(" caller=hub map=1 dest=300,0,0 dist=100 fails=1 wait_s=60 places=1") != std::string::npos);
    CHECK(armed);
    CHECK(Logged(held.events, "no_route_backoff: bot=10 stage=held why=") && !Logged(plain.events, "no_route_backoff:"));
    // The no_route line says which part of the policy asked for the walk.
    CHECK(Logged(plain.events, "no_route: bot=10 action_type=1 wanted=1 target=0 target_entry=0 dest=300,0,0 cause=to_point"));
    bool caller = false;
    for (std::string const& event : plain.events)
        caller = caller || (event.rfind("no_route: bot=10", 0) == 0 && event.find(" caller=hub dist=100 dz=0 map=1") != std::string::npos);
    CHECK(caller);
    std::printf("hub walks to the refused hub in 10 minutes: %d plain, %d with NoRouteBackoff\n", plain.toFirstHub, held.toFirstHub);
}

void TheNewKeysHaveTheirDefaultsAndBounds()
{
    std::vector<std::string> warnings;
    BotSettings const defaults = ParseBotSettings({}, warnings);
    CHECK(!defaults.BelowMapRecover && defaults.BelowMapChecks == 10 && defaults.BelowMapMinPlaces == 2 && defaults.BelowMapRequireStill &&
          defaults.BelowMapStormRiseYards == 30 && defaults.BelowMapStormDistinct && !defaults.NoRouteBackoff && defaults.NoRouteBackoffSec == 60 && warnings.empty());
    BotSettings const set = ParseBotSettings({ { "BelowMapChecks", "4" }, { "BelowMapMinPlaces", "1" }, { "BelowMapRequireStill", "0" },
                                               { "BelowMapStormRiseYards", "0" }, { "BelowMapStormDistinct", "0" }, { "NoRouteBackoff", "1" },
                                               { "NoRouteBackoffSec", "300" } },
                                             warnings);
    CHECK(set.BelowMapChecks == 4 && set.BelowMapMinPlaces == 1 && !set.BelowMapRequireStill && set.BelowMapStormRiseYards == 0 && !set.BelowMapStormDistinct &&
          set.NoRouteBackoff && set.NoRouteBackoffSec == 300 && warnings.empty());
    BotSettings const bad = ParseBotSettings({ { "BelowMapChecks", "0" }, { "BelowMapMinPlaces", "50" }, { "BelowMapStormRiseYards", "900" },
                                               { "NoRouteBackoffSec", "1" } },
                                             warnings);
    CHECK(bad.BelowMapChecks == 10 && bad.BelowMapMinPlaces == 2 && bad.BelowMapStormRiseYards == 30 && bad.NoRouteBackoffSec == 60 && warnings.size() == 4);
}

} // namespace

int main()
{
    TheGroundIsTheClosestFloorToTheBot();
    TheDefaultNeedsTenChecksInARow();
    AWalkingBotIsNotJudged();
    FlyingFallingAndOnePlaceAreSkipped();
    ARealFallIsStillRecovered();
    TheStormCountsDistinctDestinationsAboveTheBot();
    ThePolicyLinesCarryTheFloors();
    TheBackoffDoublesPerPlace();
    TheCallerComesFromTheMoveReason();
    TheBackoffHoldsARefusedHub();
    TheNewKeysHaveTheirDefaultsAndBounds();
    if (failures == 0)
        std::printf("belowmap_noroute_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
