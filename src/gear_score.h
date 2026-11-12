// SPDX-License-Identifier: GPL-2.0-or-later
// Gear score: how good an item is for a class and role, and the two decisions built on it: whether a worn item keeps its slot against a candidate, and
// whether a piece of gear in the bags is outclassed by what is worn and may be sold. Pure logic, unit-tested in tests/gear_score_test.cpp.
//
// The scale is the core's (ModuleBotSupplyRules.h): item level x 10 plus the stat points, a primary stat the role wants counting 3 per point, a primary stat
// it does not use counting 3 per point against. The weights here are per role (archetype) instead of one wanted/unwanted split, so a tank's stamina and
// defence, a healer's spirit and spell power and a hunter's attack power count. Items are never judged by id: only by their numbers, so items the
// Forever world adds later with higher ids (or higher item levels) are scored like any other.
//
// NOT WIRED YET. The module is not given the stats of a worn or carried item (the snapshot has none and kSellToVendor sells whole entries), and the core
// already scores worn-vs-candidate itself (BotGearRules::KeepWorn). This header is the module-side rule, ready for the day the core sends item stats
// per bag and worn item; the requirement is in forever-ops docs/99-todo.md.
#pragma once

#include <array>
#include <cstdint>

namespace gear_score {

enum Stat : std::uint8_t
{
    kStrength, kAgility, kStamina, kIntellect, kSpirit,
    kAttackPower, kSpellPower, kCrit, kHit, kHaste,
    kDefense, kDodge, kParry, kBlock, kStatCount
};

enum class Archetype : std::uint8_t { Tank, MeleeStrength, MeleeAgility, Hunter, Caster, Healer };

// The archetype of a class (ids as in the core: 1 warrior, 2 paladin, 3 hunter, 4 rogue, 5 priest, 7 shaman, 8 mage, 9 warlock, 11 druid) playing a role:
// role 0 = what the class gives (a warrior tanks, a priest, paladin, shaman and druid heal), 1 tank, 2 healer, 3 damage.
Archetype ArchetypeOf(std::uint32_t classId, std::uint32_t role);

struct Weights
{
    std::array<std::int32_t, kStatCount> Per{};
};
Weights WeightsOf(Archetype archetype);

struct Item
{
    std::uint32_t Level = 0;                         // item level
    std::uint8_t Quality = 0;                        // 0 grey, 1 white, 2 green, 3 blue, 4 epic, 5 legendary
    std::uint32_t RequiredLevel = 0;
    std::uint32_t InventoryType = 0;                 // the game's slot type; 0 = not equipment
    std::uint32_t ArmorSubclass = 0;                 // 1 cloth, 2 leather, 3 mail, 4 plate; 0 = not body armour
    bool QuestItem = false;
    bool SetItem = false;                            // part of an item set
    bool Usable = true;                              // the bot's class and skills can use it
    std::array<std::int32_t, kStatCount> Stats{};    // amount per stat
};

// Item level x 10 plus the weighted stats.
std::int32_t Score(Weights const& weights, Item const& item);

// A worn item keeps its slot unless the candidate scores strictly higher (a tie keeps it, so two passes cannot undo each other).
bool KeepWorn(Weights const& weights, Item const& worn, Item const& candidate);

enum class SellVerdict : std::uint8_t
{
    Sell,
    KeepNotEquipment, KeepQuestItem, KeepSetItem, KeepQuality, KeepNotUsable,
    KeepLevelLater,        // it needs a higher level than the bot has: it may become an upgrade
    KeepNothingWorn,       // the slot is empty (or the worn list does not cover every slot of that type)
    KeepOtherSlotType,     // what is worn there is another type (a 2-hand weapon against a 1-hand, say): not comparable
    KeepHeavierArmor,      // heavier body armour than what is worn: the core may still swap to it
    KeepCouldBeBetter,     // not outclassed by every worn item by the margin
};
char const* SellVerdictName(SellVerdict verdict);

// Bag gear is outclassed (and a vendor may take it) only when every slot it could fill holds a worn item of the same type that scores more by
// kSellMarginScore with an item level at least as high. `worn` lists the items in those slots (one, or two for rings, trinkets and one-hand weapons);
// `slots` is how many slots the type has. Green and lower only: blue and better are never sold here.
constexpr std::int32_t kSellMarginScore = 30;
constexpr std::uint8_t kMaxSellQuality = 2;
SellVerdict JudgeBagGear(Weights const& weights, std::uint32_t botLevel, Item const& bag, Item const* worn, std::size_t wornCount, std::size_t slots);

}  // namespace gear_score
