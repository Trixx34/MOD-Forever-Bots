// Class rotations of the bots (milestone M2). The rule evaluator is ported from the forever branch's BotRotation
// (src/server/game/Bots/BotRotation.{h,cpp}, pure part, GPL-2.0-or-later as in TrinityCore). The warrior rows are the
// forever spell table's warrior rows, rank 1 ids; the core casts the highest rank the bot knows.
//
// Pure logic over Tier 2 snapshot types: no SDK state, unit-tested in tests/rotation_test.cpp.
#pragma once

#include "forever_module/tier2_session.h"

#include <cstdint>
#include <iosfwd>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace rotation {

enum class Cond : std::uint8_t
{
    Always,
    TargetHpAbove,     // Param: percent
    TargetHpBelow,     // Param: percent
    SelfHpBelow,       // Param: percent
    SelfPowerBelow,    // Param: percent of the main resource
    SelfPowerAbove,    // Param: percent
    EnemiesAtLeast,    // Param: hostile units close to the bot
    TargetCasting,
    TargetFleeing,
    Opener,            // Param: seconds from the start of the fight
    TargetStrong,      // the target is elite, or Param or more levels above the bot
    ComboPointsAtLeast, // Param: combo points the bot holds on its target, counted from its own Sinister Strikes
    EnemiesBelow,      // Param: fewer hostile units close to the bot than this (a single-target spell: 'enemies_below 2')
    InParty,           // the bot is in a group (a warrior in a party holds Defensive Stance)
    NoPet,             // the snapshot reports no pet (pet_state 0): a summon spell is due. A dead pet (state 2) is not 'no pet'
    SelfAuraBelow,     // Param: aura spell id, Param2: stacks. The bot has fewer stacks of the aura than this (0 stacks when it is missing)
    SelfAuraAtLeast,   // Param: aura spell id, Param2: stacks. The bot carries the aura with this many stacks or more (a proc such as Missile Barrage)
};

// What a rule reads. Built from a snapshot by FactsFor().
struct Facts
{
    std::int32_t SelfHpPct = 100;
    std::int32_t SelfPowerPct = 100;
    std::int32_t TargetHpPct = 100;
    std::uint32_t EnemiesOnBot = 0;
    bool TargetCasting = false;
    bool TargetFleeing = false;   // not in the snapshot yet, so always false
    bool TargetElite = false;
    std::int32_t TargetLevelDiff = 0;
    std::uint64_t FightMs = 0;
    std::int32_t ComboPoints = 0;
    bool InParty = false;
    std::uint32_t PetState = 0;                   // the snapshot's pet_state: 0 none, 1 alive, 2 dead
    std::uint32_t AuraCount = 0;                  // the bot's own auras (layout 13), as many as the snapshot lists
    std::uint32_t AuraSpell[16] = {};
    std::uint32_t AuraStacks[16] = {};
};

struct Rule
{
    Cond Condition = Cond::Always;
    std::int32_t Param = 0;
    std::int32_t Param2 = 0;
};

bool Allowed(Rule const& rule, Facts const& facts);

// One spell of a rotation. OnTarget false casts on the bot itself. For OnTarget rows the target must be between MinRange
// and MaxRange yards (MaxRange 0 means no limit).
struct Row
{
    std::uint32_t Spell;
    Rule When;
    bool OnTarget;
    float MinRange;
    float MaxRange;
    // From a rotation file (RotationData); the built-in rows leave these at their defaults.
    std::uint32_t MinLevel = 0;     // the bot's level that learns the spell (0 = the built-in LearnLevel table decides)
    std::uint32_t MaxLevel = 0;     // not used above this level, when a better spell replaces it (0 = no limit)
    std::uint32_t DurationSec = 0;  // how long the effect lasts: a damage-over-time effect or a buff is not recast while it runs
    bool Buff = false;              // an upkeep spell cast out of combat, not part of the fight rotation
    bool Party = false;             // a buff: cast on every member of the bot's group (itself included), not only on the bot
};

