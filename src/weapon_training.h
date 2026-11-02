// SPDX-License-Identifier: GPL-2.0-or-later
// Weapon skill training (WeaponTraining = 1, off by default): a class learns its weapon skills from its class trainers. The table
// below is the class-to-weapon-skill list (the skill lines a class may train); the due check is pure: no core, no clock, no file access.
// The trainer visit itself reuses the town-errand walk (errand_trainers, kTrain). tests/weapon_training_test.cpp covers the rules.
#pragma once

#include <cstdint>
#include <vector>

namespace weapons {

// The skill lines of each class's weapon proficiencies (Skill Line ids as in BotProfessions).
struct ClassWeapons
{
    std::uint32_t ClassId = 0;
    std::vector<std::uint32_t> Skills;
};

// Class ids as the module uses them: 1 Warrior, 2 Paladin, 3 Hunter, 4 Rogue, 5 Priest, 7 Shaman, 8 Mage, 9 Warlock, 11 Druid.
std::vector<ClassWeapons> const& ClassWeaponTable();

// The weapon skills the bot may learn now: the skills of its class at or below its level that it does not have yet. A skill is
// "learned" when the bot's skill list (skill line ids it has) contains it. Returns the skills in table order.
std::vector<std::uint32_t> WeaponSkillsDue(std::uint32_t classId, std::uint32_t level, std::vector<std::uint32_t> const& learned);

} // namespace weapons
