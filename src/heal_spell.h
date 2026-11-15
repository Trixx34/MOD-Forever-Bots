// SPDX-License-Identifier: GPL-2.0-or-later
// HealSpellMix (config, default off): which heal spell a healer casts on a target, instead of the one spell per class HealSpellFor names. Pure decision logic, no core
// needed (tests/heal_spell_test.cpp); the rank ladders and their amounts are src/heal_ranks_data.h (generated from the client data, no spell power).
//
// The choice follows the target's missing health, the damage it takes, whether it is the tank, the healer's mana and what a rank costs:
//  - urgent (health low, or the damage taken would empty it in about three seconds): the fast heal (priest Flash Heal, paladin Flash of Light, shaman Lesser Healing Wave, druid
//    Regrowth), the lowest rank that covers the deficit, else the highest the bot can pay;
//  - safe: the efficient big heal (priest Greater Heal from 40, else Heal; paladin Holy Light; shaman Healing Wave; druid Healing Touch) at the lowest rank that covers the
//    deficit (downrank saves mana); a paladin or shaman with a small deficit uses the cheap fast heal when it covers it;
//  - the tank: a pulling tank gets a shield (priest) or a heal over time (Renew, Rejuvenation) before it is hit, and between pulls a small deficit is a heal over time, not a direct heal;
//  - paladin Lay on Hands for a target nearly dead; shaman Chain Heal when three or more members are hurt;
//  - low mana: the cheapest rank that is worth casting, and no fast heal unless it is urgent.
// A deficit smaller than a third of the smallest heal on offer is no cast at all (Skip): no overheal spam.
// Every spell is used only when the bot's level has it (the ladder's learn levels, and the first-rank levels below); ranks are named exactly (BotAction::dest_x = 1).
#pragma once

#include "heal_rank.h"

#include <algorithm>
#include <cstdint>

