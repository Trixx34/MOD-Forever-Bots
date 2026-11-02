// SPDX-License-Identifier: GPL-2.0-or-later
#include "duels.h"

namespace duels {

Action Decide(Settings const& settings, State const& state, Context const& ctx, std::uint64_t nowMs)
{
    if (!settings.Enabled || ctx.InCombat || ctx.Dead)
        return Action::None;
    // A challenge to this bot is answered whatever its cooldown: refusing costs nothing and the rival walks away otherwise.
    if (ctx.ChallengeReceived && ctx.Level >= settings.MinLevel && ctx.Idle)
        return Action::Accept;
    if (!ctx.Idle || ctx.Level < settings.MinLevel || !ctx.RivalNearby || nowMs < state.NextMs)
        return Action::None;
    return Action::Challenge;
}

void NoteStarted(State& state, Settings const& settings, std::uint64_t nowMs)
{
    state.NextMs = nowMs + std::uint64_t(settings.CooldownMin) * 60000ull;
}

}  // namespace duels
