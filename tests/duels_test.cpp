// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the duel decision (src/duels.h). No core or SDK needed.
#include "duels.h"
#include <cstdio>
namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
}  // namespace

using namespace duels;

int main()
{
    Settings on;
    on.Enabled = true;
    State fresh;
    Context idle;
    idle.Idle = true;
    idle.Level = 20;
    idle.RivalNearby = true;

    CHECK(Decide(Settings{}, fresh, idle, 0) == Action::None);                 // off by default
    CHECK(Decide(on, fresh, idle, 0) == Action::Challenge);
    Context low = idle; low.Level = 5;
    CHECK(Decide(on, fresh, low, 0) == Action::None);                          // below DuelMinLevel
    Context alone = idle; alone.RivalNearby = false;
    CHECK(Decide(on, fresh, alone, 0) == Action::None);
    Context busy = idle; busy.InCombat = true;
    CHECK(Decide(on, fresh, busy, 0) == Action::None);
    Context incoming = idle; incoming.ChallengeReceived = true; incoming.RivalNearby = false;
    CHECK(Decide(on, fresh, incoming, 0) == Action::Accept);                   // a challenge is answered without a rival in view

    State after;
    NoteStarted(after, on, 1000);
    CHECK(Decide(on, after, idle, 1000) == Action::None);                      // cooldown
    CHECK(Decide(on, after, idle, 1000 + 30ull * 60000ull) == Action::Challenge);

    if (failures == 0) std::printf("duels: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
