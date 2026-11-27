// SPDX-License-Identifier: GPL-2.0-or-later
// Tests of TravelGraph (travel_graph.h) and of the two keys being off by default.
#include "policy_test_support.h"
#include "bot_settings.h"
#include "travel_graph.h"

#include <cstdio>

namespace {

using travel_graph::Graph;
using travel_graph::Pos;

// A bot walks the polyline at 7 yards a second, seen once a second.
void Walk(Graph& graph, std::uint64_t bot, std::uint32_t map, std::initializer_list<Pos> points)
{
    std::uint64_t ms = 1000;
    Pos at = *points.begin();
    graph.Observe(bot, map, at, ms, true);
    for (Pos const to : points)
        while (std::hypot(to.X - at.X, to.Y - at.Y) > 0.5f)
        {
            float const left = std::hypot(to.X - at.X, to.Y - at.Y);
            float const step = std::min(left, 7.0f);
            at = { at.X + (to.X - at.X) / left * step, at.Y + (to.Y - at.Y) / left * step, to.Z };
            graph.Observe(bot, map, at, ms += 1000, true);
        }
}

// Follows legs from `at` to `to`; the legs taken, or -1 when the way ends early.
int Follow(Graph const& graph, Pos at, Pos to)
{
    for (int legs = 0; legs < 40; ++legs)
    {
        if (std::hypot(to.X - at.X, to.Y - at.Y) <= 60.0f)
            return legs;
        auto const leg = graph.Next(0, at, to);
        if (!leg || std::hypot(leg->Point.X - at.X, leg->Point.Y - at.Y) > travel_graph::kLegYards + 60.0f)
            return -1;
        at = leg->Point;
    }
    return -1;
}

void AWayAroundAWallIsFollowedLegByLeg()
{
    Graph graph;
    // From (0,0) to (1000,0) only by way of y = 600.
    Walk(graph, 1, 0, { { 0, 0, 0 }, { 0, 600, 0 }, { 1000, 600, 0 }, { 1000, 0, 0 } });
    auto const first = graph.Next(0, { 0, 0, 0 }, { 1000, 0, 0 });
    CHECK(first && first->Point.X < 60.0f && first->Point.Y > 100.0f);       // it starts away from the straight line
    CHECK(first && first->Remaining > 2000.0f && first->Remaining < 2500.0f);
    CHECK(Follow(graph, { 0, 0, 0 }, { 1000, 0, 0 }) > 5);
    CHECK(Follow(graph, { 1000, 0, 0 }, { 0, 0, 0 }) > 5);                   // flat ground: both ways
    CHECK(!graph.Next(1, { 0, 0, 0 }, { 1000, 0, 0 }));                      // another map
    CHECK(!graph.Next(0, { 400, 0, 0 }, { 1000, 0, 0 }));                    // the bot is not on a known way
}

void FlightsTeleportsAndGhostsAreNotLearned()
{
    Graph graph;
    for (int i = 0; i <= 30; ++i)
        graph.Observe(1, 0, { float(i) * 32.0f, 0, 0 }, 1000 + i * 1000, true);   // a flight, 32 yards a second
    graph.Observe(2, 0, { 0, 0, 0 }, 1000, true);
    graph.Observe(2, 0, { 3000, 0, 0 }, 60000, true);                             // a teleport
    for (int i = 0; i <= 30; ++i)
        graph.Observe(3, 0, { float(i) * 7.0f, 500, 0 }, 1000 + i * 1000, false); // a ghost
    CHECK(graph.Nodes() == 0);
}

void ADropIsOneWay()
{
    Graph graph;
    Walk(graph, 1, 0, { { 0, 0, 100 }, { 200, 0, 100 } });
    for (int i = 0; i <= 60; ++i)
        graph.Observe(2, 0, { 200.0f + float(i) * 7.0f, 0, i < 3 ? 100.0f : 0.0f }, 1000 + i * 1000, true);
    CHECK(graph.Next(0, { 0, 0, 100 }, { 600, 0, 0 }));
    CHECK(Follow(graph, { 600, 0, 0 }, { 0, 0, 100 }) == -1);                  // not back up the drop
}

// An errand 1000 yards off with a wall between: the core answers every walk "no route" and the bot does not move. The bot has walked round the wall before, so
// with the key the next walk goes to a leg of that way; without it nothing changes.
void AChainThatEndsTakesAKnownWay()
{
    for (bool on : { true, false })
    {
        std::vector<std::string> events;
        PolicyOptions options = AggressiveOptions();
        options.TravelGraph = on;
        options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
        BotPolicy policy(options);
        std::uint64_t now = 1000;
        WorldTickSnapshotView view = View(1, 0, {});
        view.bots[0].level = 10;
        for (auto const& [x, y] : std::initializer_list<std::pair<float, float>>{ { 0, 0 }, { 0, 600 }, { 1000, 600 }, { 1000, 0 } })
            for (;;)
            {
                float const dx = x - view.bots[0].pos_x, dy = y - view.bots[0].pos_y, left = std::hypot(dx, dy);
                if (left < 0.5f)
                    break;
                float const step = std::min(left, 7.0f);
                view.bots[0].pos_x += dx / left * step;
                view.bots[0].pos_y += dy / left * step;
                policy.Decide(view, now += 1000);
            }
        view.bots[0].pos_x = 0;
        view.bots[0].pos_y = 0;
        ErrandOrder order;
        order.Map = 0;
        order.Dest = Point{ 1000.0f, 0.0f, 0.0f };
        policy.StartErrand(kBot, order);
        std::uint64_t sequence = 0;
        bool detoured = false;
        for (int i = 0; i < 40 && !detoured; ++i)
        {
            bool moved = false;
            for (BotAction const& action : policy.Decide(view, now += 1000))
                if (action.type == ActionType::kMoveTo)
                {
                    moved = true;
                    detoured = detoured || action.dest_y > 100.0f;
                }
            if (moved && !detoured)
            {
                view.bots[0].last_action_sequence = ++sequence;
                view.bots[0].last_action_type = std::uint8_t(ActionType::kMoveTo);
                view.bots[0].last_action_status = BotActionStatus::kNoRoute;
            }
        }
        CHECK(detoured == on);
        bool logged = false;
        for (std::string const& line : events)
            logged = logged || line.rfind("travel_detour: bot=42 ", 0) == 0;
        CHECK(logged == on);
    }
}

void BothKeysAreOffByDefault()
{
    BotSettings const settings;
    CHECK(!settings.TravelGraph && !settings.DeathPlaceMemory);
    PolicyOptions const options;
    CHECK(!options.TravelGraph && !options.DeathPlaceMemory);
}

} // namespace

int main()
{
    AWayAroundAWallIsFollowedLegByLeg();
    FlightsTeleportsAndGhostsAreNotLearned();
    ADropIsOneWay();
    AChainThatEndsTakesAKnownWay();
    BothKeysAreOffByDefault();
    if (failures == 0)
        std::printf("travel_graph_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
