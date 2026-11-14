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
//   avoid|map|object|entry|radius|name         a game object of this entry (kept, not acted on: write a trap game object as a spell row with its trap spell, kind 3 ground effects)
//   swap|map|boss_entry|aura_id|stacks|name    the tank holding `stacks` of this aura on it is swapped for another tank
//   noaoe|map|boss_entry|name                  no area damage in this fight (boss_entry 0 = every fight on the map)
//   frontal|map|boss_entry|spell_id|name       a frontal cast of the boss (the telegraph escape; the positioning is tank_spot / flank)
//   spread|map|boss_entry|yards|who|name      while an engaged hostile of this entry is in view (0 = any), the planner's raid members of group `who` stay at least `yards` apart (chain lightning, bombs, Holy Wrath); who: all, ranged, healers or melee; tanks never move
//   avoid_enemy|map|entry|yards|who|name       (who also `tanks`, see kWhoTanks) a member of group `who` keeps at least `yards` from a live hostile of this entry (Holy Nova, a charging add, melee reach)
//   avoid_enemy_aura|map|entry|yards|who|aura_id|name   the same, but only while the hostile carries the aura (Axe Flurry 24018). The snapshot has no aura list for
//                                              nearby units, only target_auras of a bot's current target: the circle is dropped only when a bot that has the unit as its
//                                              target sees a complete aura list without the aura; with no such bot (or a full list) the circle stays, as avoid_enemy
//   purge|map|aura_id|name                     an aura on an enemy that a priest (Dispel Magic), shaman (Purge) or hunter (Tranquilizing Shot) removes, seen in the target_auras of a member's current target
//   mindcontrol|map|aura_id|name               a member carrying this aura is mind controlled: nobody attacks it and a mage polymorphs it
//   leash|map|yards|name                       the route leader of a dungeon party pulls only hostiles within `yards` (10..80) of the route leg on this map (narrows DungeonPullLeashYards, never widens it); it also narrows the pick of a new target by a party in a fight (the chase leash), not the defence against what attacks
//   pullpos|map|entry|x|y|z|yards[|name]       the tank pulls a creature of this entry from this position (outside its throw range, or behind a corner): it walks to within `yards` (2..40) of the point
//                                              before it pulls, goes back to it once the pull has landed, and the other members stay where they are until the tank is in the fight
//   tank_spot|map|boss_entry|x|y|z|yards|name  the tank the boss is attacking holds it from this point (within `yards`, 1..15): the boss turns to face the tank, so the point
//                                              sets which way its frontal casts go (Onyxia: north of her spawn, she faces the north wall). Module switch RaidBossPositions
//   flank|map|boss_entry|front_deg|rear_deg|rear_yards|name   the members other than the boss's victim stand on its flanks: none in the front cone (front_deg wide, any
//                                              distance) or in the rear cone (rear_deg wide, within rear_yards: a tail sweep). Module switch RaidBossPositions
//   pull_single|map|entry|yards|name           creatures of this entry are pulled one at a time (Onyxian Warders, Fire Nova): the route leader does not pull one while another
//                                              live one of the entry stands within `yards` (5..60) of it, and holds its walk while such a pair is near; in a fight nobody picks
//                                              one that fights nobody. Module switch RaidPullSingle; the ghost hold RaidGhostFightHoldMaxMs acts on maps with such a row
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

// Who a spread or avoid_enemy row applies to (a bit mask): melee damage dealers, ranged damage dealers, healers; `all` is these three. `tanks` (8, not part of `all`)
// is read only by avoid_enemy rows and only with the setting RaidAvoidEnemyTank; a spread row never moves a tank. Melee rows of avoid_enemy act only with RaidAvoidEnemyMelee.
constexpr std::uint8_t kWhoMelee = 1, kWhoRanged = 2, kWhoHealers = 4, kWhoAll = 7, kWhoTanks = 8;

struct SpreadRule
{
    std::uint32_t Entry = 0;        // the hostile's creature entry, 0 = any engaged hostile of the map
    float Yards = 0.0f;
    std::uint8_t Who = kWhoAll;
};

struct EnemyZone
{
    std::uint32_t Entry = 0;
    float Yards = 0.0f;
    std::uint8_t Who = kWhoAll;
    std::uint32_t Aura = 0;     // avoid_enemy_aura: the circle holds only while the unit carries this aura; 0 = always (avoid_enemy)
};

struct PullPosition
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    float Yards = 0.0f;
};

// tank_spot: where the tank holds the boss from (the boss faces the tank, so it faces from its own position toward this point).
struct TankSpot
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    float Yards = 0.0f;
};

