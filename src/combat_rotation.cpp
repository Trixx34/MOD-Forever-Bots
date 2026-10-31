// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from the forever branch's BotRotation.cpp (pure part) and BotCombat.cpp's warrior rows; see combat_rotation.h.
#include "combat_rotation.h"

#include "text_lines.h"
#include "text_number.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

using forever_module::BotSnapshot;
using forever_module::NearbyUnit;

namespace rotation {

namespace {

constexpr std::uint32_t kWarriorClass = 1;
constexpr std::uint32_t kPaladinClass = 2;
constexpr std::uint32_t kHunterClass = 3;
constexpr std::uint32_t kRogueClass = 4;
constexpr std::uint32_t kPriestClass = 5;
constexpr std::uint32_t kShamanClass = 7;
constexpr std::uint32_t kMageClass = 8;
constexpr std::uint32_t kWarlockClass = 9;
constexpr std::uint32_t kDruidClass = 11;

// The rows below are rank 1 spell ids, like the warrior rows. Heals and buffs on the bot have OnTarget false. Ranged
// rows reach 30 to 35 yards, the distance the policy approaches a ranged class to (ApproachYards).
constexpr Row kPaladinRows[] = {
    { 635, { Cond::SelfHpBelow, 50 }, false, 0.0f, 0.0f },   // Holy Light on self
    { 853, { Cond::TargetCasting, 0 }, true, 0.0f, 10.0f },  // Hammer of Justice to interrupt a cast
    { 21084, { Cond::Always, 0 }, false, 0.0f, 0.0f },       // Seal of Righteousness, the melee buff (last: it is always allowed)
};

constexpr Row kHunterRows[] = {
    { 1978, { Cond::Always, 0 }, true, 0.0f, 35.0f },        // Serpent Sting (damage over time)
    { 3044, { Cond::Always, 0 }, true, 0.0f, 35.0f },        // Arcane Shot
    { 75, { Cond::Always, 0 }, true, 0.0f, 35.0f },          // Auto Shot, the fallback
};

constexpr Row kRogueRows[] = {
    { 1766, { Cond::TargetCasting, 0 }, true, 0.0f, 5.0f },  // Kick to interrupt a cast
    { 2098, { Cond::ComboPointsAtLeast, 5 }, true, 0.0f, 5.0f }, // Eviscerate at five combo points, counted by the policy
    { 1752, { Cond::Always, 0 }, true, 0.0f, 5.0f },         // Sinister Strike
};

constexpr Row kPriestRows[] = {
    { 17, { Cond::SelfHpBelow, 50 }, false, 0.0f, 0.0f },   // Power Word: Shield on self
    { 2050, { Cond::SelfHpBelow, 35 }, false, 0.0f, 0.0f }, // Lesser Heal on self
    { 589, { Cond::Always, 0 }, true, 0.0f, 30.0f },        // Shadow Word: Pain (damage over time)
    { 8092, { Cond::Always, 0 }, true, 0.0f, 30.0f },       // Mind Blast
    { 585, { Cond::Always, 0 }, true, 0.0f, 30.0f },        // Smite
};

constexpr Row kShamanRows[] = {
    { 331, { Cond::SelfHpBelow, 40 }, false, 0.0f, 0.0f },   // Healing Wave on self
    { 8042, { Cond::TargetCasting, 0 }, true, 0.0f, 20.0f }, // Earth Shock to interrupt a cast
    { 8050, { Cond::Always, 0 }, true, 0.0f, 20.0f },        // Flame Shock (damage over time)
    { 403, { Cond::Always, 0 }, true, 0.0f, 30.0f },         // Lightning Bolt
};

constexpr Row kMageRows[] = {
    { 122, { Cond::EnemiesAtLeast, 2 }, false, 0.0f, 0.0f },   // Frost Nova when surrounded
    { 2136, { Cond::TargetHpBelow, 25 }, true, 0.0f, 20.0f },  // Fire Blast to finish a target (20 yards)
    { 133, { Cond::Always, 0 }, true, 0.0f, 30.0f },           // Fireball
};

constexpr Row kWarlockRows[] = {
    { 172, { Cond::Always, 0 }, true, 0.0f, 30.0f },        // Corruption (damage over time)
    { 707, { Cond::Always, 0 }, true, 0.0f, 30.0f },        // Immolate (damage over time)
    { 686, { Cond::Always, 0 }, true, 0.0f, 30.0f },        // Shadow Bolt
};

constexpr Row kDruidRows[] = {
    { 5185, { Cond::SelfHpBelow, 40 }, false, 0.0f, 0.0f }, // Healing Touch on self
    { 8921, { Cond::Always, 0 }, true, 0.0f, 30.0f },       // Moonfire (damage over time)
    { 5176, { Cond::Always, 0 }, true, 0.0f, 30.0f },       // Wrath
};

// Warrior rotation, in priority order. Spell ids are rank 1; the core casts the highest rank the bot knows.
constexpr Row kWarriorRows[] = {
    { 5308, { Cond::TargetHpBelow, 20 }, true, 0.0f, 5.0f },    // Execute
    { 2687, { Cond::SelfPowerBelow, 20 }, false, 0.0f, 0.0f },  // Bloodrage
    { 100, { Cond::Opener, 8 }, true, 8.0f, 25.0f },            // Charge: opening, from 8 to 25 yards
    { 6343, { Cond::EnemiesAtLeast, 2 }, false, 0.0f, 0.0f },   // Thunder Clap (area, around the bot)
    { 1160, { Cond::EnemiesAtLeast, 3 }, false, 0.0f, 0.0f },   // Demoralizing Shout (area)
    { 7386, { Cond::TargetStrong, 2 }, true, 0.0f, 5.0f },      // Sunder Armor on a strong target
    { 772, { Cond::Always, 0 }, true, 0.0f, 5.0f },             // Rend (no aura check yet: refreshes the bleed)
    { 845, { Cond::EnemiesAtLeast, 2 }, true, 0.0f, 5.0f },     // Cleave
    { 78, { Cond::SelfPowerAbove, 40 }, true, 0.0f, 5.0f },     // Heroic Strike with a rage reserve
};

bool IsLiveHostile(NearbyUnit const& unit)
{
    return unit.is_hostile && unit.health > 0;
}

} // namespace

// Stacks the bot carries of an aura (0 when it has none). An aura with one application reports 0 or 1 stack: both count as one while it runs.
static std::uint32_t AuraStacks(Facts const& f, std::uint32_t spell)
{
    for (std::uint32_t i = 0; i < f.AuraCount && i < 16; ++i)
        if (f.AuraSpell[i] == spell)
            return std::max<std::uint32_t>(f.AuraStacks[i], 1);
    return 0;
}

bool Allowed(Rule const& rule, Facts const& f)
{
    switch (rule.Condition)
    {
        case Cond::Always:         return true;
        case Cond::TargetHpAbove:  return f.TargetHpPct > rule.Param;
        case Cond::TargetHpBelow:  return f.TargetHpPct < rule.Param;
        case Cond::SelfHpBelow:    return f.SelfHpPct < rule.Param;
        case Cond::SelfPowerBelow: return f.SelfPowerPct < rule.Param;
        case Cond::SelfPowerAbove: return f.SelfPowerPct >= rule.Param;
        case Cond::EnemiesAtLeast: return f.EnemiesOnBot >= std::uint32_t(std::max(rule.Param, 1));
        case Cond::TargetCasting:  return f.TargetCasting;
        case Cond::TargetFleeing:  return f.TargetFleeing;
        case Cond::Opener:         return f.FightMs < std::uint64_t(std::max(rule.Param, 0)) * 1000u;
        case Cond::TargetStrong:   return f.TargetElite || f.TargetLevelDiff >= std::max(rule.Param, 1);
        case Cond::ComboPointsAtLeast: return f.ComboPoints >= rule.Param;
        case Cond::EnemiesBelow:   return f.EnemiesOnBot < std::uint32_t(std::max(rule.Param, 1));
        case Cond::InParty:        return f.InParty;
        case Cond::NoPet:          return f.PetState == 0;
        case Cond::SelfAuraBelow:  return AuraStacks(f, std::uint32_t(rule.Param)) < std::uint32_t(std::max(rule.Param2, 1));
        case Cond::SelfAuraAtLeast: return AuraStacks(f, std::uint32_t(rule.Param)) >= std::uint32_t(std::max(rule.Param2, 1));
    }
    return false;
}

namespace {

struct SpecInfo
{
    std::uint32_t ClassId;
    char const* Key;
    std::uint32_t Role; // 1 tank, 2 healer, 3 dps
    bool Default;       // the spec a bot of this class and role plays without one in its roster entry
    std::uint32_t GearProfile; // see GearProfileFor's comment in combat_rotation.h
};

constexpr std::uint32_t kGearStrStam = 0, kGearAgiStam = 1, kGearIntSpiritStam = 2, kGearIntCrit = 3, kGearIntSpiritHaste = 4,
                         kGearStrStamDef = 5, kGearAgiStamDef = 6;

constexpr SpecInfo kSpecs[] = {
    { 3, "bm", 3, true, kGearAgiStam },           { 3, "mm", 3, false, kGearAgiStam },            { 3, "sv", 3, false, kGearAgiStam },
    { 1, "prot", 1, true, kGearStrStamDef },      { 1, "arms", 3, true, kGearStrStam },           { 1, "fury", 3, false, kGearStrStam },
    { 2, "prot", 1, true, kGearStrStamDef },      { 2, "holy", 2, true, kGearIntSpiritHaste },    { 2, "ret", 3, true, kGearStrStam },
    { 5, "holy", 2, true, kGearIntSpiritHaste },  { 5, "disc", 2, false, kGearIntSpiritHaste },   { 5, "shadow", 3, true, kGearIntCrit },
    { 7, "resto", 2, true, kGearIntSpiritHaste }, { 7, "ele", 3, false, kGearIntCrit },           { 7, "enh", 3, true, kGearAgiStam },
    { 8, "arcane", 3, true, kGearIntCrit },       { 8, "fire", 3, false, kGearIntCrit },          { 8, "frost", 3, false, kGearIntCrit },
    { 9, "demonology", 3, true, kGearIntCrit },   { 9, "affliction", 3, false, kGearIntCrit },    { 9, "destruction", 3, false, kGearIntCrit },
    { 4, "combat", 3, true, kGearAgiStam },       { 4, "assassination", 3, false, kGearAgiStam }, { 4, "subtlety", 3, false, kGearAgiStam },
    { 11, "feral_tank", 1, true, kGearAgiStamDef },{ 11, "resto", 2, true, kGearIntSpiritHaste }, { 11, "feral_cat", 3, true, kGearAgiStam },   { 11, "balance", 3, false, kGearIntCrit },
};

} // namespace

std::uint32_t SpecRole(std::uint32_t classId, std::string const& spec)
{
    for (SpecInfo const& info : kSpecs)
        if (info.ClassId == classId && spec == info.Key)
            return info.Role;
    return 0;
}

std::string DefaultSpec(std::uint32_t classId, std::uint32_t role)
{
    for (SpecInfo const& info : kSpecs)
        if (info.ClassId == classId && info.Role == role && info.Default)
            return info.Key;
    return {};
}

std::uint32_t GearProfileFor(std::uint32_t classId, std::string const& spec)
{
    for (SpecInfo const& info : kSpecs)
        if (info.ClassId == classId && spec == info.Key)
            return info.GearProfile;
    return kGearIntSpiritStam; // an unmapped class+spec: the old generic caster bucket, never strength/agility-penalised
}

std::vector<Row> const* RowsFor(RotationData const& data, std::uint32_t classId, std::string const& spec)
{
    if (!spec.empty())
        if (auto const at = data.BySpec.find({ classId, spec }); at != data.BySpec.end())
            return &at->second;
    if (auto const at = data.ByClass.find(classId); at != data.ByClass.end())
        return &at->second;
    return nullptr;
}

std::span<Row const> RowsForClass(std::uint32_t classId)
{
    switch (classId)
    {
        case kWarriorClass: return kWarriorRows;
        case kPaladinClass: return kPaladinRows;
        case kHunterClass: return kHunterRows;
        case kRogueClass: return kRogueRows;
        case kPriestClass: return kPriestRows;
        case kShamanClass: return kShamanRows;
        case kMageClass: return kMageRows;
        case kWarlockClass: return kWarlockRows;
        case kDruidClass: return kDruidRows;
        default: return {};
    }
}

// Trainer levels of the rank 1 rotation spells, read from the baseline trainer_spell rows of the Forever world data. Sunder Armor
// (7386) has no trainer row there, so it is not gated.
std::uint32_t LearnLevel(std::uint32_t spell)
{
    switch (spell)
    {
        case 100: return 4;    // Charge
        case 772: return 4;    // Rend
        case 1978: return 4;   // Serpent Sting
        case 589: return 4;    // Shadow Word: Pain
        case 8042: return 4;   // Earth Shock
        case 172: return 4;    // Corruption
        case 8921: return 4;   // Moonfire
        case 3044: return 6;   // Arcane Shot
        case 883: return 10;   // Call Pet
        case 982: return 10;   // Revive Pet
        case 136: return 12;   // Mend Pet
        case 2136: return 6;   // Fire Blast
        case 6343: return 6;   // Thunder Clap
        case 853: return 8;    // Hammer of Justice
        case 8092: return 10;  // Mind Blast
        case 2054: return 16;  // Heal
        case 8050: return 10;  // Flame Shock
        case 122: return 10;   // Frost Nova
        case 707: return 10;   // Immolate
        case 2687: return 10;  // Bloodrage
        case 1766: return 12;  // Kick
        case 1160: return 14;  // Demoralizing Shout
        case 845: return 20;   // Cleave
        case 5308: return 24;  // Execute
        default: return 0;
    }
}

// Yards a bot approaches its target to: casters and hunters stand off at range, melee classes close to 5 yards.
float ApproachYards(std::uint32_t classId)
{
    switch (classId)
    {
        case kHunterClass:
        case kPriestClass:
        case kShamanClass:
        case kMageClass:
        case kWarlockClass:
        case kDruidClass: return 25.0f;
        default: return 5.0f;
    }
}

float DistanceYards3d(BotSnapshot const& bot, NearbyUnit const& unit)
{
    float const dx = unit.pos_x - bot.pos_x;
    float const dy = unit.pos_y - bot.pos_y;
    float const dz = unit.pos_z - bot.pos_z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float DistanceYards(BotSnapshot const& bot, NearbyUnit const& unit)
{
    float const dx = unit.pos_x - bot.pos_x;
    float const dy = unit.pos_y - bot.pos_y;
    return std::sqrt(dx * dx + dy * dy);
}

Facts FactsFor(BotSnapshot const& bot, std::span<NearbyUnit const> nearby, NearbyUnit const* target, std::uint64_t fightMs)
{
    Facts facts;
    facts.FightMs = fightMs;
    facts.SelfHpPct = bot.max_health ? std::int32_t(std::uint64_t(bot.health) * 100 / bot.max_health) : 100;
    facts.SelfPowerPct = bot.max_power ? std::int32_t(std::uint64_t(bot.power) * 100 / bot.max_power) : 100;

    if (target)
    {
        facts.TargetHpPct = target->max_health ? std::int32_t(std::uint64_t(target->health) * 100 / target->max_health) : 100;
        facts.TargetCasting = target->casting != 0;
        facts.TargetElite = target->elite != 0;
        facts.TargetLevelDiff = std::int32_t(target->level) - std::int32_t(bot.level);
    }

    facts.InParty = bot.group_role != 0;
    facts.PetState = bot.pet_state;
    for (std::uint32_t i = 0; i < bot.aura_count && i < forever_module::kMaxAuras && i < 16; ++i)
    {
        if (bot.auras[i].remaining_ms == 0)
            continue; // a reported aura with 0 left is one that just expired
        facts.AuraSpell[facts.AuraCount] = bot.auras[i].spell_id;
        facts.AuraStacks[facts.AuraCount] = bot.auras[i].stacks;
        ++facts.AuraCount;
    }

    // Hostile units close to the bot: an approximation of the ones attacking it, for area rules.
    for (NearbyUnit const& unit : nearby)
        if (IsLiveHostile(unit) && DistanceYards(bot, unit) <= 8.0f)
            ++facts.EnemiesOnBot;
    if (target && facts.EnemiesOnBot == 0)
        facts.EnemiesOnBot = 1;

    return facts;
}

} // namespace rotation

namespace rotation {

namespace {

// Shared single-token parser (text_number.h): every plain-text data file in the module (rotations,
// grind lists, tamed-beast lists, dungeon events, ground-avoid tables) uses the same Whole/Decimal
// rules, so the implementation lives in one place.
bool Whole(std::string const& text, std::uint32_t& out)
{
    return text_number::ParseWhole(text, out);
}

bool Decimal(std::string const& text, float& out)
{
    return text_number::ParseDecimal(text, out);
}

} // namespace

RotationData ParseRotationData(std::istream& in, std::vector<std::string>& warnings)
{
    RotationData data;
    std::uint32_t classId = 0;
    std::string specKey; // the spec of the current block (empty: the class default)
    std::size_t number = 0;
    std::vector<std::string> t;
    while (text_lines::NextTokens(in, number, t))
    {
        auto const drop = [&](char const* why) { warnings.push_back("rotation file line " + std::to_string(number) + ": " + why + "; ignored"); };

        if (t[0] == "class")
        {
            specKey.clear();
            if ((t.size() == 2 || (t.size() == 4 && t[2] == "spec")) && Whole(t[1], classId) && classId > 0 && classId < 32 && (t.size() == 2 || SpecRole(classId, t[3]) != 0))
            {
                if (t.size() == 4)
                {
                    specKey = t[3];
                    data.BySpec[{ classId, specKey }];
                }
                else
                    data.ByClass[classId];
            }
            else
            {
                classId = 0;
                drop("class needs a class id, and 'spec <key>' a key the class has");
            }
            continue;
        }
        if (classId == 0)
        {
            drop("a record before the first 'class'");
            continue;
        }
        bool const buff = t[0] == "buff";
        if (!buff && t[0] != "row")
        {
            drop("unknown record");
            continue;
        }

        Row row{ 0, Rule{ Cond::Always, 0 }, true, 0.0f, 0.0f };
        row.Buff = buff;
        std::size_t i = 1;
        bool ok = t.size() >= 3 && Whole(t[i++], row.Spell) && row.Spell != 0;
        bool targetGiven = false;
        while (ok && i < t.size())
        {
            std::string const& key = t[i++];
            std::uint32_t n = 0;
            auto const arg = [&](std::uint32_t& out) { return i < t.size() && Whole(t[i++], out); };
            if (key == "level") ok = arg(row.MinLevel);
            else if (key == "maxlevel") ok = arg(row.MaxLevel);
            else if (key == "duration") ok = arg(row.DurationSec);
            else if (key == "self") { row.OnTarget = false; targetGiven = true; }
            else if (key == "target") { row.OnTarget = true; targetGiven = true; }
            else if (key == "party") { row.Party = true; row.OnTarget = false; targetGiven = true; }
            else if (key == "range") ok = i + 1 < t.size() && Decimal(t[i++], row.MinRange) && Decimal(t[i++], row.MaxRange);
            else if (key == "always") row.When = Rule{ Cond::Always, 0 };
            else if (key == "target_casting") row.When = Rule{ Cond::TargetCasting, 0 };
            else if (key == "target_hp_above") { ok = arg(n); row.When = Rule{ Cond::TargetHpAbove, std::int32_t(n) }; }
            else if (key == "target_hp_below") { ok = arg(n); row.When = Rule{ Cond::TargetHpBelow, std::int32_t(n) }; }
            else if (key == "self_hp_below") { ok = arg(n); row.When = Rule{ Cond::SelfHpBelow, std::int32_t(n) }; }
            else if (key == "self_power_below") { ok = arg(n); row.When = Rule{ Cond::SelfPowerBelow, std::int32_t(n) }; }
            else if (key == "self_power_above") { ok = arg(n); row.When = Rule{ Cond::SelfPowerAbove, std::int32_t(n) }; }
            else if (key == "enemies") { ok = arg(n); row.When = Rule{ Cond::EnemiesAtLeast, std::int32_t(n) }; }
            else if (key == "opener") { ok = arg(n); row.When = Rule{ Cond::Opener, std::int32_t(n) }; }
            else if (key == "target_strong") { ok = arg(n); row.When = Rule{ Cond::TargetStrong, std::int32_t(n) }; }
            else if (key == "combo") { ok = arg(n); row.When = Rule{ Cond::ComboPointsAtLeast, std::int32_t(n) }; }
            else if (key == "enemies_below") { ok = arg(n); row.When = Rule{ Cond::EnemiesBelow, std::int32_t(n) }; }
            else if (key == "in_party") row.When = Rule{ Cond::InParty, 0 };
            else if (key == "no_pet") row.When = Rule{ Cond::NoPet, 0 };
            else if (key == "self_aura_below" || key == "self_aura_at_least")
            {
                std::uint32_t spell = 0;
                std::uint32_t stacks = 0;
                ok = arg(spell) && arg(stacks) && spell != 0 && stacks != 0;
                row.When = Rule{ key == "self_aura_below" ? Cond::SelfAuraBelow : Cond::SelfAuraAtLeast, std::int32_t(spell), std::int32_t(stacks) };
            }
            else ok = false;
        }
        if (ok && buff && (!targetGiven || row.OnTarget || row.DurationSec == 0))
            ok = false; // a buff needs 'self' or 'party' and a duration
        if (!ok)
        {
            drop(buff ? "buff needs '<spell> duration <sec> self|party'" : "row needs '<spell> [level n] [duration s] <condition> self|target [range min max]'");
            continue;
        }
        if (!buff && !targetGiven)
            row.OnTarget = true;
        (specKey.empty() ? data.ByClass[classId] : data.BySpec[{ classId, specKey }]).push_back(row);
    }
    return data;
}

RotationData LoadRotationData(std::string const& path, std::vector<std::string>& warnings)
{
    std::ifstream in(path);
    if (!in)
    {
        warnings.push_back("rotation file '" + path + "' cannot be read; the built-in rotations are used");
        return {};
    }
    return ParseRotationData(in, warnings);
}

} // namespace rotation
