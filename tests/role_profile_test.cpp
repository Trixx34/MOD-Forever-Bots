// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the role profiles (src/role_profile.h): the style of a class and role, and that every profile still equals the old global marks.
#include "role_profile.h"

#include <cstdio>
#include <initializer_list>

using namespace role_profile;

namespace {
int failures = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                                \
        }                                                                              \
    } while (0)
} // namespace

int main()
{
    // Auto: what the class gives
    CHECK(StyleOf(1, PlayedRole::Auto) == Style::Tank);
    CHECK(StyleOf(5, PlayedRole::Auto) == Style::Healer);
    CHECK(StyleOf(2, PlayedRole::Auto) == Style::Healer);
    CHECK(StyleOf(3, PlayedRole::Auto) == Style::RangedDps);
    CHECK(StyleOf(8, PlayedRole::Auto) == Style::CasterDps);
    CHECK(StyleOf(9, PlayedRole::Auto) == Style::CasterDps);
    CHECK(StyleOf(4, PlayedRole::Auto) == Style::MeleeDps);
    CHECK(StyleOf(0, PlayedRole::Auto) == Style::MeleeDps); // unknown class
    // A set role wins over the class
    CHECK(StyleOf(5, PlayedRole::Dps) == Style::CasterDps);   // shadow priest
    CHECK(StyleOf(2, PlayedRole::Dps) == Style::MeleeDps);    // retribution paladin
    CHECK(StyleOf(1, PlayedRole::Dps) == Style::MeleeDps);    // arms or fury warrior
    CHECK(StyleOf(2, PlayedRole::Tank) == Style::Tank);       // protection paladin
    CHECK(StyleOf(11, PlayedRole::Tank) == Style::Tank);      // bear druid
    CHECK(StyleOf(8, PlayedRole::Healer) == Style::Healer);
    // Every profile equals the old constants (kDungeonDrinkBelowPct 50, kDungeonDrinkUpToPct 95, kRouteMinManaPct 70, kPartyReadyPowerPct 85, kPartyReadyHealthPct 90)
    for (Style s : { Style::Tank, Style::Healer, Style::MeleeDps, Style::RangedDps, Style::CasterDps })
    {
        Profile const p = ProfileOf(s);
        CHECK(p.DrinkBelowPct == 50 && p.DrinkUpToPct == 95 && p.RouteMinManaPct == 70 && p.ReadyPowerPct == 85 && p.ReadyHealthPct == 90);
    }
    if (failures == 0)
        std::puts("role_profile_test: all passed");
    return failures == 0 ? 0 : 1;
}
