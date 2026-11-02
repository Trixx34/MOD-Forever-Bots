// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the inn downtime decision (src/inn_downtime.h). No core or SDK needed.
#include "inn_downtime.h"
#include <cstdio>
namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
}  // namespace

using namespace inn;

int main()
{
    Settings on;
    on.Enabled = true;
    State state;
    Context hurt;
    hurt.HealthPct = 30;
    hurt.Idle = true;

    CHECK(Decide(Settings{}, state, hurt, 0) == Action::None);                 // off by default
    CHECK(Decide(on, state, hurt, 0) == Action::GoToInn);
    Context healthy; healthy.Idle = true;
    CHECK(Decide(on, state, healthy, 0) == Action::None);
    Context atInn = hurt; atInn.AtInn = true;
    CHECK(Decide(on, state, atInn, 0) == Action::Rest);
    CHECK(state.Resting);
    Context resting = atInn; resting.HealthPct = 60;
    CHECK(Decide(on, state, resting, 5 * 60000ull) == Action::Rest);           // still under the cap
    CHECK(Decide(on, state, resting, 10ull * 60000ull) == Action::Stop);       // time cap reached
    CHECK(!state.Resting);
    CHECK(Decide(on, state, atInn, 11ull * 60000ull) == Action::None);         // cooldown after the stop

    State full;
    full.Resting = true;
    full.RestStartedMs = 0;
    Context done = atInn; done.HealthPct = 100; done.PowerPct = 100;
    CHECK(Decide(on, full, done, 60000ull) == Action::Stop);                  // full before the cap
    State fight;
    fight.Resting = true;
    Context attacked = atInn; attacked.InCombat = true;
    CHECK(Decide(on, fight, attacked, 0) == Action::Stop);                    // a fight ends the rest

    if (failures == 0) std::printf("inn_downtime: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
