// SPDX-License-Identifier: GPL-2.0-or-later
// The riding rank table: parsing and the next rank due (src/riding_training.h).
#include "riding_training.h"

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

void TheTableParsesAndABadLineIsDescribed()
{
    std::vector<std::string> errors;
    auto const ranks = riding::ParseRanks(
        "# spell|minLevel|skill|copper|name\n"
        "33391|20|75|1000|Apprentice Riding\n"
        "33392|40|150|5000|Journeyman Riding\n"
        "bad|line\n"
        "33393|60|x|1|Broken\n",
        errors);
    CHECK(ranks.size() == 2);
    CHECK(errors.size() == 2);
    CHECK(ranks.size() == 2 && ranks[0].Spell == 33391 && ranks[0].MinLevel == 20 && ranks[0].Skill == 75 && ranks[0].CostCopper == 1000);
    CHECK(ranks.size() == 2 && ranks[1].Name == "Journeyman Riding");
}

void NextRankNeedsLevelSkillAndCopper()
{
    std::vector<std::string> errors;
    auto const ranks = riding::ParseRanks("33391|20|75|1000|Apprentice\n33392|40|150|5000|Journeyman\n", errors);
    CHECK(!riding::NextRank(ranks, 19, 0, 10000).has_value());          // too low a level
    CHECK(!riding::NextRank(ranks, 25, 0, 999).has_value());            // not enough copper
    CHECK(riding::NextRank(ranks, 25, 0, 1000) == std::optional<std::size_t>(0));
    CHECK(!riding::NextRank(ranks, 25, 75, 1000).has_value());          // already has the riding skill it grants
    CHECK(riding::NextRank(ranks, 45, 75, 5000) == std::optional<std::size_t>(1));
    CHECK(!riding::NextRank(ranks, 60, 150, 10000).has_value());        // both ranks done
}

} // namespace

int main()
{
    TheTableParsesAndABadLineIsDescribed();
    NextRankNeedsLevelSkillAndCopper();
    if (failures == 0)
        std::printf("riding_training_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
