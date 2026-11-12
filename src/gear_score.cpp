// SPDX-License-Identifier: GPL-2.0-or-later
#include "gear_score.h"

namespace gear_score {

namespace {
constexpr std::int32_t kPrimary = 3;   // the core's primary stat weight
constexpr std::int32_t kItemLevelWeight = 10;
}  // namespace

Archetype ArchetypeOf(std::uint32_t classId, std::uint32_t role)
{
    if (role == 1)
        return Archetype::Tank;
    if (role == 2)
        return Archetype::Healer;
    if (role == 0)
    {
        switch (classId)
        {
            case 1: return Archetype::Tank;
            case 2: case 5: case 7: case 11: return Archetype::Healer;
            default: break;
        }
    }
    switch (classId)
    {
        case 1: case 2: return Archetype::MeleeStrength;
        case 3: return Archetype::Hunter;
        case 5: case 7: case 8: case 9: case 11: return Archetype::Caster;
        default: return Archetype::MeleeAgility;   // rogue and any class the table does not know
    }
}

Weights WeightsOf(Archetype archetype)
{
    Weights w;
    auto& p = w.Per;
    switch (archetype)
    {
        case Archetype::Tank:
            p[kStamina] = 2; p[kStrength] = kPrimary; p[kAgility] = 1; p[kDefense] = 2; p[kDodge] = 2; p[kParry] = 2; p[kBlock] = 1;
            p[kIntellect] = -kPrimary; p[kSpirit] = -kPrimary; p[kSpellPower] = 0; p[kAttackPower] = 1;
            break;
        case Archetype::MeleeStrength:
            p[kStrength] = kPrimary; p[kAgility] = 1; p[kStamina] = 1; p[kAttackPower] = 1; p[kCrit] = 1; p[kHit] = 1; p[kHaste] = 1;
            p[kIntellect] = -kPrimary; p[kSpirit] = -kPrimary;
            break;
        case Archetype::MeleeAgility:
            p[kAgility] = kPrimary; p[kStrength] = 1; p[kStamina] = 1; p[kAttackPower] = 1; p[kCrit] = 1; p[kHit] = 1; p[kHaste] = 1;
            p[kIntellect] = -kPrimary; p[kSpirit] = -kPrimary;
            break;
        case Archetype::Hunter:
            p[kAgility] = kPrimary; p[kIntellect] = 1; p[kStamina] = 1; p[kAttackPower] = 1; p[kCrit] = 1; p[kHit] = 1; p[kHaste] = 1;
            p[kStrength] = -kPrimary; p[kSpirit] = -kPrimary;
            break;
        case Archetype::Caster:
            p[kIntellect] = kPrimary; p[kSpellPower] = 1; p[kStamina] = 1; p[kSpirit] = 1; p[kCrit] = 1; p[kHit] = 1; p[kHaste] = 1;
            p[kStrength] = -kPrimary; p[kAgility] = -kPrimary;
            break;
        case Archetype::Healer:
            p[kIntellect] = kPrimary; p[kSpirit] = kPrimary; p[kSpellPower] = 1; p[kStamina] = 1; p[kCrit] = 1; p[kHaste] = 1;
            p[kStrength] = -kPrimary; p[kAgility] = -kPrimary;
            break;
    }
    return w;
}

std::int32_t Score(Weights const& weights, Item const& item)
{
    std::int32_t score = static_cast<std::int32_t>(item.Level) * kItemLevelWeight;
    for (std::size_t i = 0; i < kStatCount; ++i)
        score += weights.Per[i] * item.Stats[i];
    return score;
}

bool KeepWorn(Weights const& weights, Item const& worn, Item const& candidate)
{
    return Score(weights, candidate) <= Score(weights, worn);
}

char const* SellVerdictName(SellVerdict verdict)
{
    switch (verdict)
    {
        case SellVerdict::Sell: return "sell";
        case SellVerdict::KeepNotEquipment: return "keep_not_equipment";
        case SellVerdict::KeepQuestItem: return "keep_quest_item";
        case SellVerdict::KeepSetItem: return "keep_set_item";
        case SellVerdict::KeepQuality: return "keep_quality";
        case SellVerdict::KeepNotUsable: return "keep_not_usable";
        case SellVerdict::KeepLevelLater: return "keep_level_later";
        case SellVerdict::KeepNothingWorn: return "keep_nothing_worn";
        case SellVerdict::KeepOtherSlotType: return "keep_other_slot_type";
        case SellVerdict::KeepHeavierArmor: return "keep_heavier_armor";
        case SellVerdict::KeepCouldBeBetter: return "keep_could_be_better";
    }
    return "keep";
}

SellVerdict JudgeBagGear(Weights const& weights, std::uint32_t botLevel, Item const& bag, Item const* worn, std::size_t wornCount, std::size_t slots)
{
    if (bag.InventoryType == 0)
        return SellVerdict::KeepNotEquipment;
    if (bag.QuestItem)
        return SellVerdict::KeepQuestItem;
    if (bag.SetItem)
        return SellVerdict::KeepSetItem;
    if (bag.Quality > kMaxSellQuality)
        return SellVerdict::KeepQuality;
    if (!bag.Usable)
        return SellVerdict::KeepNotUsable;   // not judged: the auction rules or the junk visit handle what the bot cannot use at all
    if (bag.RequiredLevel > botLevel)
        return SellVerdict::KeepLevelLater;
    if (slots == 0 || wornCount < slots)
        return SellVerdict::KeepNothingWorn;
    std::int32_t const bagScore = Score(weights, bag);
    for (std::size_t i = 0; i < wornCount; ++i)
    {
        Item const& w = worn[i];
        if (w.InventoryType != bag.InventoryType)
            return SellVerdict::KeepOtherSlotType;
        if (bag.ArmorSubclass > w.ArmorSubclass)
            return SellVerdict::KeepHeavierArmor;
        if (w.Level < bag.Level || Score(weights, w) < bagScore + kSellMarginScore)
            return SellVerdict::KeepCouldBeBetter;
    }
    return SellVerdict::Sell;
}

}  // namespace gear_score
