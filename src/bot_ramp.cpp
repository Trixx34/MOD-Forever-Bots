// SPDX-License-Identifier: GPL-2.0-or-later
#include "bot_ramp.h"

#include <algorithm>
#include <charconv>
#include <string_view>

namespace {

constexpr std::uint32_t kSecondsPerDay = 24 * 3600;

// Parses a field of digits with a maximum value. Returns false if the field is empty, too long, or out of range.
bool ParseField(std::string_view text, std::uint32_t max, std::uint32_t& value)
{
    if (text.empty() || text.size() > 2)
        return false;
    auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc() && end == text.data() + text.size() && value <= max;
}

} // namespace

std::uint32_t RampTarget(RampPlan const& plan, std::uint64_t elapsedMs)
{
    std::uint64_t target = plan.Initial;
    std::uint64_t const stepMs = std::uint64_t(plan.StepMinutes) * 60 * 1000;
    if (stepMs != 0)
        target += std::uint64_t(plan.Step) * (elapsedMs / stepMs);
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(target, plan.Cap));
}

bool RunStopped(RampPlan const& plan, std::uint32_t startSecOfDay, std::uint64_t elapsedMs)
{
    // Seconds from the start to the first stop time after it; a start exactly at the stop time waits a full day.
    std::uint32_t untilStop = (plan.StopSecOfDay + kSecondsPerDay - startSecOfDay % kSecondsPerDay) % kSecondsPerDay;
    if (untilStop == 0)
        untilStop = kSecondsPerDay;
    return elapsedMs >= std::uint64_t(untilStop) * 1000;
}

std::optional<std::uint32_t> ParseClock(std::string const& text)
{
    // HH:MM or HH:MM:SS
    if (text.size() != 5 && text.size() != 8)
        return std::nullopt;
    if (text[2] != ':' || (text.size() == 8 && text[5] != ':'))
        return std::nullopt;

    std::uint32_t hours = 0;
    std::uint32_t minutes = 0;
    std::uint32_t seconds = 0;
    if (!ParseField(std::string_view(text).substr(0, 2), 23, hours) ||
        !ParseField(std::string_view(text).substr(3, 2), 59, minutes))
        return std::nullopt;
    if (text.size() == 8 && !ParseField(std::string_view(text).substr(6, 2), 59, seconds))
        return std::nullopt;
    return hours * 3600 + minutes * 60 + seconds;
}
