// SPDX-License-Identifier: GPL-2.0-or-later
// Profession progression (src/professions.h): the skill cap, the skill-up lines, the recipe table and plan, the empty-node memory and the safe node choice.
#include "gather_nodes.h"
#include "professions.h"

#include <cstdio>
#include <fstream>
#include <sstream>
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

void TheCapIsFiveTimesTheLevelAndNeverAboveTheMaximum()
{
    CHECK(professions::SkillCap(16, 5, 150) == 80);
    CHECK(professions::SkillCap(16, 5, 75) == 75);    // the rank's maximum holds
    CHECK(professions::SkillCap(16, 5, 0) == 80);     // maximum unknown: the level cap only
    CHECK(professions::SkillCap(1, 5, 75) == 5);
    CHECK(professions::SkillCap(60, 0, 300) == 300);  // no cap of its own
    CHECK(professions::BelowCap(79, 16, 5, 150));
    CHECK(!professions::BelowCap(80, 16, 5, 150));
    CHECK(!professions::BelowCap(75, 16, 5, 75));
    CHECK(professions::BelowCap(500, 0, 5, 75));      // level unknown: never capped
}

void SkillUpsAreRisesOfTheSameLine()
{
    std::uint16_t const prevSkill[2] = { 186, 197 };
    std::uint16_t const prevValue[2] = { 1, 30 };
    std::uint16_t const skill[2] = { 186, 197 };
    std::uint16_t const value[2] = { 2, 30 };
    auto const ups = professions::SkillUps(prevSkill, prevValue, skill, value);
    CHECK(ups.size() == 1);
    if (ups.size() == 1)
    {
        CHECK(ups[0].Skill == 186 && ups[0].From == 1 && ups[0].To == 2);
        CHECK(professions::SkillUpLine(42, ups[0]) == "profession_skillup: bot=42 skill=186 from=1 to=2");
    }
    // first sight (previous 0), a changed line, a drop: no line
    std::uint16_t const firstValue[2] = { 0, 0 };
    CHECK(professions::SkillUps(prevSkill, firstValue, skill, value).empty());
    std::uint16_t const otherSkill[2] = { 182, 165 };
    std::uint16_t const higher[2] = { 5, 40 };
    CHECK(professions::SkillUps(prevSkill, prevValue, otherSkill, higher).empty());
    std::uint16_t const lower[2] = { 1, 20 };
    CHECK(professions::SkillUps(prevSkill, prevValue, skill, lower).empty());
    // both slots rise
    std::uint16_t const both[2] = { 3, 31 };
    CHECK(professions::SkillUps(prevSkill, prevValue, skill, both).size() == 2);
}

void TheRecipeTableParsesAndSkipsBadLines()
{
    std::vector<std::string> errors;
    auto const recipes = professions::ParseRecipeTable(
        "# header\n"
        "197 2963 1 25 50 2589:2  # Bolt of Linen Cloth\n"
        "\n"
        "171 2330 1 15 60 2447:1,765:1,3371:1\r\n"
        "197 2385 1 20 x 2589:1\n"          // bad number
        "197 2386 1 20 60\n"                // no reagents
        "197 2387 1 20 60 2589:0\n",        // a count of 0
        errors);
    CHECK(recipes.size() == 2);
    CHECK(errors.size() == 3);
    if (recipes.size() == 2)
    {
        CHECK(recipes[0].Skill == 197 && recipes[0].Spell == 2963 && recipes[0].TrivialLow == 25 && recipes[0].TrivialHigh == 50);
        CHECK(recipes[0].Reagents.size() == 1 && recipes[0].Reagents[0].first == 2589 && recipes[0].Reagents[0].second == 2);
        CHECK(recipes[1].Reagents.size() == 3 && recipes[1].Reagents[2].first == 3371);
    }
}

void ThePlanNamesTheEasiestRecipeThatStillRaisesTheSkill()
{
    std::vector<std::string> errors;
    auto const recipes = professions::ParseRecipeTable(
        "197 100 1 40 75 2589:1\n"
        "197 101 1 25 50 2589:2\n"
        "197 102 50 70 95 2592:1\n"     // needs 50
        "164 200 1 15 55 2835:1\n",
        errors);
    CHECK(errors.empty());
    auto pick = professions::PlanRecipe(recipes, 197, 1);
    CHECK(pick && recipes[*pick].Spell == 101);
    // grey at 50: the next easiest
    pick = professions::PlanRecipe(recipes, 197, 50);
    CHECK(pick && recipes[*pick].Spell == 100);
    // a recipe the bot does not know is skipped
    pick = professions::PlanRecipe(recipes, 197, 1, [](std::uint32_t spell) { return spell == 101; });
    CHECK(pick && recipes[*pick].Spell == 100);
    // 75 and up: only the one that needs 50
    pick = professions::PlanRecipe(recipes, 197, 80);
    CHECK(pick && recipes[*pick].Spell == 102);
    CHECK(!professions::PlanRecipe(recipes, 197, 95).has_value());
    CHECK(!professions::PlanRecipe(recipes, 165, 1).has_value());
}

