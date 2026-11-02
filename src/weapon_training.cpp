// SPDX-License-Identifier: GPL-2.0-or-later
#include "weapon_training.h"

#include <algorithm>

namespace weapons {

namespace {

// Weapon skill lines a class may train (proficiency rules of the classic client). A skill is trainable from level 1 in this table; a
// class trainer teaches the skill only once the bot has the level, so the level gate is applied by the caller's level check.
constexpr std::uint32_t kSkillAxes = 44;
constexpr std::uint32_t kSkillBows = 45;
constexpr std::uint32_t kSkillGuns = 46;
constexpr std::uint32_t kSkillMaces = 54;
constexpr std::uint32_t kSkillTwoHandedSwords = 55;
constexpr std::uint32_t kSkillSwords = 43;
constexpr std::uint32_t kSkillStaves = 136;
constexpr std::uint32_t kSkillTwoHandedAxes = 172;
constexpr std::uint32_t kSkillDaggers = 173;
constexpr std::uint32_t kSkillThrown = 176;
constexpr std::uint32_t kSkillWands = 228;
constexpr std::uint32_t kSkillPolearms = 229;
constexpr std::uint32_t kSkillFistWeapons = 473;

} // namespace

std::vector<ClassWeapons> const& ClassWeaponTable()
{
    static std::vector<ClassWeapons> const table = {
        { 1, { kSkillSwords, kSkillAxes, kSkillMaces, kSkillTwoHandedSwords, kSkillTwoHandedAxes, kSkillPolearms, kSkillDaggers, kSkillFistWeapons, kSkillThrown } },
        { 2, { kSkillSwords, kSkillAxes, kSkillMaces, kSkillTwoHandedSwords, kSkillTwoHandedAxes, kSkillPolearms } },
        { 3, { kSkillBows, kSkillGuns, kSkillSwords, kSkillAxes, kSkillPolearms, kSkillStaves, kSkillDaggers, kSkillFistWeapons, kSkillTwoHandedSwords, kSkillTwoHandedAxes } },
        { 4, { kSkillDaggers, kSkillSwords, kSkillMaces, kSkillFistWeapons, kSkillThrown, kSkillAxes } },
        { 5, { kSkillMaces, kSkillStaves, kSkillWands, kSkillDaggers } },
        { 7, { kSkillMaces, kSkillAxes, kSkillStaves, kSkillFistWeapons, kSkillDaggers } },
        { 8, { kSkillDaggers, kSkillStaves, kSkillSwords, kSkillWands } },
        { 9, { kSkillDaggers, kSkillStaves, kSkillSwords, kSkillWands } },
        { 11, { kSkillMaces, kSkillStaves, kSkillDaggers, kSkillFistWeapons, kSkillPolearms } },
    };
    return table;
}

std::vector<std::uint32_t> WeaponSkillsDue(std::uint32_t classId, std::uint32_t level, std::vector<std::uint32_t> const& learned)
{
    std::vector<std::uint32_t> due;
    if (level == 0)
        return due;
    for (ClassWeapons const& entry : ClassWeaponTable())
    {
        if (entry.ClassId != classId)
            continue;
        for (std::uint32_t skill : entry.Skills)
            if (std::find(learned.begin(), learned.end(), skill) == learned.end())
                due.push_back(skill);
        break;
    }
    return due;
}

} // namespace weapons
