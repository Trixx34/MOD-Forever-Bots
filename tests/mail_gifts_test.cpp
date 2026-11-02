// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the gift decision (src/mail_gifts.h). No core or SDK needed.
#include "mail_gifts.h"
#include <cstdio>
namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
}  // namespace

using namespace gifts;

int main()
{
    Settings on;
    on.Enabled = true;
    on.DailyCap = 2;
    State state;
    Context ready;
    ready.Idle = true;
    ready.GiftableItems = 3;
    ready.RecipientKnown = true;

    CHECK(Decide(Settings{}, state, ready, 0) == Action::None);               // off by default
    CHECK(Decide(on, state, ready, 0) == Action::Send);
    CHECK(Decide(on, state, ready, 1000) == Action::Send);                    // second gift of the day
    CHECK(Decide(on, state, ready, 2000) == Action::None);                    // daily cap reached
    CHECK(Decide(on, state, ready, kDayMs + 2000) == Action::Send);           // a new day resets the count

    State none;
    Context nothing = ready; nothing.GiftableItems = 0;
    CHECK(Decide(on, none, nothing, 0) == Action::None);
    Context unknown = ready; unknown.RecipientKnown = false;
    CHECK(Decide(on, none, unknown, 0) == Action::None);
    Context busy = ready; busy.InCombat = true;
    CHECK(Decide(on, none, busy, 0) == Action::None);

    if (failures == 0) std::printf("mail_gifts: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
