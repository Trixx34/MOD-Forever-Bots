// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests of the damage-done estimate (damage_done.h): a target's health loss is shared among the bots on it; heals, gaps and first sightings count nothing.
#include "damage_done.h"

#include <cstdio>

using namespace damage_done;

namespace
{

int failures = 0;
#define CHECK(cond)                                                       \
    do                                                                    \
    {                                                                     \
        if (!(cond))                                                      \
        {                                                                 \
            ++failures;                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                 \
    } while (0)

void FirstSightingCountsNothingThenTheDropIsCredited()
{
    Tracker t;
    t.Observe({ { 1, 8, 100, 1000 } }, 1000);
    CHECK(t.Total() == 0);
    t.Observe({ { 1, 8, 100, 900 } }, 2000);
    CHECK(t.Total() == 100 && t.ByBot().at(1) == 100 && t.ByClass().at(8) == 100);
}

void BotsOnOneTargetShareTheDropAndNothingIsLost()
{
    Tracker t;
    t.Observe({ { 1, 4, 100, 1000 }, { 2, 8, 100, 1000 }, { 3, 9, 100, 1000 } }, 1000);
    t.Observe({ { 1, 4, 100, 800 }, { 2, 8, 100, 800 }, { 3, 9, 100, 800 } }, 2000);
    CHECK(t.Total() == 200);   // 67 + 67 + 66
    CHECK(t.ByBot().at(1) + t.ByBot().at(2) + t.ByBot().at(3) == 200);
    CHECK(t.ByClass().size() == 3);
}

void HealsGapsAndLostTargetsCountNothing()
{
    Tracker t;
    t.Observe({ { 1, 4, 100, 500 } }, 1000);
    t.Observe({ { 1, 4, 100, 700 } }, 2000);          // it healed
    CHECK(t.Total() == 0);
    t.Observe({ { 1, 4, 100, 100 } }, 2000 + kMaxGapMs + 1); // too long ago
    CHECK(t.Total() == 0);
    t.Observe({}, 9000);                              // out of view: the baseline goes
    t.Observe({ { 1, 4, 100, 50 } }, 10000);
    CHECK(t.Total() == 0);
    t.Observe({ { 1, 4, 0, 10 } }, 11000);            // no target: no sample
    CHECK(t.Total() == 0);
}

void FormatListsClassesAndBotsAndClearKeepsBaselines()
{
    Tracker t;
    CHECK(t.Format().empty());
    t.Observe({ { 7, 4, 100, 1000 }, { 8, 5, 200, 1000 } }, 1000);
    t.Observe({ { 7, 4, 100, 900 }, { 8, 5, 200, 700 } }, 2000);
    CHECK(t.Format() == " dmg_done_by_class=4:100:1,5:300:1 dmg_done_by_bot=7:4:100,8:5:300");
    t.Clear();
    CHECK(t.Total() == 0 && t.Format().empty());
    t.Observe({ { 7, 4, 100, 850 } }, 3000);          // the baseline survived the clear
    CHECK(t.Total() == 50);
}

} // namespace

int main()
{
    FirstSightingCountsNothingThenTheDropIsCredited();
    BotsOnOneTargetShareTheDropAndNothingIsLost();
    HealsGapsAndLostTargetsCountNothing();
    FormatListsClassesAndBotsAndClearKeepsBaselines();
    if (failures == 0)
        std::printf("damage_done_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
