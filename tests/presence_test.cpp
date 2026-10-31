// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the presence scheduler (src/bot_presence.h). No core or SDK needed.
#include "bot_presence.h"

#include <algorithm>
#include <cstdio>
#include <set>

using namespace presence;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

constexpr std::uint64_t kMin = 60000;
constexpr std::uint32_t kNoon = 12 * 3600;
constexpr std::uint32_t kEvening = 20 * 3600;   // the default curve's peak
constexpr std::uint32_t kNight = 4 * 3600;

Settings On()
{
    Settings settings;
    settings.Enabled = true;
    settings.DayPercent.fill(100);   // the time of day does not matter unless a test says so
    settings.MaxChangesPerTick = 1000;
    return settings;
}

// A world of `count` bots whose presence is carried out by the test the way the module does: a log off removes the bot from the online list, a
// log on puts it back.
struct World
{
    Scheduler Sched;
    std::set<std::uint64_t> OnlineSet;
    std::set<std::uint64_t> Busy;
    std::uint64_t Now = 0;
    std::uint32_t Sec = kEvening;
    std::uint32_t LogOffs = 0;
    std::uint32_t LogOns = 0;

    World(Settings const& settings, std::uint32_t count, std::uint64_t seed = 7) : Sched(settings, seed)
    {
        for (std::uint32_t i = 1; i <= count; ++i)
            OnlineSet.insert(i);
    }

    Decisions Step(std::uint64_t advanceMs = 5000)
    {
        Now += advanceMs;
        std::vector<Online> list;
        for (std::uint64_t guid : OnlineSet)
            list.push_back({ guid, Busy.count(guid) == 0 });
        Decisions decisions = Sched.Tick(Now, Sec, list);
        for (std::uint64_t guid : decisions.LogOff)
            OnlineSet.erase(guid), ++LogOffs;
        for (std::uint64_t guid : decisions.LogOn)
            OnlineSet.insert(guid), ++LogOns;
        return decisions;
    }

    void RunMinutes(std::uint32_t minutes)
    {
        for (std::uint64_t end = Now + minutes * kMin; Now < end;)
            Step();
    }
};

void NothingHappensUnlessEnabled()
{
    Settings off;
    World world(off, 20);
    world.RunMinutes(600);
    CHECK(world.LogOffs == 0 && world.LogOns == 0 && world.OnlineSet.size() == 20);
}

void SessionsLastTheirTimeAndBotsComeBack()
{
    Settings settings = On();
    settings.OnlineMinMin = 60;
    settings.OnlineMaxMin = 120;
    settings.OfflineMinMin = 30;
    settings.OfflineMaxMin = 60;
    World world(settings, 50);
    world.RunMinutes(59);
    CHECK(world.LogOffs > 0);                       // the first sessions are partly over at the start, so some leave early
    world.RunMinutes(24 * 60);
    CHECK(world.LogOffs > 100 && world.LogOns > 100);
    // Everything that left came back: at any time at most the ones in an absence are missing.
    CHECK(world.OnlineSet.size() + world.Sched.OfflineCount() >= 50);
    CHECK(world.OnlineSet.size() <= 50 && world.OnlineSet.size() > 10);
}

void ABotIsNeverLoggedOffWhileBusy()
{
    Settings settings = On();
    settings.OnlineMinMin = 1;
    settings.OnlineMaxMin = 2;
    World world(settings, 10);
    for (std::uint64_t guid = 1; guid <= 10; ++guid)
        world.Busy.insert(guid);
    world.RunMinutes(300);
    CHECK(world.LogOffs == 0 && world.OnlineSet.size() == 10);
    // Once idle, a bot that was due goes within the retry time.
    world.Busy.erase(3);
    std::vector<std::uint64_t> left;
    for (int i = 0; i < 12 && left.empty(); ++i)
        left = world.Step().LogOff;
    CHECK(left.size() == 1 && left[0] == 3);
}

void ABusyBotIsAskedAgainSoonNotEveryStep()
{
    Settings settings = On();
    settings.OnlineMinMin = 1;
    settings.OnlineMaxMin = 1;
    World world(settings, 1);
    world.Busy.insert(1);
    world.RunMinutes(3);
    world.Busy.erase(1);
    // Busy at its due time: it was pushed back by RetryMs (30 s), so it leaves within that, not at once on every step.
    std::uint64_t waited = 0;
    while (world.OnlineSet.count(1) && waited < 10 * kMin)
    {
        world.Step(1000);
        waited += 1000;
    }
    CHECK(!world.OnlineSet.count(1) && waited <= 31000);
}

