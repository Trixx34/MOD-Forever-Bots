// SPDX-License-Identifier: GPL-2.0-or-later
// Tests for talent_respec.h: when a bot's spent talent points should be respecced.
#include "talent_respec.h"

#include <cstdio>

namespace
{
int failures = 0;

void Check(bool ok, char const* what)
{
    if (!ok)
    {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

RespecInputs Base()
{
    RespecInputs in;
    in.Enabled = true;
    in.Level = 20;
    in.Alive = true;
    in.InCombat = false;
    in.PointsSpent = true;
    in.AppliedRole = 3;
    in.AppliedSpec = 0;
    in.WantRole = 1;
    in.WantSpec = 0;
    in.NowMs = 5000000;
    in.NotBeforeMs = 0;
    return in;
}
} // namespace

int main()
{
    Check(RespecDue(Base()), "a role change after points were spent is a respec");

    RespecInputs off = Base();
    off.Enabled = false;
    Check(!RespecDue(off), "off (the default) never respecs");

    RespecInputs low = Base();
    low.Level = 9;
    Check(!RespecDue(low), "below level 10 no respec");

    RespecInputs dead = Base();
    dead.Alive = false;
    Check(!RespecDue(dead), "a dead bot is not respecced");

    RespecInputs combat = Base();
    combat.InCombat = true;
    Check(!RespecDue(combat), "a bot in combat is not respecced");

    RespecInputs fresh = Base();
    fresh.PointsSpent = false;
    Check(!RespecDue(fresh), "nothing spent yet: the talent order spends, no respec");

    RespecInputs same = Base();
    same.WantRole = 3;
    Check(!RespecDue(same), "same role and spec: a level-up, not a respec");

    RespecInputs spec = Base();
    spec.WantSpec = 7;
    spec.AppliedSpec = 0;
    Check(RespecDue(spec), "a spec change after points were spent is a respec");

    RespecInputs cool = Base();
    cool.NotBeforeMs = cool.NowMs + 1;
    Check(!RespecDue(cool), "an hour between respecs of one bot");

    RespecInputs elapsed = Base();
    elapsed.NotBeforeMs = elapsed.NowMs - 1;
    Check(RespecDue(elapsed), "after the interval it may respec again");

    std::printf(failures ? "talent_respec_test: %d failure(s)\n" : "talent_respec_test: ok\n", failures);
    return failures ? 1 : 0;
}
