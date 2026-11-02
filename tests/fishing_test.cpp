// SPDX-License-Identifier: GPL-2.0-or-later
// The fishing spot table and the fishing-due rule (src/fishing.h).
#include "fishing.h"

#include <cstdio>
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
    auto const spots = fishing::ParseSpots(
        "# map|x|y|z|radius|name\n"
        "0|-3900|-560|4.5|5|Lake Everstill\n"
        "1|10|20|30|8|Pool\n"
        "0|1|2|5|Too few fields\n"
        "0|1|2|3|0|No radius\n",
        errors);
    CHECK(spots.size() == 2);
    CHECK(errors.size() == 2);
    CHECK(spots.size() == 2 && spots[0].Map == 0 && spots[0].Radius == 5.0f && spots[0].Name == "Lake Everstill");
}

void NearestIsOnTheMap()
{
    std::vector<std::string> errors;
    auto const spots = fishing::ParseSpots("0|0|0|0|5|Near\n0|100|0|0|5|Far\n1|1|0|0|5|Other map\n", errors);
    auto const nearest = fishing::NearestSpot(spots, 0, 2.0f, 0.0f);
    CHECK(nearest == std::optional<std::size_t>(0));
    CHECK(!fishing::NearestSpot(spots, 530, 0.0f, 0.0f).has_value());
}

void FishingIsDueWithAPoleAndSkillBelowTheCap()
{
    CHECK(fishing::FishingDue(10, 20, true));       // below 10 * 5
    CHECK(!fishing::FishingDue(10, 50, true));      // at the cap
    CHECK(!fishing::FishingDue(10, 20, false));     // no pole
    CHECK(!fishing::FishingDue(4, 0, true));        // below level 5
}

} // namespace

int main()
{
    TheTableParsesAndABadLineIsDescribed();
    NearestIsOnTheMap();
    FishingIsDueWithAPoleAndSkillBelowTheCap();
    if (failures == 0)
        std::printf("fishing_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
