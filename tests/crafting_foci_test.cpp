// SPDX-License-Identifier: GPL-2.0-or-later
// The crafting focus table: parsing, the focus of a skill, the nearest object and the reach (src/crafting_foci.h).
#include "crafting_foci.h"

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

void SkillsMapToTheirFocus()
{
    CHECK(crafting::FocusOfSkill(164) == crafting::kFocusAnvil);   // Blacksmithing
    CHECK(crafting::FocusOfSkill(202) == crafting::kFocusAnvil);   // Engineering
    CHECK(crafting::FocusOfSkill(186) == crafting::kFocusForge);   // Mining: smelting
    CHECK(crafting::FocusOfSkill(185) == crafting::kFocusCookingFire);
    CHECK(crafting::FocusOfSkill(197) == 0);                       // Tailoring: nothing needed
    CHECK(crafting::FocusOfSkill(333) == 0);                       // Enchanting
    CHECK(crafting::FocusOfSkill(171) == 0);                       // Alchemy
    CHECK(crafting::FocusOfSkill(129) == 0);                       // First Aid
}

void TheTableParsesAndABadLineIsDescribed()
{
    std::vector<std::string> errors;
    auto const points = crafting::ParseFociTable(
        "# comment\n"
        "\n"
        "Ironforge|0|1|-4789.2 -1128.7 498.9|10|171713|Anvil\n"
        "Ironforge|0|3|-4800 -1100 498|8|1|Forge\r\n"
        "Ironforge|0|x|1 2 3|10|5|Broken focus\n"
        "Ironforge|0|1|1 2|10|5|Two numbers\n"
        "Ironforge|0|1|1 2 3 4|10|5|Four numbers\n"
        "short|line\n",
        errors);
    CHECK(points.size() == 2);
    CHECK(errors.size() == 4);
    CHECK(!errors.empty() && errors[0].rfind("line 5:", 0) == 0);
    CHECK(points[0].City == "Ironforge" && points[0].Map == 0 && points[0].Focus == 1 && points[0].Radius == 10 && points[0].Entry == 171713);
    CHECK(points[0].X < -4789.0f && points[0].X > -4789.5f && points[0].Name == "Anvil");
    CHECK(points[1].Name == "Forge");   // the carriage return of a Windows line is dropped
}

void NearestIsOfTheFocusAndTheMap()
{
    std::vector<std::string> errors;
    auto const points = crafting::ParseFociTable(
        "A|0|1|100 0 0|10|1|Far anvil\n"
        "A|0|1|20 0 0|10|2|Near anvil\n"
        "A|0|3|5 0 0|10|3|Forge nearer still\n"
        "B|1|1|1 0 0|10|4|Anvil on another map\n",
        errors);
    CHECK(errors.empty());
    auto const anvil = crafting::NearestFocus(points, crafting::kFocusAnvil, 0, 0.0f, 0.0f);
    CHECK(anvil && *anvil == 1);
    auto const forge = crafting::NearestFocus(points, crafting::kFocusForge, 0, 0.0f, 0.0f);
    CHECK(forge && *forge == 2);
    auto const other = crafting::NearestFocus(points, crafting::kFocusAnvil, 1, 50.0f, 50.0f);
    CHECK(other && *other == 3);
    CHECK(!crafting::NearestFocus(points, crafting::kFocusCookingFire, 0, 0.0f, 0.0f));
    CHECK(!crafting::NearestFocus(points, crafting::kFocusAnvil, 2, 0.0f, 0.0f));
}

void ReachIsTheRadiusOnTheSameMap()
{
    std::vector<std::string> errors;
    auto const points = crafting::ParseFociTable("A|0|1|0 0 0|10|1|Anvil\n", errors);
    CHECK(points.size() == 1);
    CHECK(crafting::InReach(points[0], 0, 6.0f, 8.0f));     // exactly 10 yards
    CHECK(!crafting::InReach(points[0], 0, 6.0f, 8.5f));
    CHECK(!crafting::InReach(points[0], 1, 0.0f, 0.0f));    // another map
}

void TheShippedTableIsSound()
{
    std::ifstream in(FOREVER_CRAFTING_FOCI_FILE);
    CHECK(in.good());
    std::stringstream text;
    text << in.rdbuf();
    std::vector<std::string> errors;
    auto const points = crafting::ParseFociTable(text.str(), errors);
    CHECK(errors.empty());
    CHECK(points.size() > 50);
    std::size_t anvils = 0, forges = 0, fires = 0;
    for (auto const& point : points)
    {
        anvils += point.Focus == crafting::kFocusAnvil;
        forges += point.Focus == crafting::kFocusForge;
        fires += point.Focus == crafting::kFocusCookingFire;
        CHECK(point.Radius >= 1 && point.Radius <= 60);
        CHECK(point.Map == 0 || point.Map == 1);
    }
    CHECK(anvils > 0 && forges > 0 && fires > 0);
    // From Stormwind's trade district the nearest anvil is in Stormwind (the stone anvils of the Dwarven District are further).
    auto const anvil = crafting::NearestFocus(points, crafting::kFocusAnvil, 0, -8816.0f, 621.0f);
    CHECK(anvil && points[*anvil].City == "Stormwind");
}

} // namespace

int main()
{
    SkillsMapToTheirFocus();
    TheTableParsesAndABadLineIsDescribed();
    NearestIsOfTheFocusAndTheMap();
    ReachIsTheRadiusOnTheSameMap();
    TheShippedTableIsSound();
    if (failures == 0)
        std::printf("crafting_foci_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
