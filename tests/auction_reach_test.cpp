// SPDX-License-Identifier: GPL-2.0-or-later
// Tests of the auction reach fix (2026-11-20, doc/auction-reach-fix.md): the call gate the core's interaction rule asks for (auction::JudgeCall), the
// refusal codes and causes of the journal lines, the live auctioneer's own position (auction::FindLive), and with AuctionWalk3D the stand points around
// the auctioneer that the errand walks to and moves on from (auction::StandPoint, auction::JudgeStandTry). The positions are the prod and arm C samples.
#include "policy_test_support.h"
#include "auction_policy.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr std::uint64_t Ref(std::uint64_t type, std::uint64_t entry, std::uint64_t counter)
{
    return (type << 56) | (entry << 32) | counter;
}

auction::CallFacts Facts(float botX, float botY, float botZ, float ax, float ay, float az)
{
    auction::CallFacts facts;
    facts.AuctioneerValid = true;
    facts.BotX = botX;
    facts.BotY = botY;
    facts.BotZ = botZ;
    facts.AuctioneerX = ax;
    facts.AuctioneerY = ay;
    facts.AuctioneerZ = az;
    return facts;
}

// --- the call gate (no key) -------------------------------------------------------------------------------------------------------------------------------

void TheGateFollowsTheCoresInteractionRule()
{
    using auction::CallBlock;
    // On the auctioneer's floor, 3 yards away, standing: the core takes the call.
    CHECK(auction::JudgeCall(Facts(0.0f, 0.0f, -56.0f, 3.0f, 0.0f, -56.8f)) == CallBlock::kOk);
    // Arm C sample 1: 2D 3.7, 3D 14.2 (another floor of the house): in the module's old 2D reach, beyond the core's 3D reach.
    CHECK(auction::JudgeCall(Facts(0.0f, 0.0f, -43.1f, 3.7f, 0.0f, -56.8f)) == CallBlock::kFarHeight);
    // Arm C sample 2: under Ironforge (bot z 7.4, the house at z 505.2).
    CHECK(auction::JudgeCall(Facts(0.0f, 0.0f, 7.4f, 1.9f, 0.0f, 505.2f)) == CallBlock::kFarHeight);
    // The core's reach in 3D: 8.5 yards (1.5 + 4 + 1.5 + 1.5). A step of height within it is fine; just beyond it is not.
    CHECK(auction::JudgeCall(Facts(0.0f, 0.0f, 0.0f, 4.0f, 0.0f, 7.4f)) == CallBlock::kOk);    // 3D 8.41
    CHECK(auction::JudgeCall(Facts(0.0f, 0.0f, 0.0f, 4.0f, 0.0f, 7.6f)) == CallBlock::kFarHeight);   // 3D 8.59
    // The 2D reach stays 5 yards.
    CHECK(auction::JudgeCall(Facts(0.0f, 0.0f, 0.0f, 5.5f, 0.0f, 0.0f)) == CallBlock::kFar2d);
    // On a flight over the house, or walking through it: the core refuses (IsInFlight), or the bot is gone when the call lands.
    auction::CallFacts flying = Facts(0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
    flying.InFlight = true;
    CHECK(auction::JudgeCall(flying) == CallBlock::kInFlight);
    auction::CallFacts walking = Facts(0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
    walking.Moving = true;
    CHECK(auction::JudgeCall(walking) == CallBlock::kMoving);
    CHECK(auction::JudgeCall(auction::CallFacts{}) == CallBlock::kNoAuctioneer);
    CHECK(std::string(auction::CallBlockName(CallBlock::kFarHeight)) == "far_height" && std::string(auction::CallBlockName(CallBlock::kOk)) == "ok" &&
          std::string(auction::CallBlockName(CallBlock::kInFlight)) == "in_flight" && std::string(auction::CallBlockName(CallBlock::kMoving)) == "moving" &&
          std::string(auction::CallBlockName(CallBlock::kFar2d)) == "far_2d" && std::string(auction::CallBlockName(CallBlock::kNoAuctioneer)) == "no_auctioneer");
    // The gate is never wider than the AuctionWalk3D reach: whatever InReach3D accepts within 5 yards in 2D, the gate does too.
    for (float dz = 0.0f; dz <= 4.5f; dz += 0.5f)
        CHECK(!auction::InReach3D(true, 0.0f, 0.0f, dz, 1.0f, 0.0f, 0.0f) || auction::JudgeCall(Facts(0.0f, 0.0f, dz, 1.0f, 0.0f, 0.0f)) == CallBlock::kOk);
}

// --- the refusal codes and causes -----------------------------------------------------------------------------------------------------------------------

void TheRefusalLinesSayWhatFailed()
{
    std::string const interact = "auctioneer not found or cannot interact with it";
    CHECK(std::string(auction::RefusalCode(interact)) == "cannot_interact");
    CHECK(std::string(auction::RefusalCode("auctioneer not found on the player's map")) == "auctioneer_missing");
    CHECK(std::string(auction::RefusalCode("not enough money for the deposit")) == "no_money");
    CHECK(std::string(auction::RefusalCode("item is no longer in the player's bags")) == "item_gone");
    CHECK(std::string(auction::RefusalCode("auctioneer has no auction house")) == "no_house");
    CHECK(std::string(auction::RefusalCode("auction not found or not owned by the player")) == "auction_gone");
    CHECK(std::string(auction::RefusalCode("cannot bid on your own auction")) == "own_auction");
    CHECK(std::string(auction::RefusalCode("AuctionHouse list has no acting player")) == "no_player");
    CHECK(std::string(auction::RefusalCode("something new")) == "other" && std::string(auction::RefusalCode("")) == "other");
    // The prod line (reason=auctioneer_not_found_or_cannot_interact_ class=retry) gets a code and the module's view of the place.
    CHECK(auction::RefusalFields(interact, Facts(0.0f, 0.0f, -43.1f, 3.7f, 0.0f, -56.8f)) == " code=cannot_interact cause=far_height");
    auction::CallFacts unseen = Facts(0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f);
    unseen.LiveLooked = true;
    CHECK(auction::RefusalFields(interact, unseen) == " code=cannot_interact cause=not_seen");
    unseen.LiveSeen = true;
    CHECK(auction::RefusalFields(interact, unseen) == " code=cannot_interact cause=in_reach_other");
    auction::CallFacts notLooked = Facts(0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f);
    CHECK(auction::RefusalCause(interact, notLooked) == "in_reach_other");
    CHECK(auction::RefusalFields("not enough money for the deposit", notLooked) == " code=no_money cause=-");
    // The old line is unchanged (dashboards read it); the new fields come after it.
    auction::Listing listing{ 900, 818, 300, 360, 120, 1 };
    CHECK(auction::ListRefusedLine(7, listing, interact) ==
          "auction: bot 7 list_refused item 900 entry=818 price=300 reason=auctioneer_not_found_or_cannot_interact_ class=retry");
    // A house fault is not the item's: the item stays for the next visit.
    CHECK(auction::ClassifyListFailure("auctioneer has no auction house") == auction::ListFailure::Retry);
    CHECK(auction::ClassifyListFailure(interact) == auction::ListFailure::Retry);
    CHECK(auction::ClassifyListFailure("item is no longer in the player's bags") == auction::ListFailure::Skip);
}

// --- the live auctioneer ----------------------------------------------------------------------------------------------------------------------------------

void TheLiveAuctioneerGivesItsOwnHeight()
{
    std::uint64_t const spot = Ref(9, 8670, 12345);
    std::vector<auction::NearUnit> const near = {
        { Ref(9, 8671, 77), 100.0f, 100.0f, 0.0f },       // another auctioneer entry beside it
        { Ref(9, 8670, 4001), 102.0f, 100.0f, -56.8f },   // the live one: the creature's real height
        { Ref(9, 8670, 4002), 140.0f, 100.0f, 0.0f },     // the same entry far away
    };
    std::optional<auction::NearUnit> const live = auction::FindLive(spot, 100.0f, 100.0f, near);
    CHECK(live && live->Ref == Ref(9, 8670, 4001) && live->X == 102.0f && live->Z == -56.8f);
    CHECK(auction::ResolveNearby(spot, 100.0f, 100.0f, near) == Ref(9, 8670, 4001));
    CHECK(!auction::FindLive(spot, 200.0f, 200.0f, near) && !auction::FindLive(0, 100.0f, 100.0f, near) && !auction::FindLive(spot, 100.0f, 100.0f, {}));
}

// --- the stand points (AuctionWalk3D) ---------------------------------------------------------------------------------------------------------------------

void TheStandPointsRingTheAuctioneerFromTheBotsSide()
{
    auction::RefPoint const anchor{ 100.0f, 50.0f, -56.8f };
    // The first point lies on the side the bot comes from (here from the west, x < 100).
    auction::RefPoint const first = auction::StandPoint(anchor, 0.0f, 50.0f, 0);
    CHECK(std::fabs(first.X - (100.0f - auction::kStandRingYards)) < 0.01f && std::fabs(first.Y - 50.0f) < 0.01f && first.Z == -56.8f);
    // Eight distinct points, each kStandRingYards from the anchor at its height, in reach of it in 3D once the bot stands there; the far side last.
    std::vector<auction::RefPoint> points;
    for (std::uint32_t k = 0; k < auction::kStandTries; ++k)
    {
        auction::RefPoint const p = auction::StandPoint(anchor, 0.0f, 50.0f, k);
        CHECK(std::fabs(std::hypot(p.X - anchor.X, p.Y - anchor.Y) - auction::kStandRingYards) < 0.01f && p.Z == anchor.Z);
        CHECK(auction::InReach3D(true, p.X, p.Y, p.Z, anchor.X, anchor.Y, anchor.Z));
        CHECK(auction::JudgeCall(Facts(p.X, p.Y, p.Z, anchor.X, anchor.Y, anchor.Z)) == auction::CallBlock::kOk);
        for (auction::RefPoint const& seen : points)
            CHECK(std::hypot(p.X - seen.X, p.Y - seen.Y) > 1.0f);
        points.push_back(p);
    }
    CHECK(std::fabs(points.back().X - (100.0f + auction::kStandRingYards)) < 0.01f);   // the far side
    // The two points next to the first lie 45 degrees to either side of it.
    CHECK(std::fabs(std::hypot(points[1].X - first.X, points[1].Y - first.Y) - std::hypot(points[2].X - first.X, points[2].Y - first.Y)) < 0.01f);
    // A bot on the anchor itself: any side, still on the ring.
    auction::RefPoint const onTop = auction::StandPoint(anchor, 100.0f, 50.0f, 0);
    CHECK(std::fabs(std::hypot(onTop.X - anchor.X, onTop.Y - anchor.Y) - auction::kStandRingYards) < 0.01f);
    // The attempt wraps around.
    auction::RefPoint const wrapped = auction::StandPoint(anchor, 0.0f, 50.0f, auction::kStandTries);
    CHECK(wrapped.X == first.X && wrapped.Y == first.Y);
}

void AStandPointGivesWayAfterNoRouteOrFifteenSecondsBesideTheHouse()
{
    using auction::StandNext;
    CHECK(auction::JudgeStandTry(0, false, 0, 100000) == StandNext::kStay);                     // walking, not near yet
    CHECK(auction::JudgeStandTry(0, false, 90000, 100000) == StandNext::kStay);                 // 10 s beside the house
    CHECK(auction::JudgeStandTry(0, false, 85000, 100000) == StandNext::kNextPoint);            // 15 s
    CHECK(auction::JudgeStandTry(0, true, 0, 100000) == StandNext::kNextPoint);                 // no route to the point
    CHECK(auction::JudgeStandTry(auction::kStandTries - 2, true, 0, 1) == StandNext::kNextPoint);
    CHECK(auction::JudgeStandTry(auction::kStandTries - 1, true, 0, 1) == StandNext::kGiveUp);  // the last point
    CHECK(auction::JudgeStandTry(auction::kStandTries - 1, false, 1, 20000) == StandNext::kGiveUp);
    CHECK(auction::JudgeStandTry(0, false, 200000, 100000) == StandNext::kStay);                // a clock behind the mark is no time spent
}

// --- the errand walk with the policy (AuctionWalk3D) ------------------------------------------------------------------------------------------------------

struct Walk
{
    std::vector<std::string> events;
    std::vector<BotAction> moves;
    ErrandOutcome outcome = ErrandOutcome::kNone;
};

// One bot at (0, 0, botZ) on an auction errand to the snapshot's auctioneer at (3.7, 0, -56.8) (arm C sample 1), ticks every second to `untilMs`. `noRoute`:
// every move the bot is sent is answered kNoRoute. `liveZ`: the live auctioneer is in the bot's nearby list at this height.
Walk WalkToHouse(bool walk3d, float botZ, bool noRoute, std::optional<float> liveZ, std::uint64_t untilMs, float botX = 0.0f)
{
    Walk run;
    PolicyOptions options;
    options.AuctionWalk3D = walk3d;
    options.OnEvent = [&run](std::string const& text) { run.events.push_back(text); };
    BotPolicy policy(options);
    std::vector<NearbyUnit> nearby;
    if (liveZ)
    {
        NearbyUnit unit;
        unit.guid = Ref(9, 8670, 4001);
        unit.pos_x = 3.7f;
        unit.pos_z = *liveZ;
        unit.health = 100;
        unit.max_health = 100;
        nearby.push_back(unit);
    }
    WorldTickSnapshotView view = View(1, 0, nearby);
    BotSnapshot& bot = view.bots[0];
    bot.pos_x = botX;
    bot.pos_z = botZ;
    bot.auctioneer_valid = 1;
    bot.auctioneer_guid = Ref(9, 8670, 12345);
    bot.auctioneer_x = 3.7f;
    bot.auctioneer_z = -56.8f;
    policy.Decide(view, 500);
    ErrandOrder order;
    order.Map = 0;
    order.Dest = Point{ 3.7f, 0.0f, -56.8f };
    order.Auction = true;
    policy.StartErrand(kBot, order);
    std::uint64_t sequence = 0;
    for (std::uint64_t now = 1000; now <= untilMs; now += 1000)
    {
        bool moved = false;
        for (BotAction const& action : policy.Decide(view, now))
            if (action.bot_guid == kBot && action.type == ActionType::kMoveTo)
            {
                run.moves.push_back(action);
                moved = true;
            }
        if (ErrandOutcome const outcome = policy.TakeErrandOutcome(kBot); outcome != ErrandOutcome::kNone)
        {
            run.outcome = outcome;
            break;
        }
        if (noRoute && moved)
        {
            bot.last_action_sequence = ++sequence;
            bot.last_action_type = uint8_t(ActionType::kMoveTo);
            bot.last_action_status = BotActionStatus::kNoRoute;
        }
    }
    return run;
}

void TheErrandWalksToAStandPointAndNotIntoTheCounter()
{
    // A bot far below (z 0) walking in: the move goes to the stand point on its side, at the auctioneer's height; never into the auctioneer's own spot.
    Walk const walk = WalkToHouse(true, 0.0f, false, std::nullopt, 5000);
    CHECK(!walk.moves.empty());
    for (BotAction const& move : walk.moves)
        CHECK(std::fabs(std::hypot(move.dest_x - 3.7f, move.dest_y) - auction::kStandRingYards) < 0.01f && move.dest_z == -56.8f);
    CHECK(walk.moves.front().dest_x < 3.7f);   // on the bot's side
}

void NoRouteToAStandPointTriesTheNextAndOnlyTheLastFailsTheErrand()
{
    Walk const walk = WalkToHouse(true, 0.0f, true, std::nullopt, 120000);
    CHECK(walk.outcome == ErrandOutcome::kFailed);
    CHECK(Logged(walk.events, "auction_stand_retry: bot=42 try=1 why=no_route live=0"));
    CHECK(std::count_if(walk.events.begin(), walk.events.end(), [](std::string const& e) { return e.rfind("auction_stand_retry: bot=42 ", 0) == 0; }) == 7);
    CHECK(Logged(walk.events, "errand_failed: bot=42 reason=auction_stand_no_route tries=8"));
    CHECK(!LoggedWith(walk.events, "reason=no_route"));    // the errand is not failed by the first refusal any more
    CHECK(LoggedWith(walk.events, "auction_reach: bot=42 check=no_route where=errand"));
    // Each refused point was followed by a move to another point (the pause of a refused place does not hold the next one).
    std::vector<std::pair<int, int>> distinct;
    for (BotAction const& move : walk.moves)
    {
        std::pair<int, int> const at{ int(std::lround(move.dest_x * 10.0f)), int(std::lround(move.dest_y * 10.0f)) };
        if (std::find(distinct.begin(), distinct.end(), at) == distinct.end())
            distinct.push_back(at);
    }
    CHECK(distinct.size() == auction::kStandTries);
}

void WithoutTheKeyNoRouteFailsTheErrandAsBefore()
{
    Walk const walk = WalkToHouse(false, 0.0f, true, std::nullopt, 30000, -20.0f);
    CHECK(walk.outcome == ErrandOutcome::kFailed);
    CHECK(Logged(walk.events, "errand_failed: bot=42 reason=no_route"));
    CHECK(!LoggedWith(walk.events, "auction_stand_retry"));
    // ... and the walk goes to the errand's spot itself.
    CHECK(!walk.moves.empty() && walk.moves.front().dest_x == 3.7f && walk.moves.front().dest_z == -56.8f);
}

void TheLiveAuctioneersHeightIsTheOneWalkedTo()
{
    // The snapshot's spawn says z -56.8; the creature the bot sees stands at -55.2 (its real place). The walk and the reach use the live one; a bot standing
    // at -55.0 beside it is at the house (the spawn's z would have put it 1.8 yards off, still in reach, so the line says live=1 to tell them apart).
    Walk const walk = WalkToHouse(true, -55.0f, false, -55.2f, 40000);
    CHECK(walk.outcome == ErrandOutcome::kDone);
    CHECK(LoggedWith(walk.events, "auction_reach: bot=42 check=ok_3d where=errand") && LoggedWith(walk.events, "auctioneer_z=-55.2") &&
          LoggedWith(walk.events, " live=1 try=0"));
    // A live auctioneer one floor below the bot: in 2D reach, not in 3D: the points move on (near_2d_only) and the errand fails after the last.
    Walk const above = WalkToHouse(true, -43.1f, false, -56.8f, 200000);
    CHECK(above.outcome == ErrandOutcome::kFailed);
    CHECK(Logged(above.events, "auction_stand_retry: bot=42 try=1 why=near_2d_only live=1"));
    CHECK(Logged(above.events, "errand_failed: bot=42 reason=auction_reach_3d tries=8"));
}

} // namespace

int main()
{
    TheGateFollowsTheCoresInteractionRule();
    TheRefusalLinesSayWhatFailed();
    TheLiveAuctioneerGivesItsOwnHeight();
    TheStandPointsRingTheAuctioneerFromTheBotsSide();
    AStandPointGivesWayAfterNoRouteOrFifteenSecondsBesideTheHouse();
    TheErrandWalksToAStandPointAndNotIntoTheCounter();
    NoRouteToAStandPointTriesTheNextAndOnlyTheLastFailsTheErrand();
    WithoutTheKeyNoRouteFailsTheErrandAsBefore();
    TheLiveAuctioneersHeightIsTheOneWalkedTo();
    if (failures == 0)
        std::printf("auction_reach_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
