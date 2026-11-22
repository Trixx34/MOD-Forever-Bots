// SPDX-License-Identifier: GPL-2.0-or-later
// Tests of the three small fixes from the sim arm C read-out (2026-11-19, branch fix/arm-c-small-20261119): the level-safe line throttle
// (LevelSafeLogThrottleSec), the z-aware auction stand point (AuctionWalk3D) and the below-map false positives (BelowMapRecover). The positions are the
// samples of the arm C log.
#include "policy_test_support.h"
#include "bot_settings.h"
#include "auction_policy.h"
#include "below_map.h"
#include "level_safe.h"
#include "off_map.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace {

// --- (1) The level-safe line throttle -------------------------------------------------------------------------------------------------------------------

// Bot 225 of arm C: 4,655 quest_walk_blocked reason=level_unsafe lines, the same block every turn. One line per bot, quest and reason per period.
void TheLevelSafeLineIsThrottledPerBotQuestAndReason()
{
    level_safe::LogThrottle throttle;
    int written = 0;
    for (std::uint64_t now = 0; now < 120000; now += 100)   // a block every 100 ms for 2 minutes
        written += throttle.Due(225, 7, "level_unsafe", now, 60000) ? 1 : 0;
    CHECK(written == 2);                                     // at 0 and at 60 s
    CHECK(throttle.Held() == 1198);
    CHECK(throttle.Due(225, 8, "level_unsafe", 1000, 60000));   // another quest: its own line
    CHECK(throttle.Due(226, 7, "level_unsafe", 1000, 60000));   // another bot
    CHECK(throttle.Due(225, 7, "no_spot", 1000, 60000));        // another reason
    CHECK(throttle.TakeFields() == " level_safe_lines=5 level_safe_held=1198");
    CHECK(throttle.TakeFields() == " level_safe_lines=0 level_safe_held=0");   // the counts start again
    level_safe::LogThrottle every;
    for (std::uint64_t now = 0; now < 1000; now += 100)
        CHECK(every.Due(225, 7, "level_unsafe", now, 0));        // 0 = every line
    // Bounded: entries older than the period are dropped once many are kept.
    level_safe::LogThrottle many;
    for (std::uint64_t bot = 0; bot < 5000; ++bot)
        many.Due(bot, 1, "level_unsafe", 0, 60000);
    many.Due(99999, 1, "level_unsafe", 120000, 60000);
    CHECK(many.Size() < 5000);
}

std::shared_ptr<GrindData> LineSpots()
{
    std::vector<std::string> warnings;
    std::istringstream spots("spot 0 0 0 0 1 6 0 10\n"
                             "spot 0 100 0 0 2 6 0 10\n"
                             "spot 0 300 0 0 1 6 0 10\n"
                             "spot 0 2000 0 0 24 26 0 10\n"
                             "spot 0 2100 0 0 23 27 0 10\n");
    return std::make_shared<GrindData>(ParseGrindData(spots, warnings));
}

// The policy writes the first line and counts it in the hourly line; the block itself does not change.
void TheBlockStaysAndTheHourlyLineCountsTheLines()
{
    std::istringstream in("start test\nanchor 0 100 0 0\nquest 1 giver 2000 0 0 ender 2000 0 0\n");
    std::vector<std::string> warnings;
    std::vector<std::string> events;
    PolicyOptions options = AggressiveOptions();
    options.Grind = LineSpots();
    options.WanderWhenIdle = false;
    options.OnEvent = [&events](std::string const& text) { events.push_back(text); };
    options.Quests = std::make_shared<QuestData>(ParseQuestData(in, warnings));
    options.LevelSafe.Walks = true;
    options.DeathsTopEveryMs = 60000;
    BotPolicy policy(options);
    CHECK(!policy.TakeDeathsTopLine(500));
    bool walkedHigh = false;
    for (std::uint64_t now = 1000; now < 8000; now += 1000)
    {
        WorldTickSnapshotView view = View(1, 0, {});
        view.bots[0].level = 6;
        for (BotAction const& action : policy.Decide(view, now))
            walkedHigh = walkedHigh || (action.type == ActionType::kMoveTo && action.dest_x > 1000.0f);
    }
    CHECK(!walkedHigh);
    CHECK(LoggedWith(events, "quest_walk_blocked: bot=42 map=0 quest=1 reason=level_unsafe"));
    std::optional<std::string> const line = policy.TakeDeathsTopLine(61000);
    CHECK(line && line->find(" level_safe_lines=1 level_safe_held=0 below_map_recover=0 below_map_skip=none") != std::string::npos);
}

