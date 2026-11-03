// SPDX-License-Identifier: GPL-2.0-or-later
// Mail gifts (config key MailGifts, off by default): an idle bot sends one small item from its bags to a player it was told to thank, at most
// MailGiftDailyCap times a day. Pure logic. The policy (TurnSocial) turns a decision into social::Order;
// main_commands.cpp lists the bot's bags (BotTrade kListBags), PickGift chooses the stack and ModuleClient::BotGiftMail sends it. A core without the gift
// action refuses it and the policy logs the event gift_gap.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

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

// One stack from the bot's bag list (BotTrade kListBags: only items the core says can be traded, so never soulbound).
struct BagStack
{
    std::uint64_t ItemGuid = 0;
    std::uint32_t Entry = 0;
    std::uint32_t Count = 0;
    std::string Name;
};

// A gift is a small stack of goods: at least 2 units (a stackable item, so never a single piece of gear) and at most kMaxGiftCount. The core refuses
// a quest item, a bound item and anything above its own value cap (Bot.Social.GiftMaxCopper) when the action runs. `seed` (the bot's guid plus the
// day) spreads the choice over the stacks. Returns the index into `stacks`, or -1 when none is fit to give.
constexpr std::uint32_t kMinGiftCount = 2;
constexpr std::uint32_t kMaxGiftCount = 20;
int PickGift(std::vector<BagStack> const& stacks, std::uint64_t seed);
// The mail's subject, 1 to 64 bytes.
std::string GiftSubject(std::string const& itemName);

}  // namespace gifts
