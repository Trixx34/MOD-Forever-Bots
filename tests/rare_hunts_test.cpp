// SPDX-License-Identifier: GPL-2.0-or-later
// The rare hunt table: parsing, the choice of a hunt for a bot and the fight filter entries (src/rare_hunts.h).
#include "rare_hunts.h"

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
    auto const hunts = rare_hunts::ParseHunts(
        "# entry|name|map|x|y|z|minLevel|maxLevel|radius\n"
        "7000|Night Howler|1|100|200|5|20|25|30\n"
        "7001|Bad levels|1|1|2|3|30|20|10\n"
        "7002|Short|1|1|2|3\n",
        errors);
    CHECK(hunts.size() == 1);
    CHECK(errors.size() == 2);
    CHECK(hunts.size() == 1 && hunts[0].Entry == 7000 && hunts[0].Name == "Night Howler" && hunts[0].MinLevel == 20);
}

void ChooseTheNearestSuitingHuntNotDone()
{
    std::vector<std::string> errors;
    auto const hunts = rare_hunts::ParseHunts(
        "7000|Near|0|0|0|0|20|25|30\n"
        "7001|Far|0|500|0|0|20|25|30\n"
        "7002|Too high|0|10|0|0|60|70|30\n"
        "7003|Other map|1|1|0|0|20|25|30\n",
        errors);
    CHECK(errors.empty());
    CHECK(rare_hunts::ChooseHunt(hunts, 0, 2.0f, 0.0f, 22, {}) == std::optional<std::size_t>(0));
    // The nearest one is done: the far one is chosen.
    CHECK(rare_hunts::ChooseHunt(hunts, 0, 2.0f, 0.0f, 22, { 7000 }) == std::optional<std::size_t>(1));
    // A bot near the level slack edge still qualifies; one below it does not.
    CHECK(rare_hunts::ChooseHunt(hunts, 0, 2.0f, 0.0f, 17, {}) == std::optional<std::size_t>(0));
    CHECK(!rare_hunts::ChooseHunt(hunts, 0, 2.0f, 0.0f, 16, { 7000, 7001 }).has_value());
}

void EntriesAreUniqueAndCoverEveryHunt()
{
    std::vector<std::string> errors;
    auto const hunts = rare_hunts::ParseHunts("7000|A|0|0|0|0|1|5|10\n7000|A again|1|0|0|0|1|5|10\n7001|B|0|0|0|0|1|5|10\n", errors);
    auto const entries = rare_hunts::HuntEntries(hunts);
    CHECK(entries.size() == 2);
    CHECK(entries.size() == 2 && entries[0] == 7000 && entries[1] == 7001);
}

} // namespace

int main()
{
    TheTableParsesAndABadLineIsDescribed();
    ChooseTheNearestSuitingHuntNotDone();
    EntriesAreUniqueAndCoverEveryHunt();
    if (failures == 0)
        std::printf("rare_hunts_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