// --- (2) The z-aware auction stand point ----------------------------------------------------------------------------------------------------------------

void TheAuctioneerHeightIsJudgedBeforeItIsTrusted()
{
    using auction::StandZ;
    // Arm C sample 1: 2D 3.7 yards, 3D 14.2: bot_z -43.1, auctioneer_z -56.8. The vendors near the house stand on the auctioneer's floor: its z is used.
    std::vector<auction::RefPoint> const house{ { 20.0f, 0.0f, -55.0f }, { 400.0f, 0.0f, 90.0f } };
    auction::StandJudgement const lower = auction::JudgeStandZ(3.7f, 0.0f, -56.8f, std::nullopt, house);
    CHECK(lower.Verdict == StandZ::kUse3D && lower.RefKnown && lower.RefZ == -55.0f);
    CHECK(!auction::InReachJudged(lower.Verdict, true, 0.0f, 0.0f, -43.1f, 3.7f, 0.0f, -56.8f));   // in 2D reach, not in 3D: the walk goes on
    CHECK(auction::InReachJudged(lower.Verdict, true, 1.0f, 0.0f, -56.0f, 3.7f, 0.0f, -56.8f));    // on the auctioneer's floor
    CHECK(std::string(auction::ReachCheckName(lower.Verdict, false)) == "failed_3d" && std::string(auction::ReachCheckName(lower.Verdict, true)) == "ok_3d");
    // Arm C sample 2: bot_z 7.4, auctioneer_z 505.2 (Ironforge: the house floor is at about 505, so the bot is the one below the map). With the city's
    // vendors at about 501 the z is good and is used; the bot cannot be in reach from below.
    std::vector<auction::RefPoint> const city{ { 30.0f, 10.0f, 501.0f } };
    auction::StandJudgement const ironforge = auction::JudgeStandZ(1.9f, 0.0f, 505.2f, std::nullopt, city);
    CHECK(ironforge.Verdict == StandZ::kUse3D);
    CHECK(!auction::InReachJudged(ironforge.Verdict, true, 0.0f, 0.0f, 7.4f, 1.9f, 0.0f, 505.2f));
    // Bad data: the stored z lies more than 60 yards from the ground of the vendors near it: skipped.
    std::vector<auction::RefPoint> const ground{ { 30.0f, 10.0f, 7.0f } };
    auction::StandJudgement const bad = auction::JudgeStandZ(1.9f, 0.0f, 505.2f, std::nullopt, ground);
    CHECK(bad.Verdict == StandZ::kSkip && std::string(auction::StandZName(bad.Verdict)) == "skip_bad_z");
    CHECK(!auction::InReachJudged(bad.Verdict, true, 1.9f, 0.0f, 505.2f, 1.9f, 0.0f, 505.2f));     // never in reach
    // A reference farther than 200 yards is not near the auctioneer: nothing to compare with.
    auction::StandJudgement const far = auction::JudgeStandZ(0.0f, 0.0f, 505.2f, std::nullopt, { { 300.0f, 0.0f, 7.0f } });
    CHECK(far.Verdict == StandZ::kUse3D && !far.RefKnown);
    // A ground height at the auctioneer (a probe, not wired today): 40 yards or more off, the z is not trusted and the 2D reach counts.
    CHECK(auction::JudgeStandZ(0.0f, 0.0f, 505.2f, 480.0f, {}).Verdict == StandZ::kUse3D);
    auction::StandJudgement const drifted = auction::JudgeStandZ(0.0f, 0.0f, 505.2f, 400.0f, {});
    CHECK(drifted.Verdict == StandZ::kIgnoreZ && auction::InReachJudged(drifted.Verdict, true, 2.0f, 0.0f, 7.4f, 0.0f, 0.0f, 505.2f));
    CHECK(std::string(auction::ReachCheckName(StandZ::kIgnoreZ, false)) == "ignore_z");
}

