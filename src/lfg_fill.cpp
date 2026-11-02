// SPDX-License-Identifier: GPL-2.0-or-later
#include "lfg_fill.h"

namespace lfg_fill {

std::vector<Offer> PickOffers(std::uint32_t size, std::uint32_t tanks, std::uint32_t healers, std::uint32_t partyLevel, std::vector<Candidate> const& free)
{
    std::vector<Offer> offers;
    if (partyLevel == 0)
        return offers;
    std::vector<bool> taken(free.size(), false);
    for (level_dungeon::Role const role : level_dungeon::MissingRoles(size, tanks, healers))
    {
        for (std::size_t i = 0; i < free.size(); ++i)
        {
            Candidate const& candidate = free[i];
            bool const inBand = candidate.Level != 0 && candidate.Level + kLevelBand >= partyLevel && candidate.Level <= partyLevel + kLevelBand;
            if (taken[i] || candidate.RoleOf != role || !inBand)
                continue;
            taken[i] = true;
            offers.push_back(Offer{ candidate.Guid, role });
            break;
        }
    }
    return offers;
}

bool PartyDue(std::uint64_t lastMs, std::uint64_t nowMs)
{
    return lastMs == 0 || nowMs >= lastMs + kPartyCooldownMs;
}

bool BotDue(std::uint64_t lastMs, std::uint64_t nowMs)
{
    return lastMs == 0 || nowMs >= lastMs + kBotCooldownMs;
}

std::string OfferText(level_dungeon::Role role)
{
    switch (role)
    {
        case level_dungeon::Role::Tank: return "I can tank for your group. Invite me when you are ready.";
        case level_dungeon::Role::Healer: return "I can heal for your group. Invite me when you are ready.";
        default: return "I can deal damage for your group. Invite me when you are ready.";
    }
}

} // namespace lfg_fill