// flank: the boss's cones nobody but its victim stands in. Widths in degrees (a 120 degree cone is 60 either side of the facing).
struct FlankRule
{
    float FrontDeg = 0.0f;
    float RearDeg = 0.0f;
    float RearYards = 0.0f;
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
    std::uint32_t PullLeash = 0;                            // yards, 0 = none
    std::vector<SpreadRule> Spread;
    std::vector<EnemyZone> AvoidEnemy;
    std::map<std::uint32_t, std::string> Purge;             // enemy aura id -> name
    std::map<std::uint32_t, std::string> MindControl;       // aura id on a member -> name
    std::map<std::uint32_t, PullPosition> PullPositions;    // creature entry -> where the tank pulls it from
    std::map<std::uint32_t, TankSpot> TankSpots;            // boss entry -> where its tank holds it from
    std::map<std::uint32_t, FlankRule> Flanks;              // boss entry -> its front and rear cones
    std::map<std::uint32_t, float> PullSingle;              // creature entry -> the yards within which a second one of the entry makes a pair (pull_single)
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
    // The map's own pull leash in yards (0 = the map has none): the policy uses the smaller of this and DungeonPullLeashYards.
    std::uint32_t PullLeash(std::uint32_t map) const;
    // The spread rows that apply to an engaged hostile of `entry` (rows of entry 0 apply to every one).
    std::vector<SpreadRule> SpreadFor(std::uint32_t map, std::uint32_t entry) const;
    // The avoid_enemy rows of a hostile of `entry`.
    std::vector<EnemyZone> AvoidEnemyFor(std::uint32_t map, std::uint32_t entry) const;
    bool HasAvoidEnemy(std::uint32_t map) const;
    bool IsPurgeAura(std::uint32_t map, std::uint32_t aura) const;
    bool IsMindControlAura(std::uint32_t map, std::uint32_t aura) const;
    // Where the tank pulls a creature of this entry from, nullopt when the map has no `pullpos` row for it.
    std::optional<PullPosition> PullPositionOf(std::uint32_t map, std::uint32_t entry) const;
    bool IsFrontal(std::uint32_t map, std::uint32_t bossEntry, std::uint32_t spell) const;
    // A frontal row of any boss of the map names the spell (a telegraph carries a caster ref, not a boss entry).
    bool IsFrontalAny(std::uint32_t map, std::uint32_t spell) const;
    // The map lists the spell as an `avoid|spell` row: a lethal-class cast the whole raid steps out of, casters mid-cast included.
    bool IsLethalSpell(std::uint32_t map, std::uint32_t spell) const;
    // The boss's `tank_spot` / `flank` row, nullopt when the map has none for it.
    std::optional<TankSpot> TankSpotOf(std::uint32_t map, std::uint32_t bossEntry) const;
    std::optional<FlankRule> FlankOf(std::uint32_t map, std::uint32_t bossEntry) const;
    bool HasTankSpot(std::uint32_t map) const;
    // The `pull_single` yards of a creature entry (0 = the map has no row for it), and whether the map has any such row.
    float PullSingleYards(std::uint32_t map, std::uint32_t entry) const;
    bool HasPullSingle(std::uint32_t map) const;

private:
    std::map<std::uint32_t, MapRules> _maps;
};

// Adds the map's add ranks to a kill priority map (creature entry -> rank) without replacing an entry the dungeon file already ranks.
void MergeKillPriority(Table const& table, std::uint32_t map, std::map<std::uint32_t, std::uint32_t>& killPriority);

// Reads the text. A bad row is reported in `errors` ("line N: ...") and skipped; the good rows are kept.
Table Parse(std::string_view text, std::vector<std::string>& errors);

// Boss positioning geometry (RaidBossPositions), pure. Angles: WoW orientation, radians, 0 = +x (north), counter-clockwise; the degree values are relative to the facing.
// The facing to plan with: from the boss toward the tank spot while the boss stands within 1..kTankSpotFacingYards of it (the tank holds it there; robust to a snapshot
// without orientation and to the boss turning for a moment), else the boss's own orientation.
constexpr float kTankSpotFacingYards = 12.0f;
float PlannedFacing(std::optional<TankSpot> const& spot, float bossX, float bossY, float orientation);
// The bearing of (x, y) seen from the boss, relative to its facing, in degrees in (-180, 180]: 0 straight ahead, +90 its left (counter-clockwise), 180 behind.
float RelativeBearingDeg(float bossX, float bossY, float facing, float x, float y);
// A point at this relative bearing and distance is inside the front cone (any distance) or the rear cone (within RearYards), each widened by `marginDeg` per side.
bool InBossCone(FlankRule const& rule, float relDeg, float yards, float marginDeg);
// The stand bearing (degrees, relative to the facing) of slot `slot` of `slots` on one side (side > 0 the left, else the right): the flank sector between the cones,
// each edge kept `marginDeg` off, split into `slots` equal parts and the middle of part `slot` taken.
float FlankBearingDeg(FlankRule const& rule, int side, unsigned slot, unsigned slots, float marginDeg);

} // namespace raid_rules
