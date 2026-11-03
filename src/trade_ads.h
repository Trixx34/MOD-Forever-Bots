// SPDX-License-Identifier: GPL-2.0-or-later
// Trade-chat crafting ads (config TradeAds, off by default): a bot with enough skill in a crafting profession posts one templated line on the Trade channel
// every TradeAdsIntervalMin minutes. It starts no trade. Templates: data/trade_ads.txt ({profession}, {item}). Pure logic, tests/trade_ads_test.cpp.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace trade_ads {

struct Settings
{
    bool Enabled = false;                 // TradeAds
    std::uint32_t IntervalMin = 45;       // TradeAdsIntervalMin
    std::uint32_t MinSkill = 150;         // TradeAdsMinSkill: the skill in the profession at which the bot advertises
};

// The crafting skill lines of 3.3.5 and what they make; nullptr for a line that is not a crafting profession (gathering ones are not advertised).
char const* ProfessionName(std::uint32_t skillLine);
char const* ProfessionItem(std::uint32_t skillLine);

struct State
{
    std::uint64_t NextMs = 0;             // 0: draw the first time
    std::uint32_t Counter = 0;
};

// The line to post now, or "" . The bot's two professions are passed as skill line and value. The first ad comes a random part of an interval after
// the first call, so bots that start together do not post together.
std::string Next(Settings const& settings, State& state, std::vector<std::string> const& templates, std::uint32_t const skillLine[2],
                 std::uint32_t const skillValue[2], std::uint64_t seed, std::uint64_t bot, std::uint64_t nowMs);

}  // namespace trade_ads
