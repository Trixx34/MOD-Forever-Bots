// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for bot personalities (src/personality.h): off by default, traits are stable per bot and spread over bots, every effect is bounded, and a strength of 0
// switches its effect off.
#include "personality.h"

#include <algorithm>
#include <chrono>
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
    CHECK(GreetChancePct(off, traits, 3) == 3);
    CHECK(GuildPollMs(off, traits, 60000) == 60000);
    CHECK(SessionMs(off, traits, 3600000) == 3600000 && SessionPct(off, traits) == 100);
    CHECK(JitterMs(off, traits, false, kSeed, 1, 0) == 0);
    CHECK(!ForgetsBuff(off, traits, false, kSeed, 1, 0));
    Settings keysOff;   // the new keys have their own off switches
    keysOff.Enabled = true;
    keysOff.SocialStrength = 0;
    keysOff.SessionStrength = 0;
    keysOff.JitterMaxMs = 0;
    keysOff.ForgetBuffPct = 0;
    CHECK(GreetChancePct(keysOff, traits, 3) == 3 && GuildPollMs(keysOff, traits, 60000) == 60000);
    CHECK(SessionMs(keysOff, traits, 3600000) == 3600000 && SessionPct(keysOff, traits) == 100);
    CHECK(JitterMs(keysOff, traits, false, kSeed, 1, 0) == 0 && !ForgetsBuff(keysOff, traits, false, kSeed, 1, 0));
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

void SessionLengthScalesWithTheSessionTrait()
{
    Settings const settings = On();
    Traits longSession, shortSession;
    longSession.Session = 100;
    shortSession.Session = 0;
    CHECK(SessionMs(settings, longSession, 100000) == 130000);    // 1 + 0.6 * 0.5 (default strength 50)
    CHECK(SessionMs(settings, shortSession, 100000) == 70000);
    CHECK(SessionPct(settings, longSession) == 130 && SessionPct(settings, shortSession) == 70);
    Settings full = settings;
    full.SessionStrength = 100;
    CHECK(SessionMs(full, longSession, 100000) == 160000 && SessionMs(full, shortSession, 100000) == 40000);   // the bounds
    std::uint32_t low = 100, high = 100;
    for (std::uint64_t bot = 1; bot <= 1000; ++bot)
    {
        std::uint32_t const pct = SessionPct(full, TraitsFor(kSeed, bot));
        CHECK(pct >= 40 && pct <= 160);
        low = std::min(low, pct);
        high = std::max(high, pct);
    }
    CHECK(low < 50 && high > 150);   // it is spread over the bots
    CHECK(SessionPct(full, TraitsFor(kSeed, 77)) == SessionPct(full, TraitsFor(kSeed, 77)));
}

void SocialTraitScalesGreetingsAndGuildLooks()
{
    Settings const settings = On();
    Traits shy, chatty;
    shy.Sociability = 0;
    chatty.Sociability = 100;
    CHECK(GreetChancePct(settings, shy, 10) == 5 && GreetChancePct(settings, chatty, 10) == 15);   // +-50 percent at the default strength
    CHECK(GreetChancePct(settings, chatty, 90) == 100);                                           // never above 100
    CHECK(GuildPollMs(settings, chatty, 60000) < GuildPollMs(settings, shy, 60000));
    Settings full = settings;
    full.SocialStrength = 100;
    CHECK(GuildPollMs(full, chatty, 60000) == 30000 && GuildPollMs(full, shy, 60000) == 90000);   // within [0.5, 1.5]
    CHECK(GreetChancePct(full, shy, 10) == 0 && GreetChancePct(full, chatty, 10) == 20);
}

void JitterIsBoundedByPaceAndLocation()
{
    Settings settings = On();
    settings.JitterMaxMs = 500;
    Traits slow, fast;
    slow.Pace = 0;
    fast.Pace = 100;
    std::uint64_t top = 0;
    for (std::uint64_t n = 0; n < 2000; ++n)
    {
        std::uint64_t const delay = JitterMs(settings, slow, false, kSeed, 5, n);
        CHECK(delay <= 500);
        top = std::max(top, delay);
        CHECK(JitterMs(settings, fast, false, kSeed, 5, n) == 0);   // the fastest bot does not hesitate
        CHECK(delay == JitterMs(settings, slow, false, kSeed, 5, n));   // repeatable
    }
    CHECK(top > 400);                                               // the whole range is used
    // The key's own cap, whatever the setting says.
    settings.JitterMaxMs = 99999;
    for (std::uint64_t n = 0; n < 200; ++n)
        CHECK(JitterMs(settings, slow, false, kSeed, 5, n) <= kMaxJitterMs);
    // Dungeons and raids: nothing unless the key allows it.
    settings.JitterMaxMs = 500;
    std::uint64_t instanced = 0;
    for (std::uint64_t n = 0; n < 200; ++n)
        instanced += JitterMs(settings, slow, true, kSeed, 5, n);
    CHECK(instanced == 0 && !ImperfectionAllowed(settings, true) && ImperfectionAllowed(settings, false));
    settings.Instances = true;
    for (std::uint64_t n = 0; n < 200; ++n)
        instanced += JitterMs(settings, slow, true, kSeed, 5, n);
    CHECK(instanced > 0 && ImperfectionAllowed(settings, true));
}

