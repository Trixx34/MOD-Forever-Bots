// SPDX-License-Identifier: GPL-2.0-or-later
// Role profiles: the dungeon rest, drink and ready marks per play style (tank, healer, melee, ranged, caster) instead of one global set for all.
//
// The marks used to be constants in bot_policy_internal.h (kDungeonDrinkBelowPct, kDungeonDrinkUpToPct, kRouteMinManaPct, kPartyReadyPowerPct,
// kPartyReadyHealthPct) that every bot shared. A profile holds the same five values for one style. Defaults() returns exactly those constants, so the
// first version changes nothing; a style is tuned by changing its row in ProfileOf(), one style at a time, and measuring it on a Deadmines run.
//
// Header only and free of the core: it needs no state of the policy, only the class id and the role the bot plays.
#ifndef FOREVER_BOTS_ROLE_PROFILE_H
#define FOREVER_BOTS_ROLE_PROFILE_H

#include <cstdint>

namespace role_profile
{
enum class Style : std::uint8_t { Tank, Healer, MeleeDps, RangedDps, CasterDps };

// The role a bot plays, as the policy knows it (BotState::Role without the policy header): Auto = what the class gives.
enum class PlayedRole : std::uint8_t { Auto, Tank, Healer, Dps };

struct Profile
{
    std::uint32_t DrinkBelowPct = 50;    // a held mana user starts to drink below this mana
    std::uint32_t DrinkUpToPct = 95;     // ... and drinks up to this
    std::uint32_t RouteMinManaPct = 80;  // the leader walks on only when every mana user of the party has at least this much
    std::uint32_t ReadyPowerPct = 95;    // the pull hold waits until every mana user has this much (Rest tactic)
    std::uint32_t ReadyHealthPct = 90;   // ... and every member this much health
};

// The marks every style shared before profiles existed.
constexpr Profile Defaults() { return Profile{}; }

// Class ids: 1 warrior, 2 paladin, 3 hunter, 4 rogue, 5 priest, 7 shaman, 8 mage, 9 warlock, 11 druid. A set role wins over the class; with Auto a warrior is a
// tank (the policy tanks with a warrior in a party), a healer class heals (priest, paladin, shaman, druid), a hunter is ranged, a mage or warlock casts, a
// rogue fights in melee. A class the table does not know counts as melee damage (as the role code does).
constexpr Style StyleOf(std::uint32_t classId, PlayedRole role)
{
    if (role == PlayedRole::Tank)
        return Style::Tank;
    if (role == PlayedRole::Healer)
        return Style::Healer;
    if (role == PlayedRole::Dps)
    {
        switch (classId)
        {
            case 3: return Style::RangedDps;
            case 5: case 7: case 8: case 9: case 11: return Style::CasterDps;
            default: return Style::MeleeDps;
        }
    }
    switch (classId)
    {
        case 1: return Style::Tank;
        case 2: case 5: case 7: case 11: return Style::Healer;
        case 3: return Style::RangedDps;
        case 8: case 9: return Style::CasterDps;
        default: return Style::MeleeDps;
    }
}

// The marks of a style. All rows equal Defaults() in this version.
constexpr Profile ProfileOf(Style style)
{
    switch (style)
    {
        case Style::Tank:
        case Style::Healer:
        case Style::MeleeDps:
        case Style::RangedDps:
        case Style::CasterDps:
            break;
    }
    return Defaults();
}

constexpr Profile ProfileFor(std::uint32_t classId, PlayedRole role) { return ProfileOf(StyleOf(classId, role)); }
} // namespace role_profile

#endif // FOREVER_BOTS_ROLE_PROFILE_H
