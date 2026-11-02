// SPDX-License-Identifier: GPL-2.0-or-later
// Inn downtime (config key InnDowntime, off by default): an idle bot whose health or power is low sits down to rest, at an inn when one is near, and
// stops after InnMaxRestMin even if not full, so a stuck rest cannot hold it forever. Pure logic: the caller reports the bot's state and the clock.
// The policy's own rest (kRestBelowPct) still applies when this is off.
#pragma once

#include <cstdint>

namespace inn {

struct Settings
{
    bool Enabled = false;
    std::uint32_t HealthBelowPct = 50;  // InnHealthBelowPct
    std::uint32_t PowerBelowPct = 30;   // InnPowerBelowPct
    std::uint32_t MaxRestMin = 10;      // InnMaxRestMin: the rest stops after this long
    std::uint32_t CooldownMin = 20;     // gap before the next rest starts
};

struct State
{
    bool Resting = false;
    std::uint64_t RestStartedMs = 0;
    std::uint64_t NextMs = 0;
};

struct Context
{
    std::uint32_t HealthPct = 100;
    std::uint32_t PowerPct = 100;      // 100 for a bot without power
    bool Idle = false;
    bool InCombat = false;
    bool Dead = false;
    bool AtInn = false;                // an innkeeper is near the bot
};

enum class Action { None, GoToInn, Rest, Stop };

Action Decide(Settings const& settings, State& state, Context const& ctx, std::uint64_t nowMs);

}  // namespace inn
