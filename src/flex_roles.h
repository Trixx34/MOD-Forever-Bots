// SPDX-License-Identifier: GPL-2.0-or-later
// Flexible tank and healer roles (config FlexRoles, default off). Pure logic, no core; unit-tested in tests/flex_roles_test.cpp, driven by BotPolicy::BuildParties.
//
// Without it only a warrior tanks in Role::Auto and a party with no warrior has no tank. With it, when a party is formed the roles are picked from the classes in it:
//   tank candidates, in this order: warrior (stays tank, as before), paladin (protection: Righteous Fury, Holy Shield, Consecration, Judgement), druid (Bear Form, Growl,
//     Maul, Swipe, Demoralizing Roar, Enrage). A paladin or druid needs level kMinFlexTankLevel (Righteous Fury and Bear Form are learnt at 10); level 0 (unknown) passes.
//   healer candidates, in this order: priest, shaman, druid (restoration), paladin (holy).
// An explicit role (a chat `role` order, a roster spec) is never changed: it fills its slot. Only an Auto bot is picked. When there is no warrior and no explicit tank,
// the (tank, healer) pair is chosen so that BOTH roles are filled when the classes allow it, then by tank preference, then by healer preference, then by lowest guid, so the
// same party always gets the same answer. When only one of the two can be filled by the same bot, the tank wins (a party with no tank has no one to lead or pull): reason
// "tank_over_healer". The picked healer is told apart from the bots that stay Auto, which still heal on their own as before.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace flex_roles {

constexpr std::uint32_t kWarrior = 1, kPaladin = 2, kPriest = 5, kShaman = 7, kDruid = 11;
constexpr std::uint32_t kMinFlexTankLevel = 10;

// What a member already has: 0 nothing (Auto), 1 tank, 2 healer, 3 dps.
struct Member
{
    std::uint64_t Guid = 0;
    std::uint32_t ClassId = 0;
    std::uint32_t Level = 0;       // 0 = unknown
    std::uint32_t Explicit = 0;
};

struct Result
{
    std::map<std::uint64_t, std::uint32_t> Role;   // guid -> 1 tank or 2 healer, for the bots picked here (a warrior left on Auto is not listed)
    bool HasTank = false;                          // the party has a tank (a warrior, an explicit tank or a pick)
    bool HasHealer = false;
    std::string Reason;                            // one line for the log: who was picked and why, or what is missing
};

// 0 when the class cannot tank, else its rank (1 best): warrior 1, paladin 2, druid 3.
std::uint32_t TankRank(std::uint32_t classId, std::uint32_t level);
// 0 when the class cannot heal, else its rank: priest 1, shaman 2, druid 3, paladin 4.
std::uint32_t HealerRank(std::uint32_t classId);

Result Assign(std::vector<Member> const& party);

// How many mobs a tank of this class pulls together in a dungeon (kMaxDungeonPull is the warrior's): paladin and druid pull fewer.
std::size_t DungeonPullCap(std::uint32_t tankClassId, std::size_t warriorCap);

} // namespace flex_roles
