// SPDX-License-Identifier: GPL-2.0-or-later
// Talent respec (TalentRespec setting, off by default): whether a bot whose role or spec changed after it spent talent points should have them
// taken back and spent again by the new role or spec (the core's kRespecTalents action). Pure: the policy asks it once per tick for a bot it
// is about to order talents for. Unit-tested in tests/talent_respec_test.cpp.
#pragma once

#include <cstdint>

struct RespecInputs
{
    bool Enabled = false;          // the TalentRespec setting
    std::uint32_t Level = 0;       // the bot's level (a respec is for level 10 and up, as the talent order)
    bool Alive = false;
    bool InCombat = false;
    bool PointsSpent = false;      // a talent order was sent before (TalentLevel != 0): the bot has points to take back
    std::uint32_t AppliedRole = 0; // the role and spec of that order (0 = none)
    std::uint32_t AppliedSpec = 0;
    std::uint32_t WantRole = 0;    // the role and spec the bot should have now (1 to 3 role, 0 spec = none)
    std::uint32_t WantSpec = 0;
    std::uint64_t NowMs = 0;
    std::uint64_t NotBeforeMs = 0; // no respec for this bot before this time (set after each respec: one per kRespecIntervalMs)
};

constexpr std::uint64_t kRespecIntervalMs = 3600000; // one respec per bot per hour at most
constexpr std::uint32_t kRespecMinLevel = 10;

// True when the bot's points should be respecced to the wanted role and spec now. A first spend (nothing spent yet) is not a respec: the
// talent order does it. A role or spec that is the same as the applied one is not a respec either: a level-up is spent by the talent order.
inline bool RespecDue(RespecInputs const& in)
{
    if (!in.Enabled || !in.PointsSpent || in.Level < kRespecMinLevel || !in.Alive || in.InCombat)
        return false;
    if (in.NowMs < in.NotBeforeMs)
        return false;
    bool const changed = in.WantRole != in.AppliedRole || in.WantSpec != in.AppliedSpec;
    return changed && (in.AppliedRole != 0 || in.AppliedSpec != 0);
}
