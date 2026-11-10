// SPDX-License-Identifier: GPL-2.0-or-later
// Line pools for the advisor features (trade ads, party mana, whisper replies, guild milestones): plain text, one line each, "#" starts a comment,
// lines over 255 bytes are skipped. Pure; unit-tested in tests/trade_ads_test.cpp.
#pragma once

#include <istream>
#include <memory>
#include <string>
#include <vector>

namespace advisor {

std::vector<std::string> ParseLines(std::istream& in);
std::vector<std::string> LoadLines(std::string const& path);   // empty when the file is missing

// Replaces every "{key}" in `text` by `value`.
std::string Replace(std::string text, std::string const& key, std::string const& value);

struct Pools
{
    std::vector<std::string> TradeAds;
    std::vector<std::string> PartyMana;
    std::vector<std::string> WhisperReplies;      // keyword|state|text, see whisper_matrix.h
    std::vector<std::string> GuildMilestones;
    std::vector<std::string> CityGreetings;       // /say to a player nearby in a capital
    std::vector<std::string> AuctionTalk;         // the same at the auction house
};

}  // namespace advisor
