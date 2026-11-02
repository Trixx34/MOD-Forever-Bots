// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the LFG fill offers (lfg_fill.h). No core needed.
#include "lfg_fill.h"

#include <cstdio>

using namespace lfg_fill;
using level_dungeon::Role;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

Candidate Bot(std::uint64_t guid, std::uint32_t level, Role role)
{
    Candidate candidate;
    candidate.Guid = guid;
    candidate.Level = level;
    candidate.RoleOf = role;
    return candidate;
}

void LoneHumanGetsOneOfEachMissingRole()
{
    // A human alone: MissingRoles gives tank, healer, then two damage dealers (four slots to a full party).
    std::vector<Candidate> const free = { Bot(1, 10, Role::Dps), Bot(2, 10, Role::Healer), Bot(3, 11, Role::Tank), Bot(4, 10, Role::Dps),
                                          Bot(5, 10, Role::Dps), Bot(6, 10, Role::Dps) };
    std::vector<Offer> const offers = PickOffers(1, 0, 0, 10, free);
    CHECK(offers.size() == 4);
    CHECK(offers.size() >= 1 && offers[0].RoleOf == Role::Tank && offers[0].Bot == 3);
    CHECK(offers.size() >= 2 && offers[1].RoleOf == Role::Healer && offers[1].Bot == 2);
    CHECK(offers.size() >= 3 && offers[2].RoleOf == Role::Dps && offers[2].Bot == 1);
    CHECK(offers.size() >= 4 && offers[3].RoleOf == Role::Dps && offers[3].Bot == 4);
}

void EachBotOfferedOnce()
{
    // Size 3 with a healer: MissingRoles gives tank, dps. The one tank candidate fills the tank slot; no damage dealer is free.
    std::vector<Candidate> const free = { Bot(7, 10, Role::Tank) };
    std::vector<Offer> const offers = PickOffers(3, 0, 1, 10, free);
    CHECK(offers.size() == 1);
    CHECK(offers.size() >= 1 && offers[0].Bot == 7 && offers[0].RoleOf == Role::Tank);
    // One dps candidate, two dps slots: it is offered once.
    std::vector<Candidate> const dps = { Bot(8, 10, Role::Dps) };
    CHECK(PickOffers(2, 1, 1, 10, dps).size() == 1);
}

void FullPartyGetsNoOffer()
{
    std::vector<Candidate> const free = { Bot(1, 10, Role::Tank), Bot(2, 10, Role::Healer), Bot(3, 10, Role::Dps) };
    CHECK(PickOffers(5, 1, 1, 10, free).empty());
}

void HasTankAndHealerOnlyNeedsDps()
{
    // Tank and healer present: only damage dealers are wanted, and a tank candidate is not offered.
    std::vector<Candidate> const free = { Bot(1, 10, Role::Tank), Bot(2, 10, Role::Dps) };
    std::vector<Offer> const offers = PickOffers(2, 1, 1, 10, free);
    CHECK(offers.size() == 1 && offers[0].RoleOf == Role::Dps && offers[0].Bot == 2);
}

void LevelBandIsRespected()
{
    // Party level 10: a bot at 4 or 16 is out of the band (+-5), a bot at 15 is in it.
    std::vector<Candidate> const free = { Bot(1, 4, Role::Tank), Bot(2, 16, Role::Tank), Bot(3, 15, Role::Tank) };
    std::vector<Offer> const offers = PickOffers(1, 0, 0, 10, free);
    CHECK(offers.size() >= 1 && offers[0].Bot == 3);
    CHECK(PickOffers(1, 0, 0, 0, free).empty());
}

void UnknownLevelCandidateIsSkipped()
{
    std::vector<Candidate> const free = { Bot(1, 0, Role::Healer) };
    CHECK(PickOffers(1, 1, 0, 10, free).empty());
}

void CooldownsGate()
{
    CHECK(PartyDue(0, 1000));
    CHECK(!PartyDue(1000, 1000 + kPartyCooldownMs - 1));
    CHECK(PartyDue(1000, 1000 + kPartyCooldownMs));
    CHECK(BotDue(0, 1000));
    CHECK(!BotDue(1000, 1000 + kBotCooldownMs - 1));
    CHECK(BotDue(1000, 1000 + kBotCooldownMs));
}

void OfferTextFitsAWhisper()
{
    for (Role const role : { Role::Tank, Role::Healer, Role::Dps })
    {
        std::string const text = OfferText(role);
        CHECK(!text.empty());
        CHECK(text.size() <= 255);
        CHECK(text.find('{') == std::string::npos);
    }
    CHECK(OfferText(Role::Tank).find("tank") != std::string::npos);
    CHECK(OfferText(Role::Healer).find("heal") != std::string::npos);
}

} // namespace

int main()
{
    LoneHumanGetsOneOfEachMissingRole();
    EachBotOfferedOnce();
    FullPartyGetsNoOffer();
    HasTankAndHealerOnlyNeedsDps();
    LevelBandIsRespected();
    UnknownLevelCandidateIsSkipped();
    CooldownsGate();
    OfferTextFitsAWhisper();
    if (failures == 0)
        std::printf("lfg_fill_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