namespace heal_spell
{
// First ranks and learn levels of the spells that are not a ladder here (heal over time, shield, Lay on Hands; the core casts the highest rank known for them).
inline constexpr std::uint32_t kPowerWordShield = 17, kPowerWordShieldLevel = 6;
inline constexpr std::uint32_t kRenew = 139, kRenewLevel = 8;
inline constexpr std::uint32_t kRejuvenation = 774, kRejuvenationLevel = 4;
inline constexpr std::uint32_t kLayOnHands = 633, kLayOnHandsLevel = 10;
inline constexpr std::uint32_t kPriest = 5, kPaladin = 2, kShaman = 7, kDruid = 11;

struct Input
{
    std::uint32_t ClassId = 0;
    std::uint32_t Level = 0;
    std::uint32_t Mana = 0;              // the healer's mana now
    std::uint32_t MaxMana = 1;
    std::uint32_t Health = 0;            // the target's
    std::uint32_t MaxHealth = 1;
    std::uint32_t IncomingPerSec = 0;    // the damage the target took per second lately (0 = unknown or none)
    bool IsTank = false;                 // the target is the one playing the tank
    bool InCombat = false;               // the group is fighting (the tank has a target)
    std::uint32_t HurtCount = 0;         // members of the group under 75 % health in range
    bool HotActive = false;              // the healer's heal over time (Renew, Rejuvenation) still runs on the target
    bool ShieldLocked = false;           // a Power Word: Shield was cast on it lately (Weakened Soul)
    bool LayOnHandsReady = true;
    bool Proactive = false;              // no target is hurt enough for a heal: only a shield or heal over time on the pulling tank is asked for
};

struct Choice
{
    std::uint32_t Spell = 0;   // 0 with !Skip: the plan has no opinion, the caller casts as before
    bool Exact = false;        // a ladder rank named exactly (dest_x = 1); false for a spell the core casts at its highest known rank
    bool Skip = false;         // nothing worth casting: the target is not hurt enough for the smallest heal
    char const* Note = "";
};

inline std::uint32_t Pct(std::uint32_t health, std::uint32_t maxHealth)
{
    return maxHealth == 0 ? 100u : std::uint32_t(std::uint64_t(health) * 100 / maxHealth);
}

// The highest rank of `chain` the level has (any mana) and what it heals; 0 when the level has none.
inline std::uint32_t BestAmount(std::uint32_t chain, std::uint32_t level)
{
    std::uint32_t amount = 0;
    for (heal_ranks::Rank const& rank : heal_ranks::kRanks)
        if (rank.Chain == chain && rank.Level <= level)
            amount = rank.Amount;
    return amount;
}

// The lowest rank of `chain` that covers `deficit`, else the highest affordable one (0 when the bot has none or cannot pay any).
inline std::uint32_t Rank(std::uint32_t chain, std::uint32_t level, std::uint32_t deficit, std::uint32_t mana)
{
    std::uint32_t best = 0;
    for (heal_ranks::Rank const& rank : heal_ranks::kRanks)
    {
        if (rank.Chain != chain || rank.Level > level || rank.Mana > mana)
            continue;
        best = rank.Spell;
        if (rank.Amount >= deficit)
            return rank.Spell;
    }
    return best;
}

// Smallest amount of the lowest affordable rank of `chain` (what a heal of it is worth at least); 0 when none.
inline std::uint32_t SmallestAmount(std::uint32_t chain, std::uint32_t level, std::uint32_t mana)
{
    for (heal_ranks::Rank const& rank : heal_ranks::kRanks)
        if (rank.Chain == chain && rank.Level <= level && rank.Mana <= mana)
            return rank.Amount;
    return 0;
}

// The first rank of the ladder `spell` belongs to (what to cast, without naming a rank, when the core does not know the exact one); `spell` itself when it is in no ladder.
inline std::uint32_t FirstRankOf(std::uint32_t spell)
{
    std::uint32_t chain = 0;
    bool found = false;
    for (heal_ranks::Rank const& rank : heal_ranks::kRanks)
        if (rank.Spell == spell)
        {
            chain = rank.Chain;
            found = true;
        }
    if (found)
        for (heal_ranks::Rank const& rank : heal_ranks::kRanks)
            if (rank.Chain == chain)
                return rank.Spell;
    return spell;
}

inline Choice Of(std::uint32_t spell, char const* note, bool exact = true)
{
    Choice choice;
    choice.Spell = spell;
    choice.Exact = exact;
    choice.Note = note;
    return choice;
}

inline Choice Choose(Input const& in)
{
    using namespace heal_ranks;
    Choice none;
    if (in.MaxHealth == 0 || (in.Health >= in.MaxHealth && !in.Proactive && !(in.IsTank && in.InCombat)))
        return none;
    std::uint32_t const deficit = in.MaxHealth > in.Health ? in.MaxHealth - in.Health : 0;
    std::uint32_t const pct = Pct(in.Health, in.MaxHealth);
    std::uint32_t const manaPct = Pct(in.Mana, in.MaxMana);
    bool const lowMana = manaPct < 25;
    // About three seconds from empty, or nearly empty: the slow big heal would land after it is dead.
    bool const urgent = pct < 35 || (in.IncomingPerSec > 0 && in.Health <= std::uint64_t(in.IncomingPerSec) * 3);
    bool const tankPull = in.IsTank && in.InCombat;
    // The fast or big heal tier: an emergency, or a tank under half.
    bool const fastTier = urgent || (in.IsTank && pct < 50);
    // Mana reserve gates: with little mana nobody well off is topped up (a tank a bit later), and in a fight the last mana is kept for the members who are hurt.
    if (!in.Proactive && !urgent && ((manaPct < 30 && pct > (in.IsTank ? 85u : 70u)) || (in.InCombat && manaPct < 45 && pct > (in.IsTank ? 75u : 65u))))
    {
        Choice skip;
        skip.Skip = true;
        skip.Note = "mana_reserve";
        return skip;
    }
    if (deficit == 0 && !(in.Proactive || tankPull))
        return none;

    // A heal over time or a shield before the tank is hit, or between pulls instead of a direct heal for a small deficit.
    auto const hotFor = [&]() -> Choice
    {
        if (in.HotActive)
            return none;
        if (in.ClassId == kPriest && in.Level >= kRenewLevel)
            return Of(kRenew, "hot", false);
        if (in.ClassId == kDruid && in.Level >= kRejuvenationLevel)
            return Of(kRejuvenation, "hot", false);
        return none;
    };
    if (in.Proactive)
    {
        if (!tankPull || urgent)
            return none;
        // Shield economy: the worst heal per mana, so only with real damage coming in, a tank not full, and the mana for it.
        if (in.ClassId == kPriest && in.Level >= kPowerWordShieldLevel && !in.ShieldLocked && manaPct >= 50 && in.IncomingPerSec >= in.Level * 3 && pct <= 85)
            return Of(kPowerWordShield, "pull_shield", false);
        return hotFor();
    }

    // Lay on Hands: the target is about to die and a heal would land late (it drains the paladin's mana and has a long cooldown, so only for a nearly dead target).
    if (in.ClassId == kPaladin && in.Level >= kLayOnHandsLevel && in.LayOnHandsReady && pct < 15 && in.IncomingPerSec > 0)
        return Of(kLayOnHands, "lay_on_hands", false);

    // Chain Heal: three or more hurt and the mana to spare.
    if (in.ClassId == kShaman && in.HurtCount >= 3 && !lowMana)
        if (std::uint32_t const spell = Rank(kChainChainHeal, in.Level, deficit, in.Mana))
            return Of(spell, "chain");

    // Between pulls (or a tank at a small deficit): a heal over time instead of a direct heal, at no mana wasted on a heal that overheals.
    if (in.IsTank && !in.InCombat && !urgent)
    {
        std::uint32_t const direct = in.ClassId == kPriest ? BestAmount(in.Level >= 16 ? kChainHeal : kChainLesserHeal, in.Level) : 0;
        if ((in.ClassId == kPriest || in.ClassId == kDruid) && (direct == 0 || deficit * 2 < direct) && !in.HotActive)
        {
            Choice const hot = hotFor();
            if (hot.Spell != 0)
                return hot;
        }
    }
    if (tankPull && !urgent && deficit * 4 < in.MaxHealth && !in.HotActive && !lowMana)
    {
        Choice const hot = hotFor();   // pulling tank only slightly hurt: the heal over time goes on first
        if (hot.Spell != 0)
            return hot;
    }
    if (in.ClassId == kDruid && !urgent && !in.IsTank && !in.HotActive && pct >= 50 && !lowMana)
        if (in.Level >= kRejuvenationLevel && deficit * 6 < in.MaxHealth * 2)   // a member only a little hurt: Rejuvenation is the cheap heal
            return Of(kRejuvenation, "hot", false);

    // Direct heals.
    int fastChain = -1, bigChain = -1, smallChain = -1;
    switch (in.ClassId)
    {
        case kPriest:
            fastChain = in.Level >= 20 ? int(kChainFlashHeal) : -1;
            bigChain = in.Level >= 40 ? int(kChainGreaterHeal) : (in.Level >= 16 ? int(kChainHeal) : int(kChainLesserHeal));
            smallChain = in.Level >= 16 ? int(kChainHeal) : int(kChainLesserHeal);   // the cheap rank ladder below Greater Heal
            break;
        case kPaladin:
            fastChain = in.Level >= 20 ? int(kChainFlashOfLight) : -1;
            bigChain = int(kChainHolyLight);
            smallChain = fastChain;
            break;
        case kShaman:
            fastChain = in.Level >= 20 ? int(kChainLesserHealingWave) : -1;
            bigChain = int(kChainHealingWave);
            smallChain = bigChain;
            break;
        case kDruid:
            fastChain = in.Level >= 12 ? int(kChainRegrowth) : -1;
            bigChain = int(kChainHealingTouch);
            smallChain = bigChain;
            break;
        default:
            return none;
    }
    auto const pick = [&](int chain, char const* note) -> Choice
    {
        if (chain < 0)
            return none;
        std::uint32_t const spell = Rank(std::uint32_t(chain), in.Level, deficit, in.Mana);
        return spell != 0 ? Of(spell, note) : none;
    };
    // Nothing worth casting: the deficit is under a third of the smallest heal on offer.
    std::uint32_t floorAmount = SmallestAmount(std::uint32_t(bigChain), in.Level, in.Mana);
    if (smallChain >= 0)
    {
        std::uint32_t const s = SmallestAmount(std::uint32_t(smallChain), in.Level, in.Mana);
        if (s != 0 && (floorAmount == 0 || s < floorAmount))
            floorAmount = s;
    }
    if (floorAmount != 0 && deficit * 3 < floorAmount && !tankPull)
    {
        Choice skip;
        skip.Skip = true;
        skip.Note = "no_overheal";
        return skip;
    }
    if (fastTier)
    {
        // The fast heal when it covers a fair share of the deficit, else the big one (a bigger number lands slower but saves more).
        std::uint32_t const fastTop = fastChain >= 0 ? BestAmount(std::uint32_t(fastChain), in.Level) : 0;
        if (fastTop != 0 && (fastTop * 2 >= deficit || bigChain < 0))
            if (Choice const c = pick(fastChain, "fast"); c.Spell != 0)
                return c;
        if (Choice const c = pick(bigChain, "big"); c.Spell != 0)
            return c;
        return pick(fastChain, "fast");
    }
    // Safe: a small deficit takes the cheap heal when it covers it (paladin Flash of Light, priest Heal at low level), a big one the big heal; low mana takes the cheapest rank that is worth it.
    if (lowMana)
        if (Choice const c = pick(smallChain, "cheap"); c.Spell != 0)
            return c;
    if (in.ClassId == kPaladin && fastChain >= 0 && deficit <= BestAmount(std::uint32_t(fastChain), in.Level))
        if (Choice const c = pick(fastChain, "small"); c.Spell != 0)
            return c;
    if (in.ClassId == kPriest && bigChain == int(kChainGreaterHeal) && deficit <= BestAmount(kChainHeal, in.Level))
        if (Choice const c = pick(kChainHeal, "small"); c.Spell != 0)   // Heal covers it for less mana than Greater Heal
            return c;
    return pick(bigChain, "big");
}
}
