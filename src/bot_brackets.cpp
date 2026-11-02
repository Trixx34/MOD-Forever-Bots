// SPDX-License-Identifier: GPL-2.0-or-later
#include "bot_brackets.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace brackets {

namespace {

std::string Trim(std::string const& text)
{
    auto const first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos)
        return {};
    auto const last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

// Index of a bracket label, or kCount when the label is unknown.
std::size_t IndexOfLabel(std::string const& label)
{
    for (std::size_t b = 0; b < kCount; ++b)
        if (Label(b) == label)
            return b;
    return kCount;
}

} // namespace

std::size_t BracketOf(std::uint32_t level)
{
    if (level >= 60)
        return kCount - 1;
    if (level < 10)
        return 0;
    return std::min<std::size_t>(level / 10, kCount - 2);
}

Range RangeOf(std::size_t bracket)
{
    if (bracket >= kCount - 1)
        return { 60, 60 };
    if (bracket == 0)
        return { 1, 9 };
    std::uint32_t const low = std::uint32_t(bracket) * 10;
    return { low, low + 9 };
}

std::string Label(std::size_t bracket)
{
    if (bracket >= kCount - 1)
        return "60";
    Range const range = RangeOf(bracket);
    return std::to_string(range.Low) + "-" + std::to_string(range.High);
}

bool ParseTargets(std::string const& text, Shares& out, std::string& error)
{
    Shares weights{};
    std::array<bool, kCount> listed{};
    std::size_t start = 0;
    while (start <= text.size())
    {
        std::size_t const comma = text.find(',', start);
        std::string const item = Trim(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        start = comma == std::string::npos ? text.size() + 1 : comma + 1;
        if (item.empty())
            continue;
        std::size_t const colon = item.find(':');
        if (colon == std::string::npos)
        {
            error = "'" + item + "' is not bracket:weight";
            return false;
        }
        std::string const label = Trim(item.substr(0, colon));
        std::string const number = Trim(item.substr(colon + 1));
        std::size_t const bracket = IndexOfLabel(label);
        if (bracket == kCount)
        {
            error = "'" + label + "' is not a bracket (1-9, 10-19, 20-29, 30-39, 40-49, 50-59 or 60)";
            return false;
        }
        if (listed[bracket])
        {
            error = "bracket " + label + " is listed twice";
            return false;
        }
        char* end = nullptr;
        double const weight = std::strtod(number.c_str(), &end);
        if (number.empty() || end != number.c_str() + number.size() || !std::isfinite(weight) || weight < 0.0)
        {
            error = "weight '" + number + "' for " + label + " is not a number of 0 or more";
            return false;
        }
        listed[bracket] = true;
        weights[bracket] = weight;
    }
    double const total = weights[0] + weights[1] + weights[2] + weights[3] + weights[4] + weights[5] + weights[6];
    if (total <= 0.0)
    {
        error = "no bracket has a weight above 0";
        return false;
    }
    for (std::size_t b = 0; b < kCount; ++b)
        out[b] = weights[b] / total;
    return true;
}

bool ParseRetire(std::string const& text, Retire& out, std::string& error)
{
    if (text == "off")
    {
        out = Retire::kOff;
        return true;
    }
    if (text == "release")
    {
        out = Retire::kRelease;
        return true;
    }
    error = "'" + text + "' is not off or release (a retire that removes the character needs a core action, which does not exist yet)";
    return false;
}

double Deficit(Shares const& shares, Counts const& counts, std::size_t bracket)
{
    std::uint64_t total = 0;
    for (std::uint32_t const count : counts)
        total += count;
    return shares[bracket] * double(total + 1) - double(counts[bracket]);
}

std::size_t NextBracket(Shares const& shares, Counts const& counts)
{
    std::size_t most = kCount;
    double mostDeficit = -std::numeric_limits<double>::infinity();
    for (std::size_t b = 0; b < kCount; ++b)
    {
        if (shares[b] <= 0.0)
            continue;
        double const deficit = Deficit(shares, counts, b);
        if (deficit > 0.0)
            return b; // the lowest bracket still below its share
        if (deficit > mostDeficit)
        {
            mostDeficit = deficit;
            most = b;
        }
    }
    return most;
}

std::uint32_t LevelIn(std::size_t bracket, double unit)
{
    Range const range = RangeOf(bracket);
    std::uint32_t const width = range.High - range.Low + 1;
    double const clamped = std::clamp(unit, 0.0, 1.0 - 1e-12);
    return range.Low + std::min<std::uint32_t>(width - 1, std::uint32_t(clamped * width));
}

std::optional<std::size_t> WeightedSpot(std::vector<std::uint32_t> const& weights, std::vector<std::uint32_t> const& load, std::uint32_t cap, double unit)
{
    double total = 0.0;
    std::vector<double> eligible(weights.size(), 0.0);
    for (std::size_t i = 0; i < weights.size(); ++i)
    {
        if (cap != 0 && i < load.size() && load[i] >= cap)
            continue;
        eligible[i] = double(std::max<std::uint32_t>(weights[i], 1));
        total += eligible[i];
    }
    if (total <= 0.0)
        return std::nullopt;
    double target = std::clamp(unit, 0.0, 1.0 - 1e-12) * total;
    std::size_t last = std::numeric_limits<std::size_t>::max();
    for (std::size_t i = 0; i < eligible.size(); ++i)
    {
        if (eligible[i] <= 0.0)
            continue;
        last = i;
        if (target < eligible[i])
            return i;
        target -= eligible[i];
    }
    return last == std::numeric_limits<std::size_t>::max() ? std::nullopt : std::optional<std::size_t>(last);
}

std::vector<std::size_t> UnderCap(std::vector<std::size_t> const& spots, std::vector<std::uint32_t> const& load, std::uint32_t cap, bool& fellBack)
{
    fellBack = false;
    if (cap == 0)
        return spots;
    std::vector<std::size_t> open;
    for (std::size_t spot : spots)
        if (spot >= load.size() || load[spot] < cap)
            open.push_back(spot);
    if (open.empty())
    {
        fellBack = true;
        return spots;
    }
    return open;
}

void LevelWatch::Observe(std::uint64_t guid, std::uint32_t level, std::uint64_t nowMs)
{
    auto const [it, added] = _bots.try_emplace(guid);
    Entry& entry = it->second;
    if (added || entry.Level != level)
    {
        entry.Level = level;
        entry.LevelSinceMs = nowMs;
    }
}

bool LevelWatch::Stalled(std::uint64_t guid, std::uint64_t nowMs, std::uint32_t stalledMinutes) const
{
    auto const it = _bots.find(guid);
    if (stalledMinutes == 0 || it == _bots.end())
        return false;
    return nowMs - it->second.LevelSinceMs >= std::uint64_t(stalledMinutes) * 60000;
}

bool LevelWatch::MayMoveDown(std::uint64_t guid, std::uint64_t nowMs, std::uint64_t periodMs) const
{
    auto const it = _bots.find(guid);
    if (it == _bots.end())
        return false;
    return it->second.MovedMs == 0 || nowMs - it->second.MovedMs >= periodMs;
}

void LevelWatch::NoteMoveDown(std::uint64_t guid, std::uint64_t nowMs)
{
    auto const it = _bots.find(guid);
    if (it == _bots.end())
        return;
    it->second.MovedMs = nowMs == 0 ? 1 : nowMs;
    // The clock restarts: a bot moved down gets a full period to show a level.
    it->second.LevelSinceMs = nowMs;
}

} // namespace brackets
