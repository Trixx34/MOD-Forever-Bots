// SPDX-License-Identifier: GPL-2.0-or-later
// WalkDecideEvery (src/calm_walk.h and BotPolicy::DecideBot): a calm walking bot takes its full turn on one tick in N; anything that is not calm ends the skipping.
#include "policy_test_support.h"
#include "calm_walk.h"

namespace {

BotSnapshot Walker()
{
    BotSnapshot bot;
    bot.guid = 1;
    bot.moving = 1;
    bot.health = bot.max_health = 100;
    return bot;
}

calm_walk::Facts Walking()
{
    calm_walk::Facts facts;
    facts.WalkOrdered = true;
    return facts;
}

void CalmNeedsAWalkAndNothingAround()
{
    NearbyUnit hostile = Unit(8, 5, 20.0f, true, 100);
    NearbyUnit dead = Unit(8, 6, 20.0f, true, 0);
    NearbyUnit friendly = Unit(8, 7, 20.0f, false, 100);
    CHECK(calm_walk::IsCalm(Walker(), {}, Walking()));
    CHECK(!calm_walk::IsCalm(Walker(), {}, calm_walk::Facts{}));                               // no walk ordered
    CHECK(!calm_walk::IsCalm(Walker(), std::span<NearbyUnit const>(&hostile, 1), Walking()));  // a living hostile near
    CHECK(calm_walk::IsCalm(Walker(), std::span<NearbyUnit const>(&dead, 1), Walking()));      // a corpse or a friend does not matter
    CHECK(calm_walk::IsCalm(Walker(), std::span<NearbyUnit const>(&friendly, 1), Walking()));
    BotSnapshot bot = Walker();
    bot.moving = 0;
    CHECK(!calm_walk::IsCalm(bot, {}, Walking()));                                             // arrived
    bot = Walker();
    bot.in_combat = 1;
    CHECK(!calm_walk::IsCalm(bot, {}, Walking()));
    bot = Walker();
    bot.group_leader_guid = 99;
    CHECK(!calm_walk::IsCalm(bot, {}, Walking()));
    bot = Walker();
    bot.group_invite_from = 99;
    CHECK(!calm_walk::IsCalm(bot, {}, Walking()));
    bot = Walker();
    bot.life_state = 1;
    CHECK(!calm_walk::IsCalm(bot, {}, Walking()));
    bot = Walker();
    bot.transport_guid = 5;
    CHECK(!calm_walk::IsCalm(bot, {}, Walking()));
    calm_walk::Facts onOrder = Walking();
    onOrder.OnOrder = true;
    CHECK(!calm_walk::IsCalm(Walker(), {}, onOrder));
    calm_walk::Facts loot = Walking();
    loot.HasLootOrFight = true;
    CHECK(!calm_walk::IsCalm(Walker(), {}, loot));
}

void TheGateSkipsAllButEveryNth()
{
    calm_walk::Gate gate;
    CHECK(!gate.Skip(3, true, 5));          // no full turn recorded yet: full
    gate.Ran(true, 5);
    CHECK(gate.Skip(3, true, 5));           // skipped
    CHECK(gate.Skip(3, true, 5));           // skipped
    CHECK(!gate.Skip(3, true, 5));          // every third is full
    gate.Ran(true, 5);
    CHECK(gate.Skip(3, true, 5));
    CHECK(!gate.Skip(3, true, 6));          // a new result: full at once
    gate.Ran(true, 6);
    CHECK(!gate.Skip(3, false, 6));         // not calm any more: full at once
    gate.Ran(false, 6);
    CHECK(!gate.Skip(3, true, 6));          // the last full turn did not see a calm bot
    gate.Ran(true, 6);
    CHECK(!gate.Skip(1, true, 6));          // 1 = off
    CHECK(!gate.Skip(0, true, 6));
}

// A bot on its walk to a hub: with WalkDecideEvery 3 two ticks in three are skipped, and the walk is not sent again either way.
BotPolicy HubWalker(std::uint32_t every)
{
    PolicyOptions options = AggressiveOptions();
    options.HubsByMap[1] = { Point{ 500.0f, 0.0f, 0.0f }, Point{ 900.0f, 0.0f, 0.0f } };
    options.WalkDecideEvery = every;
    return BotPolicy(options);
}

void APolicyWalkerSkipsTurns()
{
    BotPolicy policy = HubWalker(3);
    WorldTickSnapshotView view = OnMap(1);
    policy.Decide(view, 0);
    CHECK(WalkTo(AfterMount(policy, view, 30000), 10) == 500.0f);   // the walk is ordered
    view.bots[0].moving = 1;
    view.bots[0].last_action_type = std::uint8_t(ActionType::kMoveTo);
    view.bots[0].last_action_status = BotActionStatus::kApplied;
    view.bots[0].last_action_sequence = 1;
    std::uint64_t const before = policy.CalmSkippedTurns();
    for (int tick = 1; tick <= 6; ++tick)
        CHECK(WalkTo(policy.Decide(view, 30000 + 500 * tick), 10) < 0.0f);   // never sent again
    CHECK(policy.CalmSkippedTurns() - before >= 3);
    // The walk ends: the next tick is a full turn, not a skipped one.
    view.bots[0].moving = 0;
    std::uint64_t const skipped = policy.CalmSkippedTurns();
    policy.Decide(view, 34000);
    CHECK(policy.CalmSkippedTurns() == skipped);
}

void AHostileOrANewResultEndsTheSkipping()
{
    BotPolicy policy = HubWalker(3);
    WorldTickSnapshotView view = OnMap(1);
    policy.Decide(view, 0);
    AfterMount(policy, view, 30000);
    view.bots[0].moving = 1;
    view.bots[0].last_action_status = BotActionStatus::kApplied;
    view.bots[0].last_action_sequence = 1;
    policy.Decide(view, 30500);
    std::uint64_t const skipped = policy.CalmSkippedTurns();
    AddNearby(view, 0, Hostile(900, 30.0f, 100));
    for (int tick = 0; tick < 4; ++tick)
        policy.Decide(view, 31000 + 500 * tick);
    CHECK(policy.CalmSkippedTurns() == skipped);   // a living hostile near: every turn is full
    view = OnMap(1);
    view.bots[0].moving = 1;
    view.bots[0].last_action_status = BotActionStatus::kApplied;
    for (std::uint32_t sequence = 2; sequence < 8; ++sequence)
    {
        view.bots[0].last_action_sequence = sequence;   // a new result every tick
        policy.Decide(view, 40000 + 500 * sequence);
    }
    CHECK(policy.CalmSkippedTurns() == skipped);
}

void OffMeansEveryTurnIsFull()
{
    BotPolicy policy = HubWalker(1);
    WorldTickSnapshotView view = OnMap(1);
    policy.Decide(view, 0);
    AfterMount(policy, view, 30000);
    view.bots[0].moving = 1;
    view.bots[0].last_action_sequence = 1;
    for (int tick = 1; tick <= 8; ++tick)
        policy.Decide(view, 30000 + 500 * tick);
    CHECK(policy.CalmSkippedTurns() == 0);
}

} // namespace

int main()
{
    CalmNeedsAWalkAndNothingAround();
    TheGateSkipsAllButEveryNth();
    APolicyWalkerSkipsTurns();
    AHostileOrANewResultEndsTheSkipping();
    OffMeansEveryTurnIsFull();
    if (failures == 0)
        std::printf("calm_walk_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
