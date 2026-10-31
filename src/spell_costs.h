// SPDX-License-Identifier: GPL-2.0-or-later
// What a spell costs in power (mana, rage, energy), from the file the core writes at startup (worldserver.conf: Bot.Dump.SpellCosts = <file>; the module
// reads it as SpellCostFile, data/spell_costs.txt). With it the policy does not try a cast the bot cannot pay for: about 22,000 casts in 20 minutes
// were refused as "not enough power" on 300 bots (a warrior has no rage out of combat, so Battle Shout was tried every two seconds, for ever).
//
// File format, tab separated, '#' starts a comment line:
//   spell_id  level  power_type  cost  cost_per_level  cost_pct  next_rank  name
// The costs are in the core's units (the ones the bot's power in the snapshot uses; rage is tenths of a point). A spell whose cost depends on the
// caster's level or on a percentage of the base mana is not priced here (its cost is unknown, so the bot just tries it).
#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace spell_cost
{

struct Entry
{
    std::uint32_t Level = 0;       // the level the spell needs
    std::int32_t PowerType = 0;    // 0 mana, 1 rage, 3 energy...
    std::int32_t Cost = 0;         // flat cost
    std::int32_t PerLevel = 0;     // cost added per level (0 for the spells priced here)
    float Percent = 0.0f;          // percent of the base mana (0 for the spells priced here)
    std::uint32_t NextRank = 0;    // the next rank's spell id, 0 for the last rank
};

struct Payment
{
    std::int32_t PowerType = 0;
    std::uint32_t Cost = 0;
};

class SpellCostData
{
public:
    std::unordered_map<std::uint32_t, Entry> BySpell;

    // What a bot of `level` pays for the spell whose first rank is `firstRank`: the cost of the highest rank it can know at that level (the core casts
    // the highest rank the bot knows). Nothing when the spell is not in the file or its cost is not a flat one.
    std::optional<Payment> CostAtLevel(std::uint32_t firstRank, std::uint32_t level) const;
};

SpellCostData ParseSpellCosts(std::istream& in, std::vector<std::string>& warnings);
SpellCostData LoadSpellCosts(std::string const& path, std::vector<std::string>& warnings);

} // namespace spell_cost
