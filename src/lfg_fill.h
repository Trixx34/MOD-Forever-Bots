// SPDX-License-Identifier: GPL-2.0-or-later
// LFG fill (config LfgFill): a human's party that is short of a tank or a healer is offered free bots. Pure logic, no core, unit-tested in
// tests/lfg_fill_test.cpp. The policy (bot_policy.cpp, DriveLfgFill) sends each chosen bot next to the party and the bot whispers its offer to the
// leader; the human invites the bot, and the usual invite accept joins it.
#pragma once

#include "level_dungeon.h"

#include <cstdint>
#include <string>
#include <vector>

namespace lfg_fill {

constexpr std::uint32_t kLevelBand = 5;                           // a free bot is offered when its level is within this of the party's highest member
constexpr std::uint64_t kPartyCooldownMs = 5ull * 60 * 1000;      // a party is offered bots again after this long
constexpr std::uint64_t kBotCooldownMs = 10ull * 60 * 1000;       // a bot is not offered again within this long

struct Candidate
{
    std::uint64_t Guid = 0;
    std::uint32_t Level = 0;
    level_dungeon::Role RoleOf = level_dungeon::Role::Dps;
};

struct Offer
{
    std::uint64_t Bot = 0;
    level_dungeon::Role RoleOf = level_dungeon::Role::Dps;
};

// Offers for one party: `size` members (the human leader included), `tanks` and `healers` of them. The roles the party is missing (MissingRoles,
// in its order) are filled one by one: for each, the first candidate of that role within kLevelBand of partyLevel. A bot is offered at most once.
// Empty when the party is full or its level is not known (partyLevel 0).
std::vector<Offer> PickOffers(std::uint32_t size, std::uint32_t tanks, std::uint32_t healers, std::uint32_t partyLevel, std::vector<Candidate> const& free);

// Whether a party (or a bot) may be offered again at nowMs. A last offer of 0 means never offered.
bool PartyDue(std::uint64_t lastMs, std::uint64_t nowMs);
bool BotDue(std::uint64_t lastMs, std::uint64_t nowMs);

// The whisper a bot sends to the leader for its offer. Always under 255 bytes.
std::string OfferText(level_dungeon::Role role);

} // namespace lfg_fill