struct ErrandRun
{
    std::vector<std::string> events;
    std::vector<BotAction> lastMoves;
    ErrandOutcome outcome = ErrandOutcome::kNone;
};

// One bot on an auction errand with AuctionWalk3D, the snapshot built by `shape`, ticks every second from 1 s to `untilMs`.
template <typename Shape>
ErrandRun RunAuctionErrand(Point dest, Shape shape, std::uint64_t untilMs, bool walk3d = true)
{
    ErrandRun run;
    PolicyOptions options;
    options.AuctionWalk3D = walk3d;
    options.OnEvent = [&run](std::string const& text) { run.events.push_back(text); };
    BotPolicy policy(options);
    WorldTickSnapshotView view = View(1, 0, {});
    shape(view.bots[0]);
    policy.Decide(view, 500);
    ErrandOrder order;
    order.Map = 0;
    order.Dest = dest;
    order.Auction = true;
    policy.StartErrand(kBot, order);
    for (std::uint64_t now = 1000; now <= untilMs; now += 1000)
    {
        for (BotAction const& action : policy.Decide(view, now))
            if (action.bot_guid == kBot && action.type == ActionType::kMoveTo)
                run.lastMoves.push_back(action);
        if (ErrandOutcome const outcome = policy.TakeErrandOutcome(kBot); outcome != ErrandOutcome::kNone)
        {
            run.outcome = outcome;
            break;
        }
    }
    return run;
}

void TheAuctionErrandWalksToTheAuctioneersOwnPlace()
{
    // The live auctioneer of the snapshot replaces the errand's spot (taken when the errand started, from farther away): its height is the walk's.
    ErrandRun const live = RunAuctionErrand(Point{ 100.0f, 0.0f, 0.0f }, [](BotSnapshot& bot)
    {
        bot.auctioneer_valid = 1;
        bot.auctioneer_x = 110.0f;
        bot.auctioneer_z = -56.8f;
        bot.vendor_valid = 1;
        bot.vendor_x = 120.0f;
        bot.vendor_z = -55.0f;
    }, 3000);
    CHECK(!live.lastMoves.empty() && live.lastMoves.back().dest_x == 110.0f && live.lastMoves.back().dest_z == -56.8f);
    // Arm C sample 1: in 2D reach (3.7 yards), 13.7 yards above the auctioneer. The walk goes on; after 60 s the errand fails with one line.
    ErrandRun const above = RunAuctionErrand(Point{ 3.7f, 0.0f, -56.8f }, [](BotSnapshot& bot)
    {
        bot.pos_z = -43.1f;
        bot.auctioneer_valid = 1;
        bot.auctioneer_x = 3.7f;
        bot.auctioneer_z = -56.8f;
        bot.vendor_valid = 1;
        bot.vendor_x = 20.0f;
        bot.vendor_z = -55.0f;
    }, 120000);
    CHECK(!above.lastMoves.empty() && above.lastMoves.back().dest_z == -56.8f);
    CHECK(above.outcome == ErrandOutcome::kFailed);
    CHECK(Logged(above.events, "auction_reach: bot=42 check=failed_3d where=errand walk3d=1 dist2d=3.7 dist3d=14.2 bot_z=-43.1 auctioneer_z=-56.8 ref_z=-55"));
    CHECK(Logged(above.events, "errand_failed: bot=42 reason=auction_reach_3d"));
    // On the auctioneer's floor: arrived (ok_3d), stands its 25 s, done.
    ErrandRun const there = RunAuctionErrand(Point{ 3.7f, 0.0f, -56.8f }, [](BotSnapshot& bot)
    {
        bot.pos_x = 1.0f;
        bot.pos_z = -56.0f;
        bot.auctioneer_valid = 1;
        bot.auctioneer_x = 3.7f;
        bot.auctioneer_z = -56.8f;
    }, 60000);
    CHECK(there.outcome == ErrandOutcome::kDone && there.lastMoves.empty());
    CHECK(Logged(there.events, "auction_reach: bot=42 check=ok_3d where=errand") && LoggedWith(there.events, "ref_z=none"));
    // Bad data: the auctioneer's z 505.2 with the vendors beside it at 7: no walk, the errand fails at once.
    ErrandRun const bad = RunAuctionErrand(Point{ 1.9f, 0.0f, 505.2f }, [](BotSnapshot& bot)
    {
        bot.pos_z = 7.4f;
        bot.auctioneer_valid = 1;
        bot.auctioneer_x = 1.9f;
        bot.auctioneer_z = 505.2f;
        bot.vendor_valid = 1;
        bot.vendor_x = 30.0f;
        bot.vendor_z = 7.0f;
    }, 10000);
    CHECK(bad.outcome == ErrandOutcome::kFailed && bad.lastMoves.empty());
    CHECK(Logged(bad.events, "auction_reach: bot=42 check=skip_bad_z where=errand walk3d=1 dist2d=1.9 dist3d=497.8 bot_z=7.4 auctioneer_z=505.2 ref_z=7"));
    CHECK(Logged(bad.events, "errand_failed: bot=42 reason=auctioneer_bad_z"));
    // AuctionWalk3D off: the 2D reach as before (sample 1 counts as arrived), no auction_reach line from the errand.
    ErrandRun const off = RunAuctionErrand(Point{ 3.7f, 0.0f, -56.8f }, [](BotSnapshot& bot)
    {
        bot.pos_z = -43.1f;
    }, 60000, false);
    CHECK(off.outcome == ErrandOutcome::kDone && !LoggedWith(off.events, "auction_reach:"));
}

