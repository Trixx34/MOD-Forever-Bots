// The bot ramp: how many bots the module wants at a given time into the run, and when the run stops. Pure logic that takes
// elapsed time as input, so it is unit-tested without a clock (tests/ramp_test.cpp).
//
// The target starts at Initial bots, adds Step bots every StepMinutes minutes, and never exceeds Cap. The run stops at the
// first StopTime (local time of day) after it starts, and never adds bots after that.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

struct RampPlan
{
    std::uint32_t Initial = 300;
    std::uint32_t Step = 100;
    std::uint32_t StepMinutes = 15;
    std::uint32_t Cap = 2500;
    std::uint32_t StopSecOfDay = 6 * 3600; // 06:00 local time
};

// Bots wanted `elapsedMs` after the run started.
std::uint32_t RampTarget(RampPlan const& plan, std::uint64_t elapsedMs);

// True once the run has reached its stop time. The stop is the first StopSecOfDay after the start, so a run that starts
// at 22:00 with a 06:00 stop runs 8 hours. A start exactly at the stop time runs a full day.
bool RunStopped(RampPlan const& plan, std::uint32_t startSecOfDay, std::uint64_t elapsedMs);

// Seconds since local midnight for "HH:MM" or "HH:MM:SS" (24-hour). Empty if the text is not a clock time.
std::optional<std::uint32_t> ParseClock(std::string const& text);