void ForgottenBuffsAreRareBoundedAndCautionDependent()
{
    Settings settings = On();
    Traits careless, careful;
    careless.Caution = 0;
    careful.Caution = 100;
    std::uint32_t carelessCount = 0, carefulCount = 0, instancedCount = 0;
    for (std::uint64_t n = 0; n < 20000; ++n)
    {
        carelessCount += ForgetsBuff(settings, careless, false, kSeed, 9, n);
        carefulCount += ForgetsBuff(settings, careful, false, kSeed, 9, n);
        instancedCount += ForgetsBuff(settings, careless, true, kSeed, 9, n);
        std::uint64_t const forgotten = ForgetMs(kSeed, 9, n);
        CHECK(forgotten >= kForgetMinMs && forgotten <= kForgetMaxMs);
    }
    CHECK(carelessCount > 400 && carelessCount < 800);   // 3 percent of 20000 is 600
    CHECK(carefulCount == 0);                            // a fully cautious bot never forgets
    CHECK(instancedCount == 0);                          // not in a dungeon or raid
    settings.ForgetBuffPct = 99;                         // the cap of the key: 30 percent
    std::uint32_t capped = 0;
    for (std::uint64_t n = 0; n < 20000; ++n)
        capped += ForgetsBuff(settings, careless, false, kSeed, 9, n);
    CHECK(capped < 6600);
    settings.Instances = true;
    for (std::uint64_t n = 0; n < 20000; ++n)
        instancedCount += ForgetsBuff(settings, careless, true, kSeed, 9, n);
    CHECK(instancedCount > 0);
}

// The cost of a bot's personality per decide tick, with every effect on: the traits are drawn (as the policy does at each use), the rest, idle, pull, greeting, guild,
// jitter and forget rules asked once each. That is more than a tick needs (the policy asks for the traits at three of these sites, and only when the site is reached),
// so it is an upper bound. The result is printed; the limit is generous so a loaded machine does not fail it, and still well under the budget (prod: 17 us a bot).
void CostPerBotTickIsSmall()
{
    Settings const settings = On();
    constexpr std::uint32_t kBots = 800;
    constexpr std::uint32_t kTicks = 200;
    std::uint64_t sink = 0;
    auto const start = std::chrono::steady_clock::now();
    for (std::uint32_t tick = 0; tick < kTicks; ++tick)
        for (std::uint64_t bot = 1; bot <= kBots; ++bot)
        {
            Traits const traits = TraitsFor(kSeed, bot);
            sink += RestPct(settings, traits, 90) + IdleDwellMs(settings, traits, 30000) + ReactionDelayMs(settings, traits, kSeed, bot, tick) + PackCap(settings, traits, 3) +
                    MakesMistake(settings, traits, kSeed, bot, tick) + GreetChancePct(settings, traits, 3) + GuildPollMs(settings, traits, 60000) + SessionPct(settings, traits) +
                    JitterMs(settings, traits, false, kSeed, bot, tick) + ForgetsBuff(settings, traits, false, kSeed, bot, tick);
        }
    double const ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()) / (double(kBots) * kTicks);
    std::printf("personality_test: all rules, traits drawn once: %.0f ns per bot tick (%.2f percent of 17 us); sink %llu\n", ns, ns / 170.0, static_cast<unsigned long long>(sink & 1));
    CHECK(ns < 1700.0);   // 10 percent of the 17 us decide time, an upper bound far above what a tick uses
    // The policy's common tick (a bot standing about): the traits once, the idle dwell once.
    std::uint64_t sink2 = 0;
    auto const start2 = std::chrono::steady_clock::now();
    for (std::uint32_t tick = 0; tick < kTicks; ++tick)
        for (std::uint64_t bot = 1; bot <= kBots; ++bot)
            sink2 += IdleDwellMs(settings, TraitsFor(kSeed, bot), 30000);
    double const common = double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start2).count()) / (double(kBots) * kTicks);
    std::printf("personality_test: idle-dwell tick (traits + one rule): %.0f ns per bot tick (%.2f percent of 17 us); sink %llu\n", common, common / 170.0, static_cast<unsigned long long>(sink2 & 1));
    CHECK(common < 850.0);   // 5 percent
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
    SessionLengthScalesWithTheSessionTrait();
    SocialTraitScalesGreetingsAndGuildLooks();
    JitterIsBoundedByPaceAndLocation();
    ForgottenBuffsAreRareBoundedAndCautionDependent();
    CostPerBotTickIsSmall();
    if (failures == 0)
        std::printf("personality_test: OK\n");
    return failures == 0 ? 0 : 1;
}
