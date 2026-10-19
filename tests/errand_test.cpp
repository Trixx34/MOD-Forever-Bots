// Unit tests for the town errand scheduler (src/errand_policy.h). No core or SDK needed.
#include "errand_policy.h"

#include <cstdio>

using namespace errand;

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
constexpr std::uint64_t kHour = 60 * kMin;

Settings On()
{
    Settings settings;
    settings.Enabled = true;
    return settings;
}

Bot Free(std::uint64_t guid, bool train, bool auction, std::vector<std::uint32_t> cities = { 1, 2 })
{
    Bot bot;
    bot.Guid = guid;
    bot.Available = true;
    bot.TrainDue = train;
    bot.AuctionDue = auction;
    bot.Cities = std::move(cities);
    return bot;
}

bool StartedFor(Decisions const& decisions, std::uint64_t guid)
{
    for (Start const& start : decisions.Starts)
        if (start.Guid == guid)
            return true;
    return false;
}

void TrainingBeforeTheHouse()
{
    Scheduler scheduler(On());
    Decisions decisions = scheduler.Tick(0, { Free(10, true, true) });
    CHECK(decisions.Starts.size() == 1);
    CHECK(decisions.Starts.size() == 1 && decisions.Starts[0].What == Kind::kTrain);
    CHECK(scheduler.Active(10));
}

void NothingDueNothingStarts()
{
    Scheduler scheduler(On());
    CHECK(scheduler.Tick(0, { Free(10, false, false) }).Starts.empty());
}

void BusyOrCalledBotsDoNotStart()
{
    Scheduler scheduler(On());
    Bot fighting = Free(10, true, false);
    fighting.Available = false;                       // in combat
    Bot partied = Free(11, true, false);
    partied.Called = true;                            // a party or a dungeon wants it
    Decisions decisions = scheduler.Tick(0, { fighting, partied });
    CHECK(decisions.Starts.empty());
}

void OffMeansNoStarts()
{
    Settings settings;
    Scheduler scheduler(settings);
    CHECK(scheduler.Tick(0, { Free(10, true, true) }).Starts.empty());
}

void SpreadsOverTheCapitals()
{
    Settings settings = On();
    settings.MaxPerCity = 1;
    settings.MaxStartsPerTick = 4;
    Scheduler scheduler(settings);
    Decisions decisions = scheduler.Tick(0, { Free(10, true, false), Free(11, true, false), Free(12, true, false) });
    CHECK(decisions.Starts.size() == 2);                       // two capitals, one bot each: the third waits
    CHECK(decisions.Starts.size() == 2 && decisions.Starts[0].City != decisions.Starts[1].City);
    CHECK(scheduler.ActiveCount(1) == 1 && scheduler.ActiveCount(2) == 1);
}

void StartsPerTickAreCapped()
{
    Settings settings = On();
    settings.MaxStartsPerTick = 1;
    settings.MaxPerCity = 5;
    Scheduler scheduler(settings);
    CHECK(scheduler.Tick(0, { Free(10, true, false), Free(11, true, false) }).Starts.size() == 1);
}

void CalledAwayAbandonsAndCharges()
{
    Scheduler scheduler(On());
    scheduler.Tick(0, { Free(10, true, false) });
    Bot called = Free(10, true, false);
    called.Called = true;
    Decisions decisions = scheduler.Tick(10 * kMin, { called });
    CHECK(decisions.Ends.size() == 1 && decisions.Ends[0].Why == EndReason::kCalled);
    CHECK(!scheduler.Active(10));
    CHECK(scheduler.MinutesUsed(10, 10 * kMin) == 10);
}

void TimeLimitEndsTheErrand()
{
    Settings settings = On();
    settings.MaxMinutes = 30;
    Scheduler scheduler(settings);
    scheduler.Tick(0, { Free(10, false, true) });
    Decisions decisions = scheduler.Tick(31 * kMin, { Free(10, false, true) });
    CHECK(decisions.Ends.size() == 1 && decisions.Ends[0].Why == EndReason::kTimeLimit);
}

void FinishedErrandIsRecorded()
{
    Scheduler scheduler(On());
    scheduler.Tick(0, { Free(10, false, true) });
    scheduler.Finish(10, 5 * kMin);
    CHECK(!scheduler.Active(10));
    CHECK(scheduler.MinutesUsed(10, 5 * kMin) == 5);
}

void GapBetweenErrands()
{
    Settings settings = On();
    settings.GapMinutes = 120;
    Scheduler scheduler(settings);
    scheduler.Tick(0, { Free(10, true, false) });
    scheduler.Finish(10, 5 * kMin);
    CHECK(!scheduler.Tick(60 * kMin, { Free(10, true, false) }).Starts.size());   // too soon after the last one
    CHECK(scheduler.Tick(125 * kMin, { Free(10, true, false) }).Starts.size() == 1);
}

void DailyBudgetResetsAfterADay()
{
    Settings settings = On();
    settings.DailyMinutes = 30;
    settings.GapMinutes = 0;
    Scheduler scheduler(settings);
    scheduler.Tick(0, { Free(10, true, false) });
    scheduler.Finish(10, 30 * kMin);                                              // the day's 30 minutes are used
    CHECK(scheduler.MinutesUsed(10, 31 * kMin) == 30);
    CHECK(scheduler.Tick(31 * kMin, { Free(10, true, false) }).Starts.empty());
    CHECK(scheduler.Tick(25 * kHour, { Free(10, true, false) }).Starts.size() == 1);   // the first errand is over a day old
}

void AbsentBotIsNotStarted()
{
    Scheduler scheduler(On());
    scheduler.Tick(0, { Free(10, true, false) });
    Decisions decisions = scheduler.Tick(kMin, {});                               // the bot left the module's view
    CHECK(decisions.Ends.size() == 1 && decisions.Ends[0].Why == EndReason::kUnavailable);
}

} // namespace

int main()
{
    TrainingBeforeTheHouse();
    NothingDueNothingStarts();
    BusyOrCalledBotsDoNotStart();
    OffMeansNoStarts();
    SpreadsOverTheCapitals();
    StartsPerTickAreCapped();
    CalledAwayAbandonsAndCharges();
    TimeLimitEndsTheErrand();
    FinishedErrandIsRecorded();
    GapBetweenErrands();
    DailyBudgetResetsAfterADay();
    AbsentBotIsNotStarted();
    if (failures == 0)
        std::printf("errand_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
