// SPDX-License-Identifier: GPL-2.0-or-later
// The raid rule table (data/raid_rules.txt, config RaidRulesFile): per raid map the facts a raid fight needs that the dungeon file's per-boss
// strategy does not carry: the adds to kill first, the casts to interrupt first, which aura is dispelled with which kind of dispel, the ground
// areas to stay out of, the debuff that makes the tanks swap, and the fights where nobody uses area damage. Designed on our side as plain data plus
// small generic rules; nothing is taken from another project's code or tables. Pure logic: no core, no clock, no file access. The module reads the
// file and passes its text to Parse; the party planner and the policy ask the queries below (tests/raid_rules_test.cpp).
//
// One row per line, '|' separated, '#' starts a comment, a trailing name field is free text:
//   boss|map|entry|name
//   add|map|entry|rank|name                    kill priority: rank 0 first (lower first)
//   interrupt|map|spell_id|name                a cast the kickers take before any other cast
//   dispel|map|aura_id|kind|name               kind: magic, curse, disease or poison
//   avoid|map|spell|spell_id|radius|name       a ground effect (dynamic object or area trigger) of this spell: step out of it
//   avoid|map|object|entry|radius|name         a game object of this entry: step out of it (the snapshot has no game objects yet, see the README)
//   swap|map|boss_entry|aura_id|stacks|name    the tank holding `stacks` of this aura on it is swapped for another tank
//   noaoe|map|boss_entry|name                  no area damage in this fight (boss_entry 0 = every fight on the map)
//   frontal|map|boss_entry|spell_id|name       a frontal cast of the boss: the tank keeps the boss turned away from the raid (data only, see the README)
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace raid_rules {

// The values of PartyDebuff::dispel_type in the SDK: 1 magic, 2 curse, 3 disease, 4 poison.
enum class DispelKind : std::uint8_t { Magic = 1, Curse = 2, Disease = 3, Poison = 4 };

struct AvoidZone
{
    std::uint32_t Id = 0;       // a spell id (Object false) or a game object entry (Object true)
    bool Object = false;
    float Radius = 0.0f;
};

struct TankSwap
{
    std::uint32_t Boss = 0;
    std::uint32_t Aura = 0;
    std::uint32_t Stacks = 0;
};

struct MapRules
{
    std::map<std::uint32_t, std::string> Bosses;            // entry -> name
    std::map<std::uint32_t, std::uint32_t> Adds;            // entry -> kill rank
    std::set<std::uint32_t> InterruptFirst;                 // spell ids
    std::map<std::uint32_t, DispelKind> Dispels;            // aura id -> kind
    std::vector<AvoidZone> Avoid;
    std::vector<TankSwap> Swaps;
    std::set<std::uint32_t> NoAoe;                          // boss entries, 0 = the whole map
    std::map<std::uint32_t, std::set<std::uint32_t>> Frontal; // boss entry -> frontal spell ids
};

class Table
{
public:
    MapRules& Edit(std::uint32_t map) { return _maps[map]; }
    MapRules const* Find(std::uint32_t map) const;
    bool Empty() const { return _maps.empty(); }
    std::size_t MapCount() const { return _maps.size(); }

    bool IsBoss(std::uint32_t map, std::uint32_t entry) const;
    // The kill rank of an add (0 first), nullopt when the map does not list the creature.
    std::optional<std::uint32_t> AddRank(std::uint32_t map, std::uint32_t entry) const;
    bool InterruptFirst(std::uint32_t map, std::uint32_t spell) const;
    std::optional<DispelKind> DispelKindOf(std::uint32_t map, std::uint32_t aura) const;
    // The map's avoid zones of one kind (spells or objects).
    std::vector<AvoidZone> AvoidOf(std::uint32_t map, bool objects) const;
    // The stack count at which a tank holding `aura` is swapped, nullopt when the aura is not a swap aura of the map.
    std::optional<std::uint32_t> SwapStacks(std::uint32_t map, std::uint32_t aura) const;
    bool NoAoe(std::uint32_t map, std::uint32_t bossEntry) const;
    bool IsFrontal(std::uint32_t map, std::uint32_t bossEntry, std::uint32_t spell) const;
    // A frontal row of any boss of the map names the spell (a telegraph carries a caster ref, not a boss entry).
    bool IsFrontalAny(std::uint32_t map, std::uint32_t spell) const;
    // The map lists the spell as an `avoid|spell` row: a lethal-class cast the whole raid steps out of, casters mid-cast included.
    bool IsLethalSpell(std::uint32_t map, std::uint32_t spell) const;

private:
    std::map<std::uint32_t, MapRules> _maps;
};

// Adds the map's add ranks to a kill priority map (creature entry -> rank) without replacing an entry the dungeon file already ranks.
void MergeKillPriority(Table const& table, std::uint32_t map, std::map<std::uint32_t, std::uint32_t>& killPriority);

// Reads the text. A bad row is reported in `errors` ("line N: ...") and skipped; the good rows are kept.
Table Parse(std::string_view text, std::vector<std::string>& errors);

} // namespace raid_rules
