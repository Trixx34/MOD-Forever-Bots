// SPDX-License-Identifier: GPL-2.0-or-later
#include "repair_policy.h"

#include <algorithm>

namespace repair {

Why Needed(Settings const& settings, Facts const& facts)
{
    if (!settings.Enabled || !facts.RepairmanKnown || facts.LowestPct >= 100)
        return Why::None;
    if (facts.LowestPct <= settings.BrokenAtPct)
        return Why::Broken;
    if (facts.AfterDeath && facts.LowestPct < settings.AfterDeathBelowPct)
        return Why::AfterDeath;
    return Why::None;
}

bool SellJunkFirst(Settings const& settings, Facts const& facts)
{
    std::uint64_t const level = std::max<std::uint32_t>(facts.Level, 1);
    return facts.Copper < level * settings.LowCopperPerLevel;
}

std::uint64_t BackoffMs(Settings const& settings, std::uint32_t failures)
{
    std::uint64_t wait = settings.RetryMs;
    for (std::uint32_t i = 1; i < failures && wait < settings.MaxBackoffMs; ++i)
        wait *= 2;
    return std::min(std::max(wait, settings.RetryMs), std::max(settings.MaxBackoffMs, settings.RetryMs));
}

bool Improved(std::uint32_t beforePct, std::uint32_t afterPct)
{
    return afterPct > beforePct;
}

char const* WhyName(Why why)
{
    switch (why)
    {
        case Why::Broken:
            return "broken";
        case Why::AfterDeath:
            return "after_death";
        case Why::None:
            break;
    }
    return "none";
}

} // namespace repair
