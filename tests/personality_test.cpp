// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for bot personalities (src/personality.h): off by default, traits are stable per bot and spread over bots, every effect is bounded, and a strength of 0
// switches its effect off.
#include "personality.h"

#include <algorithm>
#include <cstdio>

using namespace personality;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

constexpr std::uint64_t kSeed = 20261114;

Settings On()
{
    Settings settings;
    settings.Enabled = true;
    return settings;
}

void OffByDefaultChangesNothing()
{
    Settings const off;
    Traits traits;
    traits.Caution = 100;
    traits.Pace = 0;
    traits.Patience = 100;
    traits.Sociability = 100;
    traits.Session = 100;
    CHECK(!off.Enabled);
    CHECK(RestPct(off, traits, 90) == 90);
    CHECK(IdleDwellMs(off, traits, 30000) == 30000);
    CHECK(ReactionDelayMs(off, traits, kSeed, 1, 0) == 0);
    CHECK(PackCap(off, traits, 3) == 3);
    CHECK(!MakesMistake(off, traits, kSeed, 1, 0));
    CHECK(AfkChancePct(off, traits, 10) == 10);
    CHECK(AfkStayMs(off, traits, 300000) == 300000);
}

void TraitsAreStablePerBotAndSpread()
{
    Traits const a = TraitsFor(kSeed, 1001);
    Traits const again = TraitsFor(kSeed, 1001);
    CHECK(a.Pace == again.Pace && a.Caution == again.Caution && a.Sociability == again.Sociability && a.Session == again.Session && a.Patience == again.Patience);
    CHECK(TraitsFor(kSeed + 1, 1001).Caution != a.Caution || TraitsFor(kSeed + 1, 1001).Pace != a.Pace);
    std::uint32_t low = 0, high = 0;
    for (std::uint64_t bot = 1; bot <= 1000; ++bot)
    {
        Traits const t = TraitsFor(kSeed, bot);
        CHECK(t.Pace <= 100 && t.Caution <= 100 && t.Sociability <= 100 && t.Session <= 100 && t.Patience <= 100);
        low += t.Caution < 30;
        high += t.Caution > 70;
    }
    CHECK(low > 200 && low < 400 && high > 200 && high < 400);
}

void RestThresholdIsBounded()
{
    Settings settings = On();
    settings.RestStrength = 100;
    for (std::uint32_t base : { 70u, 80u, 90u, 98u })
        for (std::uint32_t caution = 0; caution <= 100; caution += 10)
        {
            Traits traits;
            traits.Caution = caution;
            std::uint32_t const value = RestPct(settings, traits, base);
            CHECK(value >= kRestFloorPct && value <= kRestCeilPct);
            CHECK(value + 9 > base && value < base + 9);
        }
    Traits cautious, bold;
    cautious.Caution = 100;
    bold.Caution = 0;
    CHECK(RestPct(settings, cautious, 90) > RestPct(settings, bold, 90));
    settings.RestStrength = 0;
    CHECK(RestPct(settings, cautious, 90) == 90);
}

void IdleDwellStaysWithinSixtyPercent()
{
    Settings settings = On();
    settings.IdleStrength = 100;
    Traits patient, hasty, mid;
    patient.Patience = 100; patient.Pace = 0;
    hasty.Patience = 0; hasty.Pace = 100;
    CHECK(IdleDwellMs(settings, patient, 30000) == 48000);
    CHECK(IdleDwellMs(settings, hasty, 30000) == 12000);
    CHECK(IdleDwellMs(settings, mid, 30000) == 30000);
    settings.IdleStrength = 0;
    CHECK(IdleDwellMs(settings, patient, 30000) == 30000);
}

void ReactionDelayScalesWithPaceAndIsCapped()
{
    Settings settings = On();
    settings.ReactionMaxMs = 3000;
    Traits fast, slow;
    fast.Pace = 100;
    slow.Pace = 0;
    std::uint64_t longest = 0;
    for (std::uint64_t counter = 0; counter < 500; ++counter)
    {
        CHECK(ReactionDelayMs(settings, fast, kSeed, 5, counter) == 0);
        std::uint64_t const delay = ReactionDelayMs(settings, slow, kSeed, 5, counter);
        CHECK(delay <= 3000);
        longest = std::max(longest, delay);
        CHECK(delay == ReactionDelayMs(settings, slow, kSeed, 5, counter));
    }
    CHECK(longest > 2500);
    settings.ReactionMaxMs = 999999;
    CHECK(ReactionDelayMs(settings, slow, kSeed, 5, 1) <= kMaxReactionMs);
    settings.ReactionMaxMs = 0;
    CHECK(ReactionDelayMs(settings, slow, kSeed, 5, 1) == 0);
}

void PackCapIsOneLessOnlyForWaryBots()
{
    Settings settings = On();
    settings.CautionStrength = 50;
    Traits wary, calm;
    wary.Caution = 80;
    calm.Caution = 60;
    CHECK(PackCap(settings, wary, 3) == 2);
    CHECK(PackCap(settings, calm, 3) == 3);
    CHECK(PackCap(settings, wary, 1) == 1);
    settings.CautionStrength = 0;
    CHECK(PackCap(settings, wary, 3) == 3);
}

void MistakeChanceFollowsCautionAndIsSmall()
{
    Settings settings = On();
    settings.MistakeMaxPct = 10;
    Traits careful, reckless;
    careful.Caution = 100;
    reckless.Caution = 0;
    std::uint32_t mistakes = 0;
    for (std::uint64_t counter = 0; counter < 10000; ++counter)
    {
        CHECK(!MakesMistake(settings, careful, kSeed, 9, counter));
        mistakes += MakesMistake(settings, reckless, kSeed, 9, counter);
    }
    CHECK(mistakes > 800 && mistakes < 1200);
    settings.MistakeMaxPct = 999;   // clamped to 30
    mistakes = 0;
    for (std::uint64_t counter = 0; counter < 10000; ++counter)
        mistakes += MakesMistake(settings, reckless, kSeed, 9, counter);
    CHECK(mistakes < 3300);
    settings.MistakeMaxPct = 0;
    CHECK(!MakesMistake(settings, reckless, kSeed, 9, 1));
}

void AfkScalesWithSociabilityAndSession()
{
    Settings settings = On();
    settings.SocialStrength = 100;
    Traits social, loner;
    social.Sociability = 100; social.Session = 100;
    loner.Sociability = 0; loner.Session = 0;
    CHECK(AfkChancePct(settings, social, 10) == 20);
    CHECK(AfkChancePct(settings, loner, 10) == 0);
    CHECK(AfkChancePct(settings, social, 80) == 100);
    CHECK(AfkStayMs(settings, social, 300000) == 600000);
    CHECK(AfkStayMs(settings, loner, 300000) >= 30000);
    settings.SocialStrength = 0;
    CHECK(AfkChancePct(settings, social, 10) == 10 && AfkStayMs(settings, social, 300000) == 300000);
}

} // namespace

int main()
{
    OffByDefaultChangesNothing();
    TraitsAreStablePerBotAndSpread();
    RestThresholdIsBounded();
    IdleDwellStaysWithinSixtyPercent();
    ReactionDelayScalesWithPaceAndIsCapped();
    PackCapIsOneLessOnlyForWaryBots();
    MistakeChanceFollowsCautionAndIsSmall();
    AfkScalesWithSociabilityAndSession();
    if (failures == 0)
        std::printf("personality_test: OK\n");
    return failures == 0 ? 0 : 1;
}
