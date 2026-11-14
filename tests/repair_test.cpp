// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the urgent repair decisions (src/repair_policy.h). No core or SDK needed.
#include "repair_policy.h"

#include <cstdio>

using namespace repair;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

Facts Worn(std::uint32_t lowest, bool afterDeath = false)
{
    Facts facts;
    facts.LowestPct = lowest;
    facts.RepairmanKnown = true;
    facts.AfterDeath = afterDeath;
    facts.Copper = 5000;
    facts.Level = 10;
    return facts;
}

// The default: one broken piece is enough (the prod paladin had six); a worn but working piece is left to the routine visit.
void AnyBrokenPieceIsUrgent()
{
    Settings const settings;
    CHECK(settings.Enabled && settings.BrokenAtPct == 0);
    CHECK(Needed(settings, Worn(0)) == Why::Broken);
    CHECK(Needed(settings, Worn(1)) == Why::None);
    CHECK(Needed(settings, Worn(100)) == Why::None);
    Settings near = settings;
    near.BrokenAtPct = 10; // RepairBrokenPct = 10: near zero counts too
    CHECK(Needed(near, Worn(10)) == Why::Broken);
    CHECK(Needed(near, Worn(11)) == Why::None);
}

// No repairman on the map, or the rule off: nothing to do.
void NoRepairmanOrOffIsNoTrip()
{
    Settings settings;
    Facts facts = Worn(0);
    facts.RepairmanKnown = false;
    CHECK(Needed(settings, facts) == Why::None);
    settings.Enabled = false;
    CHECK(Needed(settings, Worn(0)) == Why::None);
}

// After a death the worn gear is repaired before the next fight: below AfterDeathBelowPct (50), not above; 0 turns it off.
void AfterADeathWornGearIsRepaired()
{
    Settings settings;
    CHECK(Needed(settings, Worn(40, true)) == Why::AfterDeath);
    CHECK(Needed(settings, Worn(40, false)) == Why::None);
    CHECK(Needed(settings, Worn(60, true)) == Why::None);
    CHECK(Needed(settings, Worn(0, true)) == Why::Broken); // broken wins
    settings.AfterDeathBelowPct = 0;
    CHECK(Needed(settings, Worn(40, true)) == Why::None);
}

// Short of money (under level x LowCopperPerLevel), the visit sells the white junk too; level 0 counts as 1.
void LowMoneySellsJunkFirst()
{
    Settings const settings; // 100 copper per level
    Facts facts = Worn(0);
    facts.Copper = 0;
    CHECK(SellJunkFirst(settings, facts));
    facts.Copper = 999;
    CHECK(SellJunkFirst(settings, facts));
    facts.Copper = 1000;
    CHECK(!SellJunkFirst(settings, facts));
    facts.Level = 0;
    facts.Copper = 99;
    CHECK(SellJunkFirst(settings, facts));
    facts.Copper = 100;
    CHECK(!SellJunkFirst(settings, facts));
}

// A trip that repaired nothing doubles the wait, up to the cap: a broken bot with no money never loops.
void FailedTripsBackOffToTheCap()
{
    Settings const settings; // 300 s, at most an hour
    CHECK(BackoffMs(settings, 0) == 300000);
    CHECK(BackoffMs(settings, 1) == 300000);
    CHECK(BackoffMs(settings, 2) == 600000);
    CHECK(BackoffMs(settings, 3) == 1200000);
    CHECK(BackoffMs(settings, 5) == 3600000);
    CHECK(BackoffMs(settings, 1000) == 3600000);
    Settings slow = settings;
    slow.RetryMs = 7200000; // a retry above the cap is kept, not cut
    slow.MaxBackoffMs = 3600000;
    CHECK(BackoffMs(slow, 4) == 7200000);
}

void ImprovedMeansTheWorstPieceIsBetter()
{
    CHECK(Improved(0, 100));
    CHECK(Improved(0, 1));
    CHECK(!Improved(0, 0)); // could not pay: the core repaired nothing
    CHECK(!Improved(40, 40));
}

} // namespace

int main()
{
    AnyBrokenPieceIsUrgent();
    NoRepairmanOrOffIsNoTrip();
    AfterADeathWornGearIsRepaired();
    LowMoneySellsJunkFirst();
    FailedTripsBackOffToTheCap();
    ImprovedMeansTheWorstPieceIsBetter();
    if (failures == 0)
        std::printf("repair_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
