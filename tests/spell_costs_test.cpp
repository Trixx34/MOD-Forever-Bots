// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the spell cost data: the file parser and the cost of the highest rank a bot of a level can know. No core needed.
#include "spell_costs.h"

#include <cstdio>
#include <sstream>

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

spell_cost::SpellCostData Parse(char const* text, std::vector<std::string>& warnings)
{
    std::istringstream in(text);
    return spell_cost::ParseSpellCosts(in, warnings);
}

// A rage spell with three ranks (cost in tenths of a rage point), a mana spell priced by percent, and a spell with a per level cost.
char const* const kFile =
    "# spell_id\tlevel\tpower_type\tcost\tcost_per_level\tcost_pct\tnext_rank\tname\n"
    "5242\t1\t1\t100\t0\t0\t6192\tBattle Shout\n"
    "6192\t12\t1\t200\t0\t0\t6673\tBattle Shout\n"
    "6673\t22\t1\t300\t0\t0\t0\tBattle Shout\n"
    "\n"
    "9000\t5\t0\t0\t0\t8\t0\tA percent cost\n"
    "9100\t5\t0\t50\t3\t0\t0\tA per level cost\n"
    "9200\t1\t0\t40\t0\t0\t9200\tA loop\n";

void ParsesTheFile()
{
    std::vector<std::string> warnings;
    auto const data = Parse(kFile, warnings);
    CHECK(warnings.empty());
    CHECK(data.BySpell.size() == 6);
    CHECK(data.BySpell.at(5242).Cost == 100 && data.BySpell.at(5242).NextRank == 6192 && data.BySpell.at(5242).PowerType == 1);
    CHECK(data.BySpell.at(6673).NextRank == 0 && data.BySpell.at(6192).Level == 12);
}

void SkipsBadLinesWithAWarning()
{
    std::vector<std::string> warnings;
    auto const data = Parse("5242\t1\t1\t100\t0\t0\t0\tok\nnot a line\n7\tx\t1\t10\t0\t0\t0\tbad number\n", warnings);
    CHECK(data.BySpell.size() == 1 && data.BySpell.count(5242) == 1);
    CHECK(warnings.size() == 2);
}

void TheFirstPowerLineOfASpellWins()
{
    std::vector<std::string> warnings;
    auto const data = Parse("100\t1\t0\t30\t0\t0\t0\tmain\n100\t1\t3\t99\t0\t0\t0\tsecond power\n", warnings);
    CHECK(data.BySpell.size() == 1 && data.BySpell.at(100).Cost == 30 && data.BySpell.at(100).PowerType == 0);
}

void CostIsThatOfTheHighestRankTheBotCanKnow()
{
    std::vector<std::string> warnings;
    auto const data = Parse(kFile, warnings);
    auto cost = data.CostAtLevel(5242, 10);
    CHECK(cost && cost->Cost == 100 && cost->PowerType == 1);       // only rank 1 yet
    cost = data.CostAtLevel(5242, 12);
    CHECK(cost && cost->Cost == 200);                               // rank 2 from level 12
    cost = data.CostAtLevel(5242, 21);
    CHECK(cost && cost->Cost == 200);
    cost = data.CostAtLevel(5242, 60);
    CHECK(cost && cost->Cost == 300);
    cost = data.CostAtLevel(5242, 1);
    CHECK(cost && cost->Cost == 100);
}

void UnpricedSpellsHaveNoCost()
{
    std::vector<std::string> warnings;
    auto const data = Parse(kFile, warnings);
    CHECK(!data.CostAtLevel(424242, 10));      // not in the file
    CHECK(!data.CostAtLevel(9000, 10));        // a percent cost
    CHECK(!data.CostAtLevel(9100, 10));        // a cost that depends on the level
    CHECK(data.CostAtLevel(9200, 10));         // a rank that points at itself: the walk stops, the cost is the spell's own
}

void MissingFileGivesAnEmptyTableAndAWarning()
{
    std::vector<std::string> warnings;
    auto const data = spell_cost::LoadSpellCosts("/nonexistent/spell_costs.txt", warnings);
    CHECK(data.BySpell.empty() && warnings.size() == 1);
}

} // namespace

int main()
{
    ParsesTheFile();
    SkipsBadLinesWithAWarning();
    TheFirstPowerLineOfASpellWins();
    CostIsThatOfTheHighestRankTheBotCanKnow();
    UnpricedSpellsHaveNoCost();
    MissingFileGivesAnEmptyTableAndAWarning();
    if (failures == 0)
        std::printf("spell_costs_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
