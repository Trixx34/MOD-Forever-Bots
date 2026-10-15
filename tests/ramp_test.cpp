// Unit tests for the bot ramp schedule and the stop time. No clock needed: elapsed time is an input.
#include "bot_ramp.h"

#include <cstdio>

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

constexpr std::uint64_t kMinute = 60 * 1000;
constexpr std::uint64_t kHour = 60 * kMinute;

void RampStartsAtInitialAndStepsEveryFifteenMinutes()
{
    RampPlan plan; // 300, +100 every 15 minutes, cap 1500
    CHECK(RampTarget(plan, 0) == 300);
    CHECK(RampTarget(plan, 15 * kMinute - 1) == 300);
    CHECK(RampTarget(plan, 15 * kMinute) == 400);
    CHECK(RampTarget(plan, 30 * kMinute) == 500);
    CHECK(RampTarget(plan, 2 * kHour + 59 * kMinute) == 1400); // eleven steps
    CHECK(RampTarget(plan, 3 * kHour) == 1500);                 // twelfth step reaches the cap
}

void RampNeverExceedsTheCap()
{
    RampPlan plan;
    CHECK(RampTarget(plan, 100 * kHour) == 2500);

    RampPlan capped;
    capped.Cap = 700;
    CHECK(RampTarget(capped, 100 * kHour) == 700);

    RampPlan initialAboveCap;
    initialAboveCap.Initial = 900;
    initialAboveCap.Cap = 500;
    CHECK(RampTarget(initialAboveCap, 0) == 500);
}

void ZeroStepHoldsTheInitialTarget()
{
    RampPlan plan;
    plan.Step = 0;
    CHECK(RampTarget(plan, 10 * kHour) == 300);
}

void RunStopsAtTheNextStopTime()
{
    RampPlan plan;
    plan.StopSecOfDay = 6 * 3600; // 06:00

    // Started at 22:00: eight hours to 06:00.
    std::uint32_t const at22 = 22 * 3600;
    CHECK(!RunStopped(plan, at22, 8 * 3600 * 1000 - 1000));
    CHECK(RunStopped(plan, at22, 8 * 3600 * 1000));

    // Started at 05:00: one hour.
    std::uint32_t const at05 = 5 * 3600;
    CHECK(!RunStopped(plan, at05, 59 * kMinute));
    CHECK(RunStopped(plan, at05, kHour));

    // Started exactly at 06:00: a full day, not zero.
    std::uint32_t const at06 = 6 * 3600;
    CHECK(!RunStopped(plan, at06, 23 * kHour));
    CHECK(RunStopped(plan, at06, 24 * kHour));
}

void ClockTimesParse()
{
    CHECK(ParseClock("06:00") == 21600u);
    CHECK(ParseClock("22:30:15") == 81015u);
    CHECK(ParseClock("00:00") == 0u);
    CHECK(!ParseClock("24:00").has_value());
    CHECK(!ParseClock("12:60").has_value());
    CHECK(!ParseClock("6:00").has_value());
    CHECK(!ParseClock("06-00").has_value());
    CHECK(!ParseClock("").has_value());
    CHECK(!ParseClock("06:00:99").has_value());
}

} // namespace

int main()
{
    RampStartsAtInitialAndStepsEveryFifteenMinutes();
    RampNeverExceedsTheCap();
    ZeroStepHoldsTheInitialTarget();
    RunStopsAtTheNextStopTime();
    ClockTimesParse();

    if (failures == 0)
        std::printf("ramp_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