void TheMinimumStaysOnline()
{
    Settings settings = On();
    settings.MinOnline = 8;
    settings.OnlineMinMin = 1;
    settings.OnlineMaxMin = 2;
    settings.OfflineMinMin = 200;
    settings.OfflineMaxMin = 300;
    World world(settings, 20);
    std::size_t lowest = 99;
    for (int i = 0; i < 12 * 60 * 12; ++i)
    {
        world.Step();
        lowest = std::min(lowest, world.OnlineSet.size());
    }
    CHECK(lowest >= 8);
    CHECK(world.LogOffs >= 12);
}

void TheCapOnBotsAtOnceHolds()
{
    Settings settings = On();
    settings.MaxOnline = 12;
    World world(settings, 20);
    std::size_t highest = 0;
    for (int i = 0; i < 24 * 60 * 12; ++i)
    {
        world.Step();
        highest = std::max(highest, world.OnlineSet.size());
    }
    // Twenty bots exist but only twelve may be around: the rest are sent away at the start and take turns.
    CHECK(highest <= 20);
    CHECK(world.OnlineSet.size() <= 12 + 1);
    CHECK(world.LogOffs >= 8);
}

void TheCrowdFollowsTheTimeOfDay()
{
    Settings settings = On();
    settings.DayPercent.fill(100);
    settings.DayPercent[4] = 20;      // the small hours: a fifth
    settings.DayPercent[5] = 20;
    settings.OnlineMinMin = 60;
    settings.OnlineMaxMin = 120;
    settings.OfflineMinMin = 10;      // short absences, so the evening is nearly full
    settings.OfflineMaxMin = 20;
    World world(settings, 100);
    world.Sec = kEvening;
    world.RunMinutes(240);
    std::size_t const evening = world.OnlineSet.size();
    world.Sec = kNight;
    world.RunMinutes(120);
    std::size_t const night = world.OnlineSet.size();
    world.Sec = kEvening;
    world.RunMinutes(240);
    std::size_t const morning = world.OnlineSet.size();
    CHECK(evening >= 70);
    CHECK(night <= 20 && night >= 10);    // down to the ceiling of 20 percent
    CHECK(morning >= 70);
}

void AllowedOnlineInterpolatesBetweenHours()
{
    Settings settings = On();
    settings.DayPercent.fill(0);
    settings.DayPercent[10] = 100;
    Scheduler sched(settings, 1);
    CHECK(sched.AllowedOnline(10 * 3600, 100) == 100);
    CHECK(sched.AllowedOnline(10 * 3600 + 1800, 100) == 50);   // halfway to 0 percent at 11:00
    CHECK(sched.AllowedOnline(11 * 3600, 100) == 0);
    CHECK(sched.AllowedOnline(9 * 3600 + 1800, 100) == 50);
    CHECK(sched.AllowedOnline(10 * 3600, 3) == 3);
    settings.DayPercent[10] = 1;                                // rounded up: one percent of a few bots is a bot
    Scheduler tiny(settings, 1);
    CHECK(tiny.AllowedOnline(10 * 3600, 3) == 1);
    settings.MinOnline = 5;
    settings.MaxOnline = 4;                                     // the cap wins over the minimum
    Scheduler capped(settings, 1);
    CHECK(capped.AllowedOnline(10 * 3600, 20) == 4);
    settings.MaxOnline = 0;
    Scheduler floored(settings, 1);
    CHECK(floored.AllowedOnline(11 * 3600, 20) == 5 && floored.AllowedOnline(11 * 3600, 3) == 3);
}

void ChangesPerTickAreLimited()
{
    Settings settings = On();
    settings.MaxChangesPerTick = 3;
    settings.DayPercent.fill(0);   // everyone should go
    World world(settings, 30);
    Decisions first = world.Step();
    CHECK(first.LogOff.size() == 3);
    std::size_t gone = 3;
    for (int i = 0; i < 20; ++i)
        gone += world.Step().LogOff.size();
    CHECK(gone == 30 && world.OnlineSet.empty());
}

