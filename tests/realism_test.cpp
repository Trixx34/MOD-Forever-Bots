// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the realism rules (src/realism.h): idle fidgeting, the reading delay, the passing-player buff and town AFK. No core or SDK needed.
#include "realism.h"

#include <cstdio>
#include <set>

using namespace realism;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

FidgetSettings FidgetOn()
{
    FidgetSettings settings;
    settings.Enabled = true;
    settings.MinGapSec = 1;
    settings.MaxGapSec = 1;
    return settings;
}

// Item 2.
void FidgetIsOffByDefault()
{
    FidgetSettings off;
    FidgetState state;
    for (std::uint64_t t = 0; t < 100000; t += 1000)
        CHECK(NextFidget(off, state, 1, 5, t) == Fidget::None);
}

void TwoBotsWithTheSameSeedAndClockPickDifferentActions()
{
    FidgetSettings settings = FidgetOn();
    settings.ChancePct = 100;
    FidgetState a, b;
    int differ = 0, acted = 0;
    for (std::uint64_t t = 1000; t < 200000; t += 1000)
    {
        Fidget const x = NextFidget(settings, a, 7, 101, t);
        Fidget const y = NextFidget(settings, b, 7, 202, t);
        differ += x != y;
        acted += x != Fidget::None;
    }
    CHECK(acted > 50);
    CHECK(differ > 20);
}

void FidgetChanceAndWeightsMatchTheConfig()
{
    FidgetSettings settings = FidgetOn();
    settings.ChancePct = 30;
    settings.StandWeight = 2;
    settings.StepWeight = 1;
    settings.TurnWeight = 1;
    int checks = 0, acted = 0, stand = 0, step = 0, turn = 0;
    for (std::uint64_t bot = 1; bot <= 40; ++bot)
    {
        FidgetState state;
        for (std::uint64_t t = 1000; t <= 1000ull * 400; t += 1000)
        {
            Fidget const f = NextFidget(settings, state, 99, bot, t);
            if (t == 1000)
                continue;   // the first call only draws the first gap
            ++checks;
            acted += f != Fidget::None;
            stand += f == Fidget::Stand;
            step += f == Fidget::Step;
            turn += f == Fidget::Turn;
        }
    }
    double const rate = double(acted) / checks;
    CHECK(rate > 0.27 && rate < 0.33);                     // the configured 30 percent
    CHECK(acted > 0 && double(stand) / acted > 0.45 && double(stand) / acted < 0.55);   // weight 2 of 4
    CHECK(double(step) / acted > 0.20 && double(step) / acted < 0.30);
    CHECK(double(turn) / acted > 0.20 && double(turn) / acted < 0.30);
}

void FidgetWaitsForItsFirstGap()
{
    FidgetSettings settings = FidgetOn();
    settings.ChancePct = 100;
    settings.MinGapSec = 5;
    settings.MaxGapSec = 5;
    FidgetState state;
    CHECK(NextFidget(settings, state, 1, 3, 10000) == Fidget::None);   // draws the first gap
    CHECK(NextFidget(settings, state, 1, 3, 12000) == Fidget::None);   // not due yet
    CHECK(NextFidget(settings, state, 1, 3, 15000) != Fidget::None);   // due, and the chance is 100
}

// Item 3.
void ReadDelayStaysWithinTheRange()
{
    ReadSettings settings;
    settings.Enabled = true;
    std::uint64_t lo = ~0ull, hi = 0;
    for (std::uint64_t n = 0; n < 2000; ++n)
    {
        std::uint64_t const d = ReadDelayMs(settings, 5, 9 + n % 7, n);
        lo = d < lo ? d : lo;
        hi = d > hi ? d : hi;
    }
    CHECK(lo >= 3000 && hi <= 12000);
    CHECK(hi - lo > 6000);      // and it really varies
}

void NothingIsAcceptedBeforeTheDelayAndItIsAcceptedAfter()
{
    ReadSettings settings;
    settings.Enabled = true;
    ReadGate gate;
    CHECK(!ReadReady(settings, gate, 783, 1, 10, 100000));       // opens the quest
    std::uint64_t const delay = gate.DelayMs;
    CHECK(delay >= 3000 && delay <= 12000);
    CHECK(!ReadReady(settings, gate, 783, 1, 10, 100000 + delay - 1));
    CHECK(ReadReady(settings, gate, 783, 1, 10, 100000 + delay));
    Close(gate);
    CHECK(!ReadReady(settings, gate, 784, 1, 10, 200000));        // the next quest is read again
    // Another quest while one is open starts a new reading.
    CHECK(!ReadReady(settings, gate, 785, 1, 10, 200000 + 20000));
}

void ReadDelayIsOffByDefault()
{
    ReadSettings off;
    ReadGate gate;
    CHECK(ReadReady(off, gate, 1, 1, 1, 0));
}

// Item 4.
BuffFacts RichBotNearAPlayer()
{
    BuffFacts facts;
    facts.ManaUser = true;
    facts.Power = 5000;
    facts.MaxPower = 5000;
    facts.BuffCost = 300;
    facts.RotationReserve = 1500;
    facts.TargetDistance = 10.0f;
    return facts;
}

BuffSettings BuffOn()
{
    BuffSettings settings;
    settings.Enabled = true;
    return settings;
}