// --- (3) Below-map false positives ----------------------------------------------------------------------------------------------------------------------

below_map::Options BelowOn()
{
    below_map::Options options;
    options.Enabled = true;
    options.BelowChecks = 5;   // the arm C value; the 2026-11-20 default (10) is tested in belowmap_noroute_test.cpp
    return options;
}

// The verdicts of checks every 2 s from `fromMs` to `toMs`, as the policy makes them.
std::vector<below_map::Verdict> Checks(below_map::Options const& options, below_map::Watch& watch, below_map::Facts const& facts, std::uint64_t fromMs,
                                       std::uint64_t toMs)
{
    std::vector<below_map::Verdict> verdicts;
    for (std::uint64_t now = fromMs; now <= toMs; now += 2000)
        verdicts.push_back(below_map::Check(options, watch, facts, now));
    return verdicts;
}

std::size_t CountOf(std::vector<below_map::Verdict> const& verdicts, below_map::Verdict verdict)
{
    return std::size_t(std::count(verdicts.begin(), verdicts.end(), verdict));
}

// Map 1 pos 10329,848,1133 with ground_z 1326 (a Teldrassil place 193 yards up). Checks every 2 s, as the policy does.
below_map::Facts Teldrassil()
{
    below_map::Facts facts;
    facts.HasGround = true;
    facts.GroundZ = 1326.0f;
    facts.NearestDz = 193.0f;
    facts.Places = 2;   // 2026-11-20: BelowMapMinPlaces
    facts.Floors = 1;
    facts.Z = 1133.0f;
    return facts;
}

void TheDropMustPersistForSeveralChecks()
{
    below_map::Options const options = BelowOn();
    below_map::Watch watch;
    below_map::Facts const facts = Teldrassil();
    // Four checks over 30 s are not enough (BelowChecks 5), nor five within 8 s (BelowForMs 20 s).
    CHECK(below_map::Check(options, watch, facts, 0) == below_map::Verdict::kWaiting);
    CHECK(below_map::Check(options, watch, facts, 10000) == below_map::Verdict::kWaiting);
    CHECK(below_map::Check(options, watch, facts, 20000) == below_map::Verdict::kWaiting);
    CHECK(below_map::Check(options, watch, facts, 30000) == below_map::Verdict::kWaiting);
    CHECK(below_map::Check(options, watch, facts, 32000) == below_map::Verdict::kSendBelow);   // the fifth, 32 s after the first
    below_map::Watch quick;
    for (std::uint64_t now = 0; now <= 8000; now += 2000)
        CHECK(below_map::Check(options, quick, facts, now) == below_map::Verdict::kWaiting);
    // A break starts it over.
    below_map::Watch broken;
    for (std::uint64_t now = 0; now < 30000; now += 2000)
    {
        below_map::Facts step = facts;
        if (now == 16000)
            step.Z = 1320.0f;
        below_map::Check(options, broken, step, now);
    }
    CHECK(broken.BelowChecks == 6 && broken.BelowSinceMs == 18000);
}

