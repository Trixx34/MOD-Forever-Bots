// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the battleground decision (src/battlegrounds.h). No core or SDK needed.
#include "battlegrounds.h"
#include <cstdio>
namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
}  // namespace

using namespace battlegrounds;

int main()
{
    Settings on;
    on.Enabled = true;
    State fresh;
    Context idle;
    idle.Idle = true;
    idle.Level = 20;

    CHECK(Decide(Settings{}, fresh, idle, 0) == Action::None);                 // off by default
    CHECK(Decide(on, fresh, idle, 0) == Action::Queue);
    Context low = idle; low.Level = 5;
    CHECK(Decide(on, fresh, low, 0) == Action::None);
    Context busy = idle; busy.InCombat = true;
    CHECK(Decide(on, fresh, busy, 0) == Action::None);

    State queued;
    NoteQueued(queued, 1000);
    CHECK(Decide(on, queued, idle, 1000 + 9ull * 60000ull) == Action::None);    // still waiting
    CHECK(Decide(on, queued, idle, 1000 + 10ull * 60000ull) == Action::Leave);  // queue timed out
    Context inside = idle; inside.InBattleground = true;
    CHECK(Decide(on, queued, inside, 1000 + 10ull * 60000ull) == Action::None); // popped: stay

    State left;
    NoteLeft(left, on, 5000);
    CHECK(!left.Queued);
    CHECK(Decide(on, left, idle, 5000) == Action::None);                       // cooldown
    CHECK(Decide(on, left, idle, 5000 + 60ull * 60000ull) == Action::Queue);

    if (failures == 0) std::printf("battlegrounds: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
