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

namespace gifts {

int PickGift(std::vector<BagStack> const& stacks, std::uint64_t seed)
{
    std::vector<int> fit;
    for (std::size_t i = 0; i < stacks.size(); ++i)
        if (stacks[i].ItemGuid != 0 && stacks[i].Count >= kMinGiftCount && stacks[i].Count <= kMaxGiftCount)
            fit.push_back(int(i));
    if (fit.empty())
        return -1;
    return fit[std::size_t(seed % fit.size())];
}

std::string GiftSubject(std::string const& itemName)
{
    std::string subject = "A thank-you gift: " + itemName;
    if (subject.size() > 64)
        subject.resize(64);
    return subject;
}

}  // namespace gifts
