// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the scenario party cut (party_cut.h), including the empty-directors guard of the Tier 2 pump. No core needed.
#include "party_cut.h"

#include <cstdio>

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

// No director (no scenario configured): nobody is in a party, and nothing is indexed out of an empty director list.
void NoDirectorsMeansNoParty()
{
    CHECK(party_cut::PartyOf({ 10, 11, 12 }, 5, 0).empty());
    CHECK(party_cut::PartyOf({}, 5, 0).empty());
    CHECK(party_cut::PartyOf({ 10 }, 1, 0).empty());
}

// Bots in guid order are cut into parties of partySize.
void BotsAreCutIntoPartiesInOrder()
{
    auto const cut = party_cut::PartyOf({ 1, 2, 3, 4, 5, 6 }, 2, 3);
    CHECK(cut.size() == 6);
    CHECK(cut.at(1) == 0 && cut.at(2) == 0 && cut.at(3) == 1 && cut.at(4) == 1 && cut.at(5) == 2 && cut.at(6) == 2);
}

// More bots than the directors cover: the overflow joins the last party, never an index past the end.
void OverflowJoinsTheLastParty()
{
    auto const cut = party_cut::PartyOf({ 1, 2, 3, 4, 5 }, 2, 2);
    CHECK(cut.at(4) == 1 && cut.at(5) == 1);
    auto const one = party_cut::PartyOf({ 1, 2, 3, 4, 5, 6, 7 }, 5, 1);
    for (auto const& entry : one)
        CHECK(entry.second == 0);
}

// No bots: an empty map, with or without directors.
void NoBotsMeansEmptyMap()
{
    CHECK(party_cut::PartyOf({}, 5, 2).empty());
}

} // namespace

int main()
{
    NoDirectorsMeansNoParty();
    BotsAreCutIntoPartiesInOrder();
    OverflowJoinsTheLastParty();
    NoBotsMeansEmptyMap();
    if (failures == 0)
        std::printf("party_cut_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
