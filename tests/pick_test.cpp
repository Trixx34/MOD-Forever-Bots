// Unit tests for the random race and class pick. No core needed.
#include "bot_pick.h"

#include <cstdio>
#include <set>
#include <utility>

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

void TableMatchesTheForeverGrid()
{
    // Counts from the Wowhead Forever table: Warrior works for every race, Druid for four, and the Skyborne split.
    int warriors = 0;
    int druids = 0;
    for (RaceClass const& combo : pick::ValidCombinations())
    {
        if (combo.Class == 1)
            ++warriors;
        if (combo.Class == 11)
            ++druids;
    }
    CHECK(warriors == 10);
    CHECK(druids == 4);
    CHECK(pick::ValidCombinations().size() == 56);

    // Spot checks: the stars and the faction-only Skyborne classes.
    CHECK(pick::IsValidCombination(5, 2));    // Undead Paladin (starred)
    CHECK(pick::IsValidCombination(7, 5));    // Gnome Priest (starred)
    CHECK(pick::IsValidCombination(8, 9));    // Troll Warlock (starred)
    CHECK(pick::IsValidCombination(95, 8));   // Skyborne Mage, Alliance
    CHECK(pick::IsValidCombination(96, 7));   // Skyborne Shaman, Horde
    CHECK(!pick::IsValidCombination(96, 8));  // Skyborne Mage is Alliance only
    CHECK(!pick::IsValidCombination(95, 7));  // Skyborne Shaman is Horde only
    CHECK(!pick::IsValidCombination(1, 7));   // Human has no Shaman
    CHECK(!pick::IsValidCombination(5, 3));   // Undead has no Hunter
    CHECK(!pick::IsValidCombination(2, 2));   // Orc has no Paladin
}

void TeamFollowsTheRace()
{
    for (RaceClass const& combo : pick::ValidCombinations())
    {
        CHECK(pick::IsAllianceRace(combo.Race) != pick::IsHordeRace(combo.Race)); // exactly one faction
    }
    CHECK(pick::IsAllianceRace(1) && pick::IsAllianceRace(7) && pick::IsAllianceRace(95));
    CHECK(pick::IsHordeRace(2) && pick::IsHordeRace(8) && pick::IsHordeRace(96));
}

void PicksAreValidAndFollowTheFixedValues()
{
    pick::Stream stream(7);
    for (int i = 0; i < 500; ++i)
    {
        RaceClass out{};
        std::string reason;
        CHECK(pick::Pick(stream, pick::kRandom, pick::kRandom, out, reason));
        CHECK(pick::IsValidCombination(out.Race, out.Class));
    }

    // A fixed race: only that race's classes come up.
    for (int i = 0; i < 200; ++i)
    {
        RaceClass out{};
        std::string reason;
        CHECK(pick::Pick(stream, 5, pick::kRandom, out, reason));
        CHECK(out.Race == 5);
        CHECK(pick::IsValidCombination(5, out.Class));
    }

    // A fixed class: only the races that have it.
    for (int i = 0; i < 200; ++i)
    {
        RaceClass out{};
        std::string reason;
        CHECK(pick::Pick(stream, pick::kRandom, 11, out, reason));
        CHECK(out.Class == 11);
    }

    // A fixed pair that is not valid is refused, with a reason.
    RaceClass out{};
    std::string reason;
    CHECK(!pick::Pick(stream, 1, 7, out, reason));
    CHECK(!reason.empty());
}

void SameSeedSameBots()
{
    pick::Stream a(20261012);
    pick::Stream b(20261012);
    pick::Stream c(20261013);
    bool allSame = true;
    bool anyDifferent = false;
    for (int i = 0; i < 300; ++i)
    {
        RaceClass fromA{};
        RaceClass fromB{};
        RaceClass fromC{};
        std::string reason;
        pick::Pick(a, pick::kRandom, pick::kRandom, fromA, reason);
        pick::Pick(b, pick::kRandom, pick::kRandom, fromB, reason);
        pick::Pick(c, pick::kRandom, pick::kRandom, fromC, reason);
        allSame = allSame && fromA.Race == fromB.Race && fromA.Class == fromB.Class;
        anyDifferent = anyDifferent || fromA.Race != fromC.Race || fromA.Class != fromC.Class;
    }
    CHECK(allSame);       // the same seed reproduces the run
    CHECK(anyDifferent);  // another seed gives another run
}

void EveryCombinationComesUp()
{
    pick::Stream stream(99);
    std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
    for (int i = 0; i < 20000; ++i)
    {
        RaceClass out{};
        std::string reason;
        pick::Pick(stream, pick::kRandom, pick::kRandom, out, reason);
        seen.insert({ out.Race, out.Class });
    }
    CHECK(seen.size() == pick::ValidCombinations().size());
}

void ClassTableRemembersEachBot()
{
    pick::ClassTable table;
    table.Set(101, 1);
    table.Set(102, 8);
    CHECK(table.Get(101) == 1);
    CHECK(table.Get(102) == 8);
    CHECK(table.Get(103) == 0); // unknown bots read as 0
    table.Set(101, 5);          // a later set replaces the class
    CHECK(table.Get(101) == 5);
    CHECK(table.GetRace(101) == 0); // no race set yet
    table.SetRace(101, 2);
    CHECK(table.GetRace(101) == 2 && table.Get(101) == 5); // the race and the class are kept apart
    CHECK(table.Size() == 2);
}

} // namespace

int main()
{
    TableMatchesTheForeverGrid();
    TeamFollowsTheRace();
    PicksAreValidAndFollowTheFixedValues();
    SameSeedSameBots();
    EveryCombinationComesUp();
    ClassTableRemembersEachBot();

    if (failures == 0)
        std::printf("pick_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
