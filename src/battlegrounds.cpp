// SPDX-License-Identifier: GPL-2.0-or-later
#include "battlegrounds.h"

namespace battlegrounds {

Action Decide(Settings const& settings, State const& state, Context const& ctx, std::uint64_t nowMs)
{
    if (!settings.Enabled)
        return Action::None;
    if (state.Queued) {
        if (ctx.InBattleground)
            return Action::None;
        if (ctx.InGroup)
            return Action::Leave;   // a party pulled the bot into a dungeon or raid run: a pop would teleport it out of the group's instance
        if (nowMs >= state.QueuedAtMs && nowMs - state.QueuedAtMs >= std::uint64_t(settings.MaxWaitMin) * 60000ull)
            return Action::Leave;
        return Action::None;
    }
    if (ctx.InBattleground || ctx.InGroup || !ctx.Idle || ctx.InCombat || ctx.Level < settings.MinLevel || nowMs < state.NextMs)
        return Action::None;
    return Action::Queue;
}

void NoteQueued(State& state, std::uint64_t nowMs)
{
    state.Queued = true;
    state.QueuedAtMs = nowMs;
}

void NoteQueueSent(State& state, Settings const& settings, std::uint64_t nowMs)
{
    state.NextMs = nowMs + std::uint64_t(settings.CooldownMin) * 60000ull;
}

void NoteLeft(State& state, Settings const& settings, std::uint64_t nowMs)
{
    state.Queued = false;
    state.NextMs = nowMs + std::uint64_t(settings.CooldownMin) * 60000ull;
}

}  // namespace battlegrounds