void ABotStillInViewRightAfterLeavingIsNotAReturn()
{
    Settings settings = On();
    settings.OnlineMinMin = 1;
    settings.OnlineMaxMin = 1;
    settings.OfflineMinMin = 100;
    settings.OfflineMaxMin = 100;
    Scheduler sched(settings, 3);
    std::vector<Online> list{ { 1, true } };
    Decisions d = sched.Tick(0, kEvening, list);
    d = sched.Tick(70 * 1000, kEvening, list);
    CHECK(d.LogOff.size() == 1);
    // The core has not logged it out yet: it is still listed for a few seconds, and nothing changes.
    d = sched.Tick(75 * 1000, kEvening, list);
    CHECK(d.LogOff.empty() && d.LogOn.empty() && sched.OfflineCount() == 1);
    d = sched.Tick(90 * 1000, kEvening, list);
    CHECK(d.LogOff.empty() && sched.OfflineCount() == 1);
}

void AFailedReturnIsTriedAgain()
{
    Settings settings = On();
    settings.OfflineMinMin = 1;
    settings.OfflineMaxMin = 1;
    settings.OnlineMinMin = 1;
    settings.OnlineMaxMin = 1;
    World world(settings, 1);
    while (world.OnlineSet.count(1))
        world.Step();
    std::uint64_t returned = 0;
    while (returned == 0 && world.Now < 10 * kMin)
    {
        Decisions d = world.Step();
        if (!d.LogOn.empty())
        {
            returned = world.Now;
            world.OnlineSet.erase(1);                 // the core refused: it never shows up
            world.Sched.LogOnFailed(1, world.Now);
        }
    }
    CHECK(returned != 0);
    std::uint32_t again = 0;
    for (int i = 0; i < 20 * 12; ++i)
        again += world.Step().LogOn.empty() ? 0 : 1;
    CHECK(again >= 1);
}

void AReturningBotThatNeverAppearsIsAwayAgain()
{
    Settings settings = On();
    settings.OfflineMinMin = 1;
    settings.OfflineMaxMin = 1;
    settings.OnlineMinMin = 1;
    settings.OnlineMaxMin = 1;
    settings.LoginGraceMs = 2 * kMin;
    World world(settings, 1);
    bool refused = false;
    std::uint32_t logOns = 0;
    for (int i = 0; i < 60 * 12; ++i)
    {
        Decisions d = world.Step();
        if (!d.LogOn.empty())
        {
            ++logOns;
            if (!refused)
            {
                refused = true;
                world.OnlineSet.erase(1);             // never shows up, and nobody tells the scheduler
            }
        }
    }
    CHECK(logOns >= 2);                               // it was sent again after the grace ran out
}

void ForgottenBotsAreNotScheduled()
{
    Settings settings = On();
    Scheduler sched(settings, 1);
    sched.Tick(0, kEvening, { { 1, true }, { 2, true } });
    CHECK(sched.TrackedCount() == 2);
    sched.Forget(1);
    CHECK(sched.TrackedCount() == 1);
    // A bot that left the view without being logged off is dropped after a while.
    sched.Tick(settings.ForgetAfterMs + 1000, kEvening, {});
    CHECK(sched.TrackedCount() == 0);
}

void TheSameSeedGivesTheSameRun()
{
    Settings settings = On();
    World a(settings, 40, 99), b(settings, 40, 99), c(settings, 40, 100);
    a.RunMinutes(600);
    b.RunMinutes(600);
    c.RunMinutes(600);
    CHECK(a.OnlineSet == b.OnlineSet && a.LogOffs == b.LogOffs);
    CHECK(a.LogOffs != c.LogOffs || a.OnlineSet != c.OnlineSet);
}

} // namespace

int main()
{
    NothingHappensUnlessEnabled();
    SessionsLastTheirTimeAndBotsComeBack();
    ABotIsNeverLoggedOffWhileBusy();
    ABusyBotIsAskedAgainSoonNotEveryStep();
    TheMinimumStaysOnline();
    TheCapOnBotsAtOnceHolds();
    TheCrowdFollowsTheTimeOfDay();
    AllowedOnlineInterpolatesBetweenHours();
    ChangesPerTickAreLimited();
    ABotStillInViewRightAfterLeavingIsNotAReturn();
    AFailedReturnIsTriedAgain();
    AReturningBotThatNeverAppearsIsAwayAgain();
    ForgottenBotsAreNotScheduled();
    TheSameSeedGivesTheSameRun();
    if (failures == 0)
        std::printf("presence_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
