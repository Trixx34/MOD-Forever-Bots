// SPDX-License-Identifier: GPL-2.0-or-later
// TransportRide: the pure rules (transport_ride.h). No core needed.
#include "transport_ride.h"

#include <cstdio>

using namespace transport_ride;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

Settings On()
{
    Settings s;
    s.Enabled = true;
    return s;
}

Facts Follower()
{
    Facts f;
    f.InGroup = true;
    f.LeaderKnown = true;
    return f;
}

}  // namespace

int main()
{
    // Off by default.
    CHECK(!Settings{}.Enabled);
    {
        Facts f = Follower();
        f.LeaderTransportEntry = 176231;
        CHECK(Decide(Settings{}, f) == Order::kNone);
        CHECK(Decide(On(), f) == Order::kJoin);
    }
    // Aboard the leader's transport: nothing; aboard another one: join; unknown entry: left alone.
    {
        Facts f = Follower();
        f.LeaderTransportEntry = 176231;
        f.BotOnTransport = true;
        f.BotTransportEntry = 176231;
        CHECK(Decide(On(), f) == Order::kNone);
        f.BotTransportEntry = 176310;
        CHECK(Decide(On(), f) == Order::kJoin);
        f.BotTransportEntry = 0;
        CHECK(Decide(On(), f) == Order::kNone);
    }
    // Combat, water, death, flight, no group, the leader itself, an unknown leader: no join.
    {
        Facts f = Follower();
        f.LeaderTransportEntry = 176231;
        f.InCombat = true;
        CHECK(Decide(On(), f) == Order::kNone);
        f.InCombat = false;
        f.Swimming = true;
        CHECK(Decide(On(), f) == Order::kNone);
        f.Swimming = false;
        f.Alive = false;
        CHECK(Decide(On(), f) == Order::kNone);
        f.Alive = true;
        f.InFlight = true;
        CHECK(Decide(On(), f) == Order::kNone);
        f.InFlight = false;
        f.InGroup = false;
        CHECK(Decide(On(), f) == Order::kNone);
        f.InGroup = true;
        f.IsLeader = true;
        CHECK(Decide(On(), f) == Order::kNone);
        f.IsLeader = false;
        f.LeaderKnown = false;
        CHECK(Decide(On(), f) == Order::kNone);
    }
    // The leader on no transport: a passenger is taken off, outside combat.
    {
        Facts f = Follower();
        f.BotOnTransport = true;
        f.BotTransportEntry = 176231;
        CHECK(Decide(On(), f) == Order::kLeave);
        f.InCombat = true;
        CHECK(Decide(On(), f) == Order::kNone);
        f.InCombat = false;
        f.BotOnTransport = false;
        CHECK(Decide(On(), f) == Order::kNone);
    }
    // Pace.
    {
        Settings s = On();
        CHECK(Due(1000, 1000));
        CHECK(!Due(999, 1000));
        CHECK(NextAfter(s, 1000, false) == 1000 + s.EveryMs);
        CHECK(NextAfter(s, 1000, true) == 1000 + s.BackoffMs);
    }
    CHECK(OnTransport(kFlagOnTransport, 0));
    CHECK(OnTransport(0, 5));
    CHECK(!OnTransport(kFlagSwimming, 0));
    CHECK(kBoardSpell == 1 && kLeaveSpell == 2 && kJoinSpell == 3);

    if (failures == 0)
        std::printf("transport_ride_test: OK\n");
    return failures == 0 ? 0 : 1;
}