// Rotations read from a file (data/rotations.txt, made by tools/make_rotations.py), per class id. A class missing from the file keeps the built-in rows.
//   class <id> [spec <key>]            the rows below belong to the class, or to one spec of it (a spec with no block uses the class's)
//   row <spell> [level <n>] [maxlevel <n>] [duration <sec>] <condition> self|target [range <min> <max>]       a fight spell, in priority order
//   buff <spell> [level <n>] [maxlevel <n>] duration <sec> self|party                                        an upkeep spell, cast out of combat when it has run out
// Conditions: always | target_hp_above N | target_hp_below N | self_hp_below N | self_power_below N | self_power_above N | enemies N |
// target_casting | opener N | target_strong N | combo N | enemies_below N | in_party | no_pet | self_aura_below <spell> <stacks> | self_aura_at_least <spell> <stacks>.
struct RotationData
{
    std::map<std::uint32_t, std::vector<Row>> ByClass;                          // `class <id>`: the class's default rows, and the fallback for a spec with no block
    std::map<std::pair<std::uint32_t, std::string>, std::vector<Row>> BySpec;   // `class <id> spec <key>`: the rows of one spec
};

// Rows for a class and spec, in priority order: the spec's block, else the class's default block, else nullptr (the caller uses the built-in rows). An empty spec is the default.
std::vector<Row> const* RowsFor(RotationData const& data, std::uint32_t classId, std::string const& spec);

// Specs. A bot's spec is the optional third field of its scenario roster entry (race:class:spec); the keys are the SDK's spec keys, lower case. Role 1 tank, 2 healer, 3 dps.
//   hunter bm mm sv; warrior arms fury prot; paladin holy prot ret; priest disc holy shadow; shaman ele enh resto; mage arcane fire frost;
//   warlock affliction demonology destruction; rogue assassination combat subtlety; druid balance feral_tank feral_cat resto
// SpecRole is 0 for a key the class does not have. DefaultSpec is what a bot of the class and role plays without a spec in its roster entry:
//   tank: warrior prot, paladin prot, druid feral_tank; healer: priest holy, paladin holy, shaman resto, druid resto;
//   dps: warrior arms, paladin ret, hunter bm, rogue combat, priest shadow, shaman enh, mage arcane, warlock demonology, druid feral_cat. Empty for a class with no such role.
std::uint32_t SpecRole(std::uint32_t classId, std::string const& spec);
std::string DefaultSpec(std::uint32_t classId, std::uint32_t role);

RotationData ParseRotationData(std::istream& in, std::vector<std::string>& warnings);
RotationData LoadRotationData(std::string const& path, std::vector<std::string>& warnings);

// Rows in priority order, for a class id (1 = warrior). Empty for a class id without rows.
std::span<Row const> RowsForClass(std::uint32_t classId);

// The level at which a trainer teaches the rank 1 spell, from the Forever trainer data (the spell ids in the rows are the same as in
// vanilla, and so are these levels). 0 for a spell a class has from level 1 and for a spell not in the table; a bot below the learn
// level of a spell does not know it yet, so the rotation skips the row until the bot reaches it.
std::uint32_t LearnLevel(std::uint32_t spell);

// How far the bot approaches its target before it attacks or casts: 25 yards for the ranged classes, 5 for the rest.
float ApproachYards(std::uint32_t classId);

// Distance in yards on the ground between the bot and a unit in the snapshot.
float DistanceYards(forever_module::BotSnapshot const& bot, forever_module::NearbyUnit const& unit);

// The same in three dimensions: what the core's spell range and loot distance checks measure (up to the units' reaches). A
// target on a ledge above the bot is farther than its ground distance says.
float DistanceYards3d(forever_module::BotSnapshot const& bot, forever_module::NearbyUnit const& unit);

// Facts for the rules: health and power percentages, the target's state and level, hostiles close to the bot.
// target may be null (the target is not in the snapshot's view).
rotation::Facts FactsFor(forever_module::BotSnapshot const& bot, std::span<forever_module::NearbyUnit const> nearby,
                         forever_module::NearbyUnit const* target, std::uint64_t fightMs);

} // namespace rotation
