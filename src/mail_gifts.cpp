// SPDX-License-Identifier: GPL-2.0-or-later
#include "mail_gifts.h"

namespace gifts {

Action Decide(Settings const& settings, State& state, Context const& ctx, std::uint64_t nowMs)
{
    if (!settings.Enabled)
        return Action::None;
    if (nowMs - state.DayStartMs >= kDayMs) {
        state.DayStartMs = nowMs;
        state.SentToday = 0;
    }
    if (state.SentToday >= settings.DailyCap || !ctx.Idle || ctx.InCombat || ctx.GiftableItems == 0 || !ctx.RecipientKnown)
        return Action::None;
    ++state.SentToday;
    return Action::Send;
}

}  // namespace gifts
