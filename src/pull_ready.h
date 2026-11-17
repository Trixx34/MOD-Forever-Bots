// SPDX-License-Identifier: GPL-2.0-or-later
// The ready gate before a pull (bot AI plan, overnight job 2026-11-16): a bot that is short of mana, or has an upkeep buff to cast, waits before it starts a fight, for at most a
// set time. Pure helpers; the policy's turn reads them (bot_policy_pullready.cpp), the tests call them directly (tests/ai_next_test.cpp).
#pragma once

#include <algorithm>
#include <cstdint>

namespace pull_ready
{

// One wait: since when it holds, and whether it has given up (a timed-out wait does not start again until the need has been over once).
struct Gate
{
    std::uint64_t SinceMs = 0;
    std::uint64_t LogMs = 0;
    bool GaveUp = false;
};

// The mana a waiting bot rests up to: the start mark plus this much, at most 100.
constexpr std::uint32_t kManaReleaseGapPct = 20;

// Whether a mana user needs to wait: below `startPct` when it is not waiting yet, below `startPct` + the gap while it waits (so it does not start a fight the moment it passes the mark).
inline bool ManaNeeded(std::uint32_t power, std::uint32_t maxPower, std::uint32_t startPct, bool waiting)
{
    if (startPct == 0 || maxPower == 0)
        return false;
    std::uint32_t const mark = waiting ? std::min<std::uint32_t>(100, startPct + kManaReleaseGapPct) : startPct;
    return std::uint64_t(power) * 100 < std::uint64_t(maxPower) * mark;
}

// One step of the wait. Returns whether the bot holds now. `needed` false ends the wait and clears the give-up. A wait that has run `maxMs` gives up (timedOut is set once, on that step).
inline bool Hold(Gate& gate, bool needed, std::uint64_t nowMs, std::uint64_t maxMs, bool& timedOut)
{
    timedOut = false;
    if (!needed)
    {
        gate = Gate{};
        return false;
    }
    if (gate.GaveUp)
        return false;
    if (gate.SinceMs == 0)
        gate.SinceMs = nowMs;
    if (nowMs - gate.SinceMs >= maxMs)
    {
        gate.GaveUp = true;
        timedOut = true;
        return false;
    }
    return true;
}

// Whether a wait logs now (at most once per `everyMs`).
inline bool LogDue(Gate& gate, std::uint64_t nowMs, std::uint64_t everyMs = 10000)
{
    if (gate.LogMs != 0 && nowMs - gate.LogMs < everyMs)
        return false;
    gate.LogMs = nowMs;
    return true;
}

} // namespace pull_ready
