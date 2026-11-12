// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the heal rank ladders (src/heal_rank.h, generated src/heal_ranks_data.h): which rank covers a deficit, and the shape of the generated table.
#include "heal_rank.h"

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

void LadderShape()
{
    // within a chain the ranks rise in level, mana and amount (what PickRank relies on)
    heal_ranks::Rank const* previous = nullptr;
    for (heal_ranks::Rank const& rank : heal_ranks::kRanks)
    {
        if (previous && previous->Chain == rank.Chain)
            CHECK(rank.Level >= previous->Level && rank.Mana > previous->Mana && rank.Amount > previous->Amount);
        CHECK(rank.Amount > 0 && rank.Mana > 0);
        previous = &rank;
    }
    CHECK(heal_ranks::IsLadderSpell(2054) && heal_ranks::IsLadderSpell(25357) && !heal_ranks::IsLadderSpell(585));
}

void PicksTheLowestRankThatCoversTheDeficit()
{
    CHECK(heal_ranks::PickRank(5, 30, 100) == 2054u);          // priest level 30, Heal: 292 covers 100
    CHECK(heal_ranks::PickRank(5, 30, 400) == 2055u);          // 406
    CHECK(heal_ranks::PickRank(5, 30, 500) == 6063u);          // 524
    CHECK(heal_ranks::PickRank(5, 30, 5000) == 6063u);         // nothing covers it: the highest rank the level has (6064 is level 34)
    CHECK(heal_ranks::PickRank(5, 34, 5000) == 6064u);
    CHECK(heal_ranks::PickRank(5, 10, 100) == 2053u);          // below level 16 a priest heals with Lesser Heal (142); 2052 heals 79
    CHECK(heal_ranks::PickRank(5, 10, 60) == 2052u);
    CHECK(heal_ranks::PickRank(5, 0, 1) == 2050u);             // level 0: no level limit, the chain of a bot below 16
    CHECK(heal_ranks::PickRank(2, 60, 1000) == 10329u);        // paladin Holy Light: 900 does not cover 1000, 1218 (level 54) does
    CHECK(heal_ranks::PickRank(2, 50, 1000) == 10328u);        // at level 50 the 900 of rank 7 is the highest there is
    CHECK(heal_ranks::PickRank(7, 60, 1500) == 25357u);        // shaman Healing Wave
    CHECK(heal_ranks::PickRank(11, 8, 90) == 5186u);           // druid Healing Touch: 101 covers 90
    CHECK(heal_ranks::PickRank(1, 60, 100) == 0u);             // a warrior has no ladder
}

void MaxesOutOnManaAndLevel()
{
    CHECK(heal_ranks::PickRank(5, 30, 500, 300) == 6063u);     // 6063 costs 255 mana and covers 500
    CHECK(heal_ranks::PickRank(5, 30, 500, 250) == 2055u);     // 6063 is not affordable: the highest rank that is (406, short of 500)
    CHECK(heal_ranks::PickRank(5, 30, 100, 100) == 0u);        // 2054 costs 155: nothing is affordable
    CHECK(heal_ranks::PickRank(5, 30, 500, 255) == 6063u);
}

}

int main()
{
    LadderShape();
    PicksTheLowestRankThatCoversTheDeficit();
    MaxesOutOnManaAndLevel();
    if (failures == 0)
        std::printf("ok\n");
    return failures == 0 ? 0 : 1;
}