void ABotThatWalkedThereIsNotBelowTheMap()
{
    using below_map::Verdict;
    below_map::Options const options = BelowOn();
    below_map::Facts const facts = Teldrassil();
    // The core applied a walk with the bot at this height: it stands on the navmesh, on a floor the known places do not show. Skipped (at 22 s, the
    // sign's 20 s), and the sign starts over.
    below_map::Watch walked;
    below_map::NoteRouted(options, walked, 1133.0f, 0);
    std::vector<Verdict> const onPath = Checks(options, walked, facts, 2000, 30000);
    CHECK(onPath[10] == Verdict::kSkipOnPath && CountOf(onPath, Verdict::kSkipOnPath) == 1 && CountOf(onPath, Verdict::kSendBelow) == 0);
    CHECK(std::string(below_map::VerdictName(Verdict::kSkipOnPath)) == "on_path" && walked.LastSkipBelow && walked.BelowChecks == 4);
    // ... unless it is falling (the drop is happening now), or it dropped more than 20 yards since that walk, or the walk is older than 60 s. 2026-11-20:
    // a fall is skipped for its first 30 s (FallGraceMs), then the drop must persist: a fall of 50 s is sent.
    below_map::Watch falling;
    below_map::NoteRouted(options, falling, 1133.0f, 0);
    below_map::Facts fall = facts;
    fall.Falling = true;
    std::vector<Verdict> const fallen = Checks(options, falling, fall, 2000, 60000);
    CHECK(CountOf(fallen, Verdict::kSendBelow) == 1 && fallen[0] == Verdict::kSkipFalling && fallen[14] == Verdict::kSkipFalling && fallen[15] == Verdict::kWaiting);
    below_map::Watch dropped;
    below_map::NoteRouted(options, dropped, 1300.0f, 0);
    CHECK(CountOf(Checks(options, dropped, facts, 2000, 30000), Verdict::kSendBelow) == 1);
    below_map::Watch old;
    below_map::NoteRouted(options, old, 1133.0f, 0);
    std::vector<Verdict> const late = Checks(options, old, facts, 62000, 90000);
    CHECK(CountOf(late, Verdict::kSendBelow) == 1 && CountOf(late, Verdict::kSkipOnPath) == 0);
    // The key off: nothing is noted.
    below_map::Options off;
    below_map::Watch none;
    below_map::NoteRouted(off, none, 1133.0f, 0);
    CHECK(!none.Routed);
}

void TransportsAndWaterAreSkipped()
{
    below_map::Options const options = BelowOn();
    // Map 0 (the Ironforge sample of 11-18, z 19 under the house at 505): a real fall is still sent.
    below_map::Facts ironforge;
    ironforge.HasGround = true;
    ironforge.GroundZ = 505.0f;
    ironforge.NearestDz = 486.0f;
    ironforge.Places = 2;
    ironforge.Z = 19.0f;
    using below_map::Verdict;
    below_map::Watch real;
    std::vector<Verdict> const fell = Checks(options, real, ironforge, 0, 30000);
    CHECK(fell[10] == Verdict::kSendBelow && CountOf(fell, Verdict::kSendBelow) == 1);
    below_map::Facts ship = ironforge;
    ship.OnTransport = true;
    below_map::Watch onShip;
    std::vector<Verdict> const deck = Checks(options, onShip, ship, 0, 30000);
    CHECK(deck[10] == Verdict::kSkipTransport && CountOf(deck, Verdict::kSendBelow) == 0 && std::string(below_map::VerdictName(Verdict::kSkipTransport)) == "on_transport");
    below_map::Facts water = ironforge;
    water.Swimming = true;
    below_map::Watch swim;
    std::vector<Verdict> const swimming = Checks(options, swim, water, 0, 30000);
    CHECK(swimming[10] == Verdict::kSkipSwimming && CountOf(swimming, Verdict::kSendBelow) == 0);
    // A flight is still blocked, not skipped.
    below_map::Facts taxi = ironforge;
    taxi.Travelling = true;
    below_map::Watch flying;
    std::vector<Verdict> const flight = Checks(options, flying, taxi, 0, 30000);
    CHECK(flight.back() == Verdict::kBlocked && CountOf(flight, Verdict::kSendBelow) == 0);
}

