// SPDX-License-Identifier: GPL-2.0-or-later
// bot_policy_detail helpers: the spell, class and snapshot facts (declared in bot_policy_internal.h).
#include "bot_policy_internal.h"

namespace bot_policy_detail {

bool HasLine(BotSnapshot const& bot, std::uint32_t line)
{
    if (line == 129 || line == 185)
        return true;
    return (bot.profession_skill[0] == line && bot.profession_value[0] != 0) || (bot.profession_skill[1] == line && bot.profession_value[1] != 0);
}

std::uint32_t ComboPointsGained(std::uint32_t spell)
{
    if (spell == kMutilate)
        return 2;
    return spell == kSinisterStrike || spell == kSaberSlash || spell == kHemorrhage || spell == kClaw || spell == kRake || spell == kShred ? 1 : 0;
}

bool IsCasting(BotSnapshot const& bot)
{
    return bot.casting_spell != 0;
}

bool KnownCastTimeSpell(std::uint32_t spell)
{
    switch (spell)
    {
        case 686: case 585: case 133: case 116: case 5176: case 5185: case 403: case 331: case 2050: case 2054: case 2060: case 635:
        case 8092: case 348: case 707: case 172: case 1120: case 689: case 5676: case 5143: case 8004: case 421: case 1064: case 14914:
        case 15407: case 2948: case 5019: case 8921: case 19750: case 2006: case 7328: case 2008:
            return true;
        default:
            return false;
    }
}

std::uint32_t HealSpellFor(std::uint32_t classId, std::uint32_t level)
{
    switch (classId)
    {
        case 5: return level >= 16 ? 2054 : 2050; // priest: Heal, or Lesser Heal
        case 2: return 635;                       // paladin: Holy Light
        case 7: return 331;                       // shaman: Healing Wave
        case 11: return 5185;                     // druid: Healing Touch
        default: return 0;
    }
}

std::uint32_t ResurrectSpellFor(std::uint32_t classId, std::uint32_t level)
{
    switch (classId)
    {
        case 5: return level == 0 || level >= 10 ? kResurrection : 0;    // priest
        case 2: return level == 0 || level >= 12 ? kRedemption : 0;      // paladin
        case 7: return level == 0 || level >= 12 ? kAncestralSpirit : 0; // shaman
        default: return 0;
    }
}

bool IsResurrectSpell(std::uint32_t spell)
{
    return spell == kResurrection || spell == kRedemption || spell == kAncestralSpirit;
}

CreatureType CreatureTypeOf(std::uint32_t entry)
{
    switch (entry)
    {
        // Deadmines (36): the Defias Brotherhood and its goblins, all Humanoid.
        case 622:    // Goblin Engineer
        case 634:    // Defias Overseer
        case 639:    // Edwin VanCleef
        case 644:    // Rhahk'Zor
        case 645:    // Cookie
        case 646:    // Mr. Smite
        case 647:    // Captain Greenskin
        case 1729:   // Defias Evoker
        case 1731:   // Goblin Craftsman
        case 1732:   // Defias Squallshaper
        case 1763:   // Gilnid
        case 4418:   // Defias Wizard
        // Ragefire Chasm (389): orcs, trolls and goblins of the Searing Blade camp and the bosses, all Humanoid.
        case 11322:  // Searing Blade Cultist
        case 11323:  // Searing Blade Enforcer
        case 11324:  // Searing Blade Warlock
        case 11517:  // Oggleflint
        case 11518:  // Jergosh the Invoker
        case 11519:  // Bazzalan
        case 11520:  // Taragaman the Hungerer
            return CreatureType::kHumanoid;
        case 642:    // Sneed's Shredder: Mechanical (and a boss: never a cc target anyway, see the elite check below)
        case 2520:   // Remote-Controlled Golem: Mechanical
            return CreatureType::kMechanical;
        default:
            return CreatureType::kUnknown;
    }
}

std::uint32_t CcSpellForClass(std::uint32_t classId)
{
    switch (classId)
    {
        case 4: return kSapSpell;        // rogue
        case 8: return kPolymorphSpell;  // mage
        case 9: return kBanishSpell;     // warlock
        default: return 0;
    }
}

std::uint32_t CcLearnLevel(std::uint32_t spell)
{
    switch (spell)
    {
        case kSapSpell: return 10;
        case kPolymorphSpell: return 8;
        case kBanishSpell: return 28;
        default: return 0;
    }
}

bool CcAffects(std::uint32_t spell, CreatureType type)
{
    switch (spell)
    {
        case kSapSpell:
        case kPolymorphSpell: return type == CreatureType::kHumanoid;
        case kBanishSpell: return type == CreatureType::kDemon || type == CreatureType::kElemental;
        default: return false;
    }
}

bool AuraRunning(BotSnapshot const& bot, std::uint32_t spell)
{
    for (std::uint32_t i = 0; i < bot.aura_count && i < forever_module::kMaxAuras; ++i)
        if (bot.auras[i].spell_id == spell)
            return bot.auras[i].remaining_ms != 0;
    return false;
}

bool AuraRunning(BotSnapshot const& bot, std::uint32_t spell, std::vector<std::uint32_t> const& ranks)
{
    if (AuraRunning(bot, spell))
        return true;
    for (std::uint32_t rank : ranks)
        if (AuraRunning(bot, rank))
            return true;
    return false;
}

PetKnown PetOf(BotSnapshot const& bot)
{
    switch (bot.pet_state)
    {
        case 1: return PetKnown::kOut;
        case 2: return PetKnown::kDead;
        default: return PetKnown::kNone;
    }
}

std::uint64_t DotDurationMs(std::uint32_t spell)
{
    switch (spell)
    {
        case kRend: return kRendDurationMs;
        case 172: return 18000;  // Corruption
        case 707: return 15000;  // Immolate
        case 589: return 18000;  // Shadow Word: Pain
        case 1978: return 15000; // Serpent Sting
        case 8050: return 12000; // Flame Shock
        case 8921: return 12000; // Moonfire
        case 21084: return 30000; // Seal of Righteousness (self)
        case 17: return 30000;    // Power Word: Shield (self)
        // The cc tactic's holds (SpellDuration.db2 of this server's own client data, read 2026-10-27): the same record keeps the
        // policy from recasting a hold that is still running, same as a dot.
        case kSapSpell: return 25000;        // Sap
        case kPolymorphSpell: return 20000;  // Polymorph
        case kBanishSpell: return 20000;     // Banish
        default: return 0;
    }
}

std::uint32_t ClassFor(PolicyOptions const& options, std::uint64_t guid)
{
    if (options.ClassOf)
        if (std::uint32_t const own = options.ClassOf(guid))
            return own;
    return options.ClassId;
}

bool NoAmmo(BotSnapshot const& bot, std::uint32_t classId)
{
    return classId == kHunterClass && bot.ammo_entry != 0 && bot.ammo_count == 0;
}

bool CanPay(spell_cost::SpellCostData const* costs, std::uint32_t spell, BotSnapshot const& bot)
{
    if (!costs)
        return true;
    auto const payment = costs->CostAtLevel(spell, bot.level);
    if (!payment || payment->PowerType != std::int32_t(bot.power_type))
        return true;
    return bot.power >= payment->Cost;
}

supply::Facts FactsOf(BotSnapshot const& bot)
{
    supply::Facts facts;
    facts.AmmoEntry = bot.ammo_entry;
    facts.AmmoCount = bot.ammo_count;
    facts.FoodCount = bot.food_count;
    facts.DrinkCount = bot.drink_count;
    facts.ManaUser = bot.power_type == 0 && bot.max_power > 0;
    facts.Copper = bot.copper;
    facts.FreeSlots = bot.bag_free_slots;
    facts.AmmoVendor = bot.ammo_vendor_valid != 0;
    facts.FoodVendor = bot.food_vendor_valid != 0;
    facts.HealthPotions = bot.health_potion_count;
    facts.ManaPotions = bot.mana_potion_count;
    facts.Bandages = bot.bandage_count;
    facts.GoodsVendor = bot.goods_vendor_valid != 0;
    facts.ReagentEntry = bot.reagent_entry;
    facts.ReagentCount = bot.reagent_count;
    facts.ReagentVendor = bot.reagent_vendor_valid != 0;
    return facts;
}

} // namespace bot_policy_detail
