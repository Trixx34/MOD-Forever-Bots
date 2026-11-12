// SPDX-License-Identifier: GPL-2.0-or-later
// HealDownrank (protocol 28): which rank of a class's single-target heal to cast for a deficit. kCastSpell with BotAction::dest_x = 1 makes the core cast exactly that spell id
// (kNotKnown for a rank the bot has not learned); dest_x 0 casts the highest known rank, as before. The ladders and the amounts are src/heal_ranks_data.h (generated).
#pragma once

#include "heal_ranks_data.h"

#include <cstdint>
#include <limits>

namespace heal_ranks
{
// The ladder HealSpellFor's spell belongs to (a priest below level 16 heals with Lesser Heal, from 16 with Heal), or -1 for a class with no direct heal here.
inline int ChainFor(std::uint32_t classId, std::uint32_t level)
{
    switch (classId)
    {
        case 5: return level >= 16 ? int(kChainHeal) : int(kChainLesserHeal);
        case 2: return int(kChainHolyLight);
        case 7: return int(kChainHealingWave);
        case 11: return int(kChainHealingTouch);
        default: return -1;
    }
}

// The spell id of the lowest rank, among those the bot has the level for and the mana for, that heals at least `deficit`; when none does, the highest such rank (a bigger heal
// is the best there is). 0 when the class has no ladder or no rank is affordable: the caller then casts as before (the core picks the highest known rank).
// `level` 0 means any level; `mana` is the bot's current mana (pass the maximum for a bot that does not use mana).
inline std::uint32_t PickRank(std::uint32_t classId, std::uint32_t level, std::uint32_t deficit, std::uint32_t mana = std::numeric_limits<std::uint32_t>::max())
{
    int const chain = ChainFor(classId, level);
    if (chain < 0)
        return 0;
    std::uint32_t best = 0;
    for (Rank const& rank : kRanks)
    {
        if (rank.Chain != std::uint32_t(chain) || (level != 0 && rank.Level > level) || rank.Mana > mana)
            continue;
        best = rank.Spell;               // ranks are lowest first: the last one that fits is the highest
        if (rank.Amount >= deficit)
            return rank.Spell;
    }
    return best;
}

// True when `spell` is a rank of any ladder.
inline bool IsLadderSpell(std::uint32_t spell)
{
    for (Rank const& rank : kRanks)
        if (rank.Spell == spell)
            return true;
    return false;
}
}