// Map 2991 pos 4126,1953,976 with ground_z 980: a storm of short no-route answers beside a place at the bot's own height.
void AStormBesideAPlaceAtTheBotsHeightIsSkipped()
{
    below_map::Options const options = BelowOn();
    below_map::Facts skyborne;
    skyborne.HasGround = true;
    skyborne.GroundZ = 980.0f;
    skyborne.NearestDz = 4.0f;
    skyborne.Places = 2;
    skyborne.Z = 976.0f;
    // 2026-11-20: eight different destinations, each 44 yards above the bot (the storm counts nothing else).
    auto const refuse = [&](below_map::Watch& watch, int i, std::uint64_t at)
    { below_map::NoteNoRoute(options, watch, 40.0f, 4126.0f + float(i) * 20.0f, 1953.0f, 1020.0f, 976.0f, at); };
    below_map::Watch storm;
    for (int i = 0; i < 8; ++i)
        refuse(storm, i, 1000 + std::uint64_t(i) * 1000);
    below_map::Verdict const near = below_map::Check(options, storm, skyborne, 10000);
    CHECK(near == below_map::Verdict::kSkipNearGround && !storm.LastSkipBelow && storm.NoRoutes.empty());
    // No place near the bot's height: the storm is sent, unless a walk was applied after the storm began.
    below_map::Facts lost = skyborne;
    lost.NearestDz = 80.0f;
    below_map::Watch walked;
    for (int i = 0; i < 8; ++i)
        refuse(walked, i, 1000 + std::uint64_t(i) * 1000);
    below_map::NoteRouted(options, walked, 976.0f, 5000);
    CHECK(below_map::Check(options, walked, lost, 10000) == below_map::Verdict::kSkipOnPath);
    below_map::Watch stuck;
    below_map::NoteRouted(options, stuck, 976.0f, 500);   // before the storm
    for (int i = 0; i < 8; ++i)
        refuse(stuck, i, 1000 + std::uint64_t(i) * 1000);
    CHECK(below_map::Check(options, stuck, lost, 10000) == below_map::Verdict::kSendStorm);
}

void TheSkipLineIsCappedPerBot()
{
    below_map::Options const options = BelowOn();
    below_map::Watch watch;
    CHECK(below_map::SkipLogDue(options, watch, 0));
    CHECK(!below_map::SkipLogDue(options, watch, 599999));
    CHECK(below_map::SkipLogDue(options, watch, 600000));
}

// The ground is the lowest known place near the bot, not the nearest: a giver on a tree platform beside a place on the ground.
void TheGroundIsTheLowestNearPlace()
{
    off_map::KnownWorld world;
    world.Add({ 1, 10311.0f, 832.0f, 1326.0f, 0 });   // up the tree
    world.Add({ 1, 10350.0f, 860.0f, 1140.0f, 0 });   // on the ground, 25 yards away
    world.Add({ 1, 10600.0f, 800.0f, 1100.0f, 0 });   // farther than 60 yards: not counted
    off_map::KnownWorld::Around const around = world.Near(1, 10329.0f, 848.0f, 1133.0f, 60.0f);
    CHECK(around.Lowest && around.Lowest->Z == 1140.0f && around.NearestDz == 7.0f);
    CHECK(world.Near(1, 0.0f, 0.0f, 0.0f, 60.0f).Lowest == nullptr && world.Near(2, 10329.0f, 848.0f, 1133.0f, 60.0f).Lowest == nullptr);
}

