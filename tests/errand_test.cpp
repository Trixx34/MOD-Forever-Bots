// Unit tests for the town errand scheduler (src/errand_policy.h). No core or SDK needed.
#include "errand_policy.h"

#include <cstdio>
#include <string>

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

std::string TestCity(std::uint32_t city)
{
    return city == 1 ? "Stormwind" : "city" + std::to_string(city);
}

void JournalLines()
{
    Journal journal(TestCity);
    CHECK(journal.OnStart(Start{ 7, Kind::kTrain, 1 }, 1000) == "errand_start bot=7 city=Stormwind kind=train");
    auto const done = journal.OnEnd(7, EndReason::kDone, false, 91000);
    CHECK(done && *done == "errand_done bot=7 city=Stormwind kind=train took_s=90");
    CHECK(!journal.OnEnd(7, EndReason::kDone, false, 92000)); // one end per start

    journal.OnStart(Start{ 8, Kind::kAuction, 2 }, 0);
    auto const timeout = journal.OnEnd(8, EndReason::kTimeLimit, false, 3600000);
    CHECK(timeout && *timeout == "errand_timeout bot=8 city=city2 kind=auction took_s=3600");

    journal.OnStart(Start{ 9, Kind::kTrain, 1 }, 0);
    auto const called = journal.OnEnd(9, EndReason::kCalled, false, 5000);
    CHECK(called && *called == "errand_abandoned reason=called bot=9 city=Stormwind kind=train took_s=5");
    journal.OnStart(Start{ 9, Kind::kTrain, 1 }, 0);
    auto const gone = journal.OnEnd(9, EndReason::kUnavailable, false, 5000);
    CHECK(gone && gone->find("errand_abandoned reason=unavailable bot=9") == 0);
    journal.OnStart(Start{ 9, Kind::kTrain, 1 }, 0);
    auto const failed = journal.OnEnd(9, EndReason::kDone, true, 5000);
    CHECK(failed && failed->find("errand_abandoned reason=failed bot=9") == 0);

    CHECK(!journal.OnEnd(99, EndReason::kCalled, false, 1)); // never started here: no line
}

void JournalFollowsTheScheduler()
{
    Scheduler scheduler(On());
    Journal journal(TestCity);
    Decisions d = scheduler.Tick(0, { Free(1, true, false, { 1 }) });
    CHECK(d.Starts.size() == 1);
    CHECK(journal.OnStart(d.Starts[0], 0) == "errand_start bot=1 city=Stormwind kind=train");
    Bot called = Free(1, true, false, { 1 });
    called.Called = true;
    d = scheduler.Tick(kMin, { called });
    CHECK(d.Ends.size() == 1);
    auto const line = journal.OnEnd(d.Ends[0].Guid, d.Ends[0].Why, false, kMin);
    CHECK(line && *line == "errand_abandoned reason=called bot=1 city=Stormwind kind=train took_s=60");
}

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
    JournalLines();
    JournalFollowsTheScheduler();
    if (failures == 0)
        std::printf("errand_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
