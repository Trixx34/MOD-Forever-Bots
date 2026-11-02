// SPDX-License-Identifier: GPL-2.0-or-later
#include "inn_downtime.h"

namespace inn {

Action Decide(Settings const& settings, State& state, Context const& ctx, std::uint64_t nowMs)
{
    if (!settings.Enabled)
        return Action::None;
    if (state.Resting) {
        bool full = ctx.HealthPct >= 100 && ctx.PowerPct >= 100;
        bool timeUp = nowMs - state.RestStartedMs >= std::uint64_t(settings.MaxRestMin) * 60000ull;
        if (full || timeUp || ctx.InCombat || ctx.Dead) {
            state.Resting = false;
            state.NextMs = nowMs + std::uint64_t(settings.CooldownMin) * 60000ull;
            return Action::Stop;
        }
        return Action::Rest;
    }
    bool low = ctx.HealthPct < settings.HealthBelowPct || ctx.PowerPct < settings.PowerBelowPct;
    if (!low || !ctx.Idle || ctx.InCombat || ctx.Dead || nowMs < state.NextMs)
        return Action::None;
    if (!ctx.AtInn)
        return Action::GoToInn;
    state.Resting = true;
    state.RestStartedMs = nowMs;
    return Action::Rest;
}

}  // namespace inn
