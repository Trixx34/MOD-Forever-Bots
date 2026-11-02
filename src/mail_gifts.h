// SPDX-License-Identifier: GPL-2.0-or-later
// Mail gifts (config key MailGifts, off by default): an idle bot sends one small item from its bags to a player it was told to thank, at most
// MailGiftDailyCap times a day. Pure logic. The SDK has no bot-to-player mail action (the auction mail path only carries auction results), so the
// policy logs the decision (event gift_gap) until a gift action is added to the core and SDK.
#pragma once

#include <cstdint>

namespace gifts {

struct Settings
{
    bool Enabled = false;
    std::uint32_t DailyCap = 1;        // MailGiftDailyCap: gifts per bot per day
};

struct State
{
    std::uint64_t DayStartMs = 0;
    std::uint32_t SentToday = 0;
};

struct Context
{
    bool Idle = false;
    bool InCombat = false;
    std::uint32_t GiftableItems = 0;   // small, non-soulbound, non-quest items in the bags
    bool RecipientKnown = false;
};

enum class Action { None, Send };

constexpr std::uint64_t kDayMs = 86400000ull;

Action Decide(Settings const& settings, State& state, Context const& ctx, std::uint64_t nowMs);

}  // namespace gifts