// The policy: a bot whose walk the core applied is skipped once (below_map_skip, counted in the hourly line); once that walk is old it is sent up.
void ThePolicySkipsABotOnItsPathAndCountsIt()
{
    std::vector<std::string> events;
    std::vector<std::string> warnings;
    // 2026-11-20: the map must be known (10 places or more) and two places must lie within 60 yards (BelowMapMinPlaces).
    std::istringstream spots("spot 1 10311 832 1326 1 6 0 10\nspot 1 10335 870 1330 1 6 0 10\nspot 1 0 0 0 1 6 0 10\nspot 1 100 0 0 1 6 0 10\n"
                             "spot 1 200 0 0 1 6 0 10\nspot 1 300 0 0 1 6 0 10\nspot 1 400 0 0 1 6 0 10\nspot 1 500 0 0 1 6 0 10\n"
                             "spot 1 600 0 0 1 6 0 10\nspot 1 700 0 0 1 6 0 10\n");
    PolicyOptions options = AggressiveOptions();
    options.Grind = std::make_shared<GrindData>(ParseGrindData(spots, warnings));
    options.WanderWhenIdle = false;
    options.OnEvent = [&events](std::string const& text) { events.push_back(text); };
    options.BelowMap.Enabled = true;
    options.DeathsTopEveryMs = 3600000;
    BotPolicy policy(options);
    CHECK(!policy.TakeDeathsTopLine(500));
    WorldTickSnapshotView view = View(1, 0, {});
    view.bots[0].map_id = 1;
    view.bots[0].level = 5;
    view.bots[0].pos_x = 10329.0f;
    view.bots[0].pos_y = 848.0f;
    view.bots[0].pos_z = 1133.0f;
    view.bots[0].last_action_type = std::uint8_t(ActionType::kMoveTo);
    view.bots[0].last_action_status = forever_module::BotActionStatus::kApplied;
    view.bots[0].last_action_sequence = 7;   // read once: the walk was applied at 1 s
    int teleports = 0;
    std::uint64_t firstTeleportMs = 0;
    for (std::uint64_t now = 1000; now <= 120000; now += 1000)
        if (TeleportOf(policy.Decide(view, now), kBot))
        {
            ++teleports;
            if (firstTeleportMs == 0)
                firstTeleportMs = now;
        }
    CHECK(teleports == 1 && firstTeleportMs > 61000);
    CHECK(Logged(events, "below_map_skip: bot=42 reason=on_path sign=z_below map=1 pos=10329,848,1133 ground_z=1326"));
    int skipLines = 0;
    for (std::string const& event : events)
        skipLines += event.rfind("below_map_skip:", 0) == 0;
    CHECK(skipLines == 1);   // once per bot per 10 minutes
    CHECK(Logged(events, "below_map_recover: bot=42 reason=z_below map=1 pos=10329,848,1133 ground_z=1326 z=1133 drop=193 places=2 floors=1 low_z=1326 "
                         "high_z=1330 checks=11 moving=0 to=10311,832,1326"));
    std::optional<std::string> const line = policy.TakeDeathsTopLine(3600500);
    CHECK(line && line->find(" below_map_recover=1 below_map_skip=on_path:1") != std::string::npos);
}

// --- Settings --------------------------------------------------------------------------------------------------------------------------------------------

void TheThrottleKeyHasItsDefaultAndBounds()
{
    std::vector<std::string> warnings;
    BotSettings const defaults = ParseBotSettings({}, warnings);
    CHECK(defaults.LevelSafeLogThrottleSec == 60 && !defaults.BelowMapRecover && !defaults.Auction.Walk3D && warnings.empty());
    BotSettings const every = ParseBotSettings({ { "LevelSafeLogThrottleSec", "0" } }, warnings);
    CHECK(every.LevelSafeLogThrottleSec == 0 && warnings.empty());
    BotSettings const bad = ParseBotSettings({ { "LevelSafeLogThrottleSec", "5000" } }, warnings);
    CHECK(bad.LevelSafeLogThrottleSec == 60 && warnings.size() == 1);
}

} // namespace

int main()
{
    TheLevelSafeLineIsThrottledPerBotQuestAndReason();
    TheBlockStaysAndTheHourlyLineCountsTheLines();
    TheAuctioneerHeightIsJudgedBeforeItIsTrusted();
    TheAuctionErrandWalksToTheAuctioneersOwnPlace();
    TheDropMustPersistForSeveralChecks();
    ABotThatWalkedThereIsNotBelowTheMap();
    TransportsAndWaterAreSkipped();
    AStormBesideAPlaceAtTheBotsHeightIsSkipped();
    TheSkipLineIsCappedPerBot();
    TheGroundIsTheLowestNearPlace();
    ThePolicySkipsABotOnItsPathAndCountsIt();
    TheThrottleKeyHasItsDefaultAndBounds();
    if (failures == 0)
        std::printf("arm_c_fixes_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