void NoBuffInCombatOrWithATarget()
{
    BuffFacts facts = RichBotNearAPlayer();
    CHECK(BuffAllowed(BuffOn(), facts, 1000, 0));
    BuffFacts combat = facts;
    combat.InCombat = true;
    CHECK(!BuffAllowed(BuffOn(), combat, 1000, 0));
    BuffFacts target = facts;
    target.HasTarget = true;
    CHECK(!BuffAllowed(BuffOn(), target, 1000, 0));
    BuffSettings off;
    CHECK(!BuffAllowed(off, facts, 1000, 0));
}

void NoBuffBelowTheManaThresholdOrWithoutTheRotationReserve()
{
    BuffFacts facts = RichBotNearAPlayer();
    facts.Power = 3900;            // 78 percent, under the 80 percent
    CHECK(!BuffAllowed(BuffOn(), facts, 1000, 0));
    BuffSettings loose = BuffOn();
    loose.ManaAbovePct = 0;
    facts.Power = 1799;            // the buff (300) plus the reserve (1500) do not fit
    CHECK(!BuffAllowed(loose, facts, 1000, 0));
    facts.Power = 1800;
    CHECK(BuffAllowed(loose, facts, 1000, 0));
    facts.BuffCost = 0;            // a cost the bot does not know: no buff
    CHECK(!BuffAllowed(loose, facts, 1000, 0));
}

void NoBuffWhileTheCooldownRunsOrTheTargetIsFar()
{
    BuffFacts facts = RichBotNearAPlayer();
    CHECK(!BuffAllowed(BuffOn(), facts, 5000, 9000));        // the cooldown ends at 9000
    CHECK(BuffAllowed(BuffOn(), facts, 9000, 9000));
    facts.TargetDistance = 15.5f;
    CHECK(!BuffAllowed(BuffOn(), facts, 9000, 0));
}

void ReserveIsTheDearestCasts()
{
    CHECK(ReserveMana({ 100, 400, 200, 50 }, 2) == 600);
    CHECK(ReserveMana({ 100 }, 4) == 100);
    CHECK(ReserveMana({}, 4) == 0);
    CHECK(ReserveMana({ 100, 200 }, 0) == 0);
}

void TheZoneRateLimitsBuffsPerMinute()
{
    ZoneRate rate;
    CHECK(rate.TryGrant(1, 0, 2));
    CHECK(rate.TryGrant(1, 1000, 2));
    CHECK(!rate.TryGrant(1, 2000, 2));
    CHECK(rate.TryGrant(2, 2000, 2));          // another map has its own rate
    CHECK(rate.TryGrant(1, 61000, 2));         // the minute has passed
    CHECK(!rate.TryGrant(1, 5000, 0));
}

// Item 5.
void AfkStayIsWithinTheConfiguredRange()
{
    AfkSettings settings;
    settings.Enabled = true;
    std::uint64_t lo = ~0ull, hi = 0;
    for (std::uint64_t n = 0; n < 2000; ++n)
    {
        std::uint64_t const d = AfkDurationMs(settings, 3, 40 + n % 13, n);
        lo = d < lo ? d : lo;
        hi = d > hi ? d : hi;
    }
    CHECK(lo >= 5 * 60000ull && hi <= 20 * 60000ull);
    CHECK(hi - lo > 10 * 60000ull);
}

void AfkChanceMatchesTheConfigAndIsOffByDefault()
{
    AfkSettings off;
    CHECK(!AfkRoll(off, 1, 1, 1));
    AfkSettings settings;
    settings.Enabled = true;
    settings.ChancePct = 10;
    int hits = 0;
    for (std::uint64_t n = 0; n < 20000; ++n)
        hits += AfkRoll(settings, 4, 1 + n % 50, n);
    CHECK(hits > 1700 && hits < 2300);
}

void AfkZonesKeepTheNumberUnderTheCap()
{
    AfkZones zones;
    CHECK(zones.TryStart(1, 10, 0, 600000, 2));
    CHECK(zones.TryStart(1, 11, 0, 600000, 2));
    CHECK(!zones.TryStart(1, 12, 0, 600000, 2));         // two already stay on map 1
    CHECK(zones.TryStart(2, 12, 0, 600000, 2));          // another map
    CHECK(zones.Count(1, 1000) == 2);
    zones.End(10);                                       // one leaves early (a fight)
    CHECK(zones.TryStart(1, 12, 1000, 700000, 2));
    CHECK(zones.Count(1, 700000) == 0);                  // all stays have run out by then
    CHECK(zones.TryStart(1, 13, 700000, 800000, 2));
}

} // namespace

int main()
{
    FidgetIsOffByDefault();
    TwoBotsWithTheSameSeedAndClockPickDifferentActions();
    FidgetChanceAndWeightsMatchTheConfig();
    FidgetWaitsForItsFirstGap();
    ReadDelayStaysWithinTheRange();
    NothingIsAcceptedBeforeTheDelayAndItIsAcceptedAfter();
    ReadDelayIsOffByDefault();
    NoBuffInCombatOrWithATarget();
    NoBuffBelowTheManaThresholdOrWithoutTheRotationReserve();
    NoBuffWhileTheCooldownRunsOrTheTargetIsFar();
    ReserveIsTheDearestCasts();
    TheZoneRateLimitsBuffsPerMinute();
    AfkStayIsWithinTheConfiguredRange();
    AfkChanceMatchesTheConfigAndIsOffByDefault();
    AfkZonesKeepTheNumberUnderTheCap();
    if (failures == 0)
        std::printf("realism_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
