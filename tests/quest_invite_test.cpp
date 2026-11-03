// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for quest group invites (src/quest_invite.h): off by default, range, shared objective, once per 30 minutes, nothing within 2 hours of a decline.
#include "quest_invite.h"

#include <cstdio>

using namespace quest_invite;

namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL %s:%d: %s\\n", __FILE__, __LINE__, #cond); } } while (0)

Settings On() { Settings s; s.Enabled = true; return s; }

Facts Good()
{
    Facts f;
    f.Idle = true;
    f.SameObjective = true;
    f.Distance = 20.0f;
    return f;
}

void Conditions()
{
    Memory m;
    CHECK(!m.ShouldInvite(Settings{}, Good(), 5, 1000));
    CHECK(m.ShouldInvite(On(), Good(), 5, 1000));
    Facts f = Good(); f.Idle = false;           CHECK(!m.ShouldInvite(On(), f, 5, 1000));
    f = Good(); f.SameObjective = false;        CHECK(!m.ShouldInvite(On(), f, 5, 1000));
    f = Good(); f.Distance = 31.0f;             CHECK(!m.ShouldInvite(On(), f, 5, 1000));
    f = Good(); f.GroupFull = true;             CHECK(!m.ShouldInvite(On(), f, 5, 1000));
}

void RepeatAndDecline()
{
    Memory m;
    m.NoteInvited(5, 1000);
    CHECK(!m.ShouldInvite(On(), Good(), 5, 1000 + 29 * 60000));
    CHECK(m.ShouldInvite(On(), Good(), 5, 1000 + 31 * 60000));
    CHECK(m.ShouldInvite(On(), Good(), 6, 2000));                 // another human
    m.NoteDeclined(5, 10000);
    CHECK(!m.ShouldInvite(On(), Good(), 5, 10000 + 119 * 60000));
    CHECK(m.ShouldInvite(On(), Good(), 5, 10000 + 121 * 60000));
}
}  // namespace

int main()
{
    Conditions();
    RepeatAndDecline();
    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::printf("quest_invite_test: ok\n");
    return 0;
}
