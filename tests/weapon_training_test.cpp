// SPDX-License-Identifier: GPL-2.0-or-later
// The weapon skills due per class (src/weapon_training.h).
#include "weapon_training.h"

#include <cstdint>
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

void EveryClassInTheTableHasSkills()
{
    auto const& table = weapons::ClassWeaponTable();
    CHECK(table.size() == 9);
    for (auto const& entry : table)
        CHECK(!entry.Skills.empty());
}

void DueSkillsAreTheMissingOnes()
{
    // A warrior with no weapon skill yet is due all of its skills, in table order.
    auto const none = weapons::WeaponSkillsDue(1, 10, {});
    CHECK(!none.empty());
    CHECK(!none.empty() && none.front() == 43);     // Swords first in the warrior list
    // Knowing a skill removes it from the due list, and nothing else changes.
    auto const oneKnown = weapons::WeaponSkillsDue(1, 10, { 43 });
    CHECK(oneKnown.size() + 1 == none.size());
    bool stillHasSwords = false;
    for (std::uint32_t skill : oneKnown)
        stillHasSwords = stillHasSwords || skill == 43;
    CHECK(!stillHasSwords);
}

void NothingDueAtLevelZeroOrForAnUnknownClass()
{
    CHECK(weapons::WeaponSkillsDue(1, 0, {}).empty());
    CHECK(weapons::WeaponSkillsDue(99, 10, {}).empty());
}

} // namespace

int main()
{
    EveryClassInTheTableHasSkills();
    DueSkillsAreTheMissingOnes();
    NothingDueAtLevelZeroOrForAnUnknownClass();
    if (failures == 0)
        std::printf("weapon_training_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