void EmptyNodesAreSkippedForTheirTimeAndBounded()
{
    professions::EmptyNodes empty;
    empty.Mark(7, 1000);
    CHECK(empty.Empty(7, 999));
    CHECK(!empty.Empty(7, 1000));
    CHECK(!empty.Empty(8, 0));
    for (std::size_t i = 0; i < professions::EmptyNodes::kMaxKept + 10; ++i)
        empty.Mark(100 + i, 5000 + i);
    CHECK(empty.Size() == professions::EmptyNodes::kMaxKept);
    CHECK(empty.Empty(100 + professions::EmptyNodes::kMaxKept + 9, 0));   // the newest is kept
}

void TheSafeNodeIsNearAndNotRejected()
{
    std::vector<std::string> errors;
    auto const nodes = gather::ParseNodeTable(
        "186 0 10 0 0 1 1 5 0\n"
        "186 0 40 0 0 2 1 5 0\n"
        "186 0 400 0 0 3 1 5 0\n"
        "182 0 5 0 0 4 1 5 0\n"
        "186 0 20 0 0 5 1 30 0\n",      // an area of level 30
        errors);
    CHECK(errors.empty());
    gather::NodeFilter filter;
    filter.SkillValue = 1;
    filter.Level = 10;
    std::uint32_t skipped = 0;
    auto pick = professions::NearestSafeNode(nodes, 186, 0, 0.0f, 0.0f, filter, 150.0f, {}, skipped);
    CHECK(pick && nodes[*pick].Entry == 1 && skipped == 0);
    // the nearest is rejected (empty, too high, dangerous): the next one in range
    pick = professions::NearestSafeNode(nodes, 186, 0, 0.0f, 0.0f, filter, 150.0f, [](std::size_t i) { return i == 0; }, skipped);
    CHECK(pick && nodes[*pick].Entry == 2 && skipped == 1);
    // everything in range rejected: none, the far node is out of range
    pick = professions::NearestSafeNode(nodes, 186, 0, 0.0f, 0.0f, filter, 150.0f, [](std::size_t i) { return i < 2; }, skipped);
    CHECK(!pick.has_value() && skipped == 2);
    // a wider range reaches it
    pick = professions::NearestSafeNode(nodes, 186, 0, 0.0f, 0.0f, filter, 600.0f, [](std::size_t i) { return i < 2; }, skipped);
    CHECK(pick && nodes[*pick].Entry == 3);
    // the level 30 area is out for a level 10 bot (the row's area level), in for a level 30 one
    filter.Level = 30;
    pick = professions::NearestSafeNode(nodes, 186, 0, 19.0f, 0.0f, filter, 150.0f, {}, skipped);
    CHECK(pick && nodes[*pick].Entry == 5);
}

void TheShippedRecipeTableParses()
{
#ifdef FOREVER_PROFESSION_RECIPES_FILE
    std::ifstream file(FOREVER_PROFESSION_RECIPES_FILE);
    std::stringstream text;
    text << file.rdbuf();
    std::vector<std::string> errors;
    auto const recipes = professions::ParseRecipeTable(text.str(), errors);
    CHECK(file.good());
    CHECK(errors.empty());
    CHECK(recipes.size() > 1000);
    // Tailoring at 1 has a recipe to name, and Bolt of Linen Cloth (2963) is in the table.
    CHECK(professions::PlanRecipe(recipes, 197, 1).has_value());
    bool linen = false;
    for (auto const& recipe : recipes)
        linen = linen || (recipe.Skill == 197 && recipe.Spell == 2963);
    CHECK(linen);
#endif
}

} // namespace

int main()
{
    TheCapIsFiveTimesTheLevelAndNeverAboveTheMaximum();
    SkillUpsAreRisesOfTheSameLine();
    TheRecipeTableParsesAndSkipsBadLines();
    ThePlanNamesTheEasiestRecipeThatStillRaisesTheSkill();
    EmptyNodesAreSkippedForTheirTimeAndBounded();
    TheSafeNodeIsNearAndNotRejected();
    TheShippedRecipeTableParses();
    if (failures == 0)
        std::printf("professions_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
