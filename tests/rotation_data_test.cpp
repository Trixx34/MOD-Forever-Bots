// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for rotations read from a file: the parser, fight rows with levels and durations, and out-of-combat buffs. No core needed.
#include "bot_policy.h"
#include "combat_rotation.h"
#include "tame_beasts.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

using forever_module::ActionType;
using forever_module::BotAction;
using forever_module::BotSnapshot;
using forever_module::MakeUnitRef;
using forever_module::NearbyUnit;
using forever_module::WorldTickSnapshotView;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

std::shared_ptr<rotation::RotationData const> Parse(char const* text, std::vector<std::string>& warnings)
{
    std::istringstream in(text);
    return std::make_shared<rotation::RotationData>(rotation::ParseRotationData(in, warnings));
}

void ParsesRowsAndBuffs()
{
    std::vector<std::string> warnings;
    auto const data = Parse(
        "# mage\n"
        "class 8\n"
        "buff 168 level 1 duration 1800 self\n"
        "buff 1459 level 1 duration 1800 party\n"
        "row 122 level 10 enemies 2 self\n"
        "row 2136 level 6 target_hp_below 25 target range 0 30\n"
        "row 133 duration 8 always target range 0 30\n"
        "class 5\n"
        "row 585 always\n"
        "row bogus\n"
        "buff 17 duration 30 target\n"      // a buff on a target is not allowed
        "buff 17 self\n"                    // no duration
        "frobnicate 3\n",
        warnings);
    CHECK(data->ByClass.size() == 2);
    auto const& mage = data->ByClass.at(8);
    CHECK(mage.size() == 5);
    CHECK(mage[0].Buff && !mage[0].OnTarget && !mage[0].Party && mage[0].DurationSec == 1800 && mage[0].Spell == 168);
    CHECK(mage[1].Buff && mage[1].Party);
    CHECK(!mage[2].Buff && !mage[2].OnTarget && mage[2].MinLevel == 10 && mage[2].When.Condition == rotation::Cond::EnemiesAtLeast && mage[2].When.Param == 2);
    CHECK(mage[3].OnTarget && mage[3].MaxRange == 30.0f && mage[3].When.Condition == rotation::Cond::TargetHpBelow);
    CHECK(mage[4].DurationSec == 8 && mage[4].OnTarget);
    CHECK(data->ByClass.at(5).size() == 1 && data->ByClass.at(5)[0].OnTarget); // a fight row with no target word aims at the target
    CHECK(warnings.size() == 4);
}

WorldTickSnapshotView Group(std::uint32_t level)
{
    WorldTickSnapshotView view;
    view.tick_number = 1;
    view.max_nearby_units = 2;
    for (std::uint64_t guid = 10; guid <= 12; ++guid)
    {
        BotSnapshot bot;
        bot.guid = guid;
        bot.health = 1000;
        bot.max_health = 1000;
        bot.level = level;
        bot.group_leader_guid = MakeUnitRef(2, 0, 900);
        view.bots.push_back(bot);
    }
    view.nearby.resize(6);
    return view;
}

std::uint32_t ClassOf(std::uint64_t guid)
{
    return guid == 10 ? 8u : 1u; // bot 10 is the mage
}

bool Casts(std::vector<BotAction> const& actions, std::uint64_t bot, std::uint32_t spell, std::uint64_t target)
{
    for (BotAction const& action : actions)
        if (action.bot_guid == bot && action.type == ActionType::kCastSpell && action.spell_id == spell && action.target_guid == target)
            return true;
    return false;
}

BotPolicy MagePolicy(char const* text)
{
    std::vector<std::string> warnings;
    PolicyOptions options;
    options.ClassOf = ClassOf;
    options.Rotations = Parse(text, warnings);
    return BotPolicy(options);
}

BotPolicy MagePolicyWithCosts(char const* text, char const* costs)
{
    std::vector<std::string> warnings;
    PolicyOptions options;
    options.ClassOf = ClassOf;
    options.Rotations = Parse(text, warnings);
    std::istringstream in(costs);
    options.SpellCosts = std::make_shared<spell_cost::SpellCostData>(spell_cost::ParseSpellCosts(in, warnings));
    return BotPolicy(options);
}

void ABuffTheBotCannotPayForIsNotCast()
{
    // The cost file says Frost Armor costs 60 mana. A mage with 20 does not try it (the core would refuse it); with 60 it does.
    char const* const rotation = "class 8\nbuff 168 duration 1800 self\n";
    BotPolicy policy = MagePolicyWithCosts(rotation, "168\t1\t0\t60\t0\t0\t0\tFrost Armor\n");
    WorldTickSnapshotView view = Group(10);
    view.bots[0].power_type = 0;          // max_power stays 0: with a small maximum the bot would be low on mana and sit down to drink instead
    view.bots[0].power = 20;
    CHECK(!Casts(policy.Decide(view, 1000), 10, 168, 0));
    view.bots[0].power = 60;
    CHECK(Casts(policy.Decide(view, 1100), 10, 168, 0));

    // A spell the file does not price is tried whatever the power is.
    BotPolicy unpriced = MagePolicyWithCosts(rotation, "999\t1\t0\t60\t0\t0\t0\tSomething else\n");
    view.bots[0].power = 0;
    CHECK(Casts(unpriced.Decide(view, 1000), 10, 168, 0));

    // A cost in another power than the bot's main one (a druid in a form) is not checked.
    BotPolicy rage = MagePolicyWithCosts(rotation, "168\t1\t1\t100\t0\t0\t0\tRage cost\n");
    CHECK(Casts(rage.Decide(view, 1000), 10, 168, 0));

    // A rank chain: at level 10 the first rank (30 mana) is what it pays, from level 12 the second (90).
    BotPolicy ranks = MagePolicyWithCosts(rotation, "168\t1\t0\t30\t0\t0\t169\tr1\n169\t12\t0\t90\t0\t0\t0\tr2\n");
    view.bots[0].power = 50;
    CHECK(Casts(ranks.Decide(Group(10), 1000), 10, 168, 0) == false); // Group() has no power: 0 < 30
    WorldTickSnapshotView low = Group(10);
    low.bots[0].power = 50;
    CHECK(Casts(ranks.Decide(low, 1100), 10, 168, 0));
    BotPolicy later = MagePolicyWithCosts(rotation, "168\t1\t0\t30\t0\t0\t169\tr1\n169\t12\t0\t90\t0\t0\t0\tr2\n");
    WorldTickSnapshotView high = Group(15);
    high.bots[0].power = 50;
    CHECK(!Casts(later.Decide(high, 1000), 10, 168, 0));              // 50 < 90
    high.bots[0].power = 90;
    CHECK(Casts(later.Decide(high, 1100), 10, 168, 0));
}

void MaxLevelRetiresASupersededBuff()
{
    BotPolicy policy = MagePolicy("class 8\nbuff 168 level 1 duration 1800 self maxlevel 10   # Frost Armor\nbuff 7302 level 11 duration 1800 self   # Ice Armor\n");
    CHECK(Casts(policy.Decide(Group(5), 1000), 10, 168, 0));
    BotPolicy later = MagePolicy("class 8\nbuff 168 level 1 duration 1800 self maxlevel 10\nbuff 7302 level 11 duration 1800 self\n");
    auto const actions = later.Decide(Group(15), 1000);
    CHECK(!Casts(actions, 10, 168, 0) && Casts(actions, 10, 7302, 0));
}

void ABuffRefusedForPowerWaitsLongBeforeItIsTriedAgain()
{
    // A warrior has no rage out of combat, a caster waits to drink: a buff the core refused for power was tried every two seconds (about 20,000
    // refused casts in 20 minutes on 300 bots). It waits 30 seconds now; a fight spell keeps the short retry.
    BotPolicy policy = MagePolicy("class 8\nbuff 168 duration 1800 self\n");
    WorldTickSnapshotView view = Group(10);
    CHECK(Casts(policy.Decide(view, 1000), 10, 168, 0));
    view.bots[0].last_action_type = std::uint8_t(ActionType::kCastSpell);
    view.bots[0].last_action_spell = 168;
    view.bots[0].last_action_status = forever_module::BotActionStatus::kNotEnoughPower;
    view.bots[0].last_action_sequence = 1;
    policy.Decide(view, 2000);
    CHECK(!Casts(policy.Decide(view, 5000), 10, 168, 0));            // not after the short retry
    CHECK(!Casts(policy.Decide(view, 20000), 10, 168, 0));
    CHECK(Casts(policy.Decide(view, 2000 + 30000 + 100), 10, 168, 0)); // but after the long one

    // Refused for another reason (not ready, the global cooldown): five seconds, not one.
    BotPolicy other = MagePolicy("class 8\nbuff 168 duration 1800 self\n");
    WorldTickSnapshotView view2 = Group(10);
    CHECK(Casts(other.Decide(view2, 1000), 10, 168, 0));
    view2.bots[0].last_action_type = std::uint8_t(ActionType::kCastSpell);
    view2.bots[0].last_action_spell = 168;
    view2.bots[0].last_action_status = forever_module::BotActionStatus::kRefused;
    view2.bots[0].last_action_sequence = 1;
    other.Decide(view2, 2000);
    CHECK(!Casts(other.Decide(view2, 4000), 10, 168, 0));
    CHECK(Casts(other.Decide(view2, 2000 + 5000 + 100), 10, 168, 0));
}

void SelfBuffIsCastOutOfCombatThenHeldForItsDuration()
{
    BotPolicy policy = MagePolicy("class 8\nbuff 168 duration 1800 self\n");
    WorldTickSnapshotView view = Group(10);
    CHECK(Casts(policy.Decide(view, 1000), 10, 168, 0));
    // The core applied it: no recast until it has run out.
    view.bots[0].last_action_type = std::uint8_t(ActionType::kCastSpell);
    view.bots[0].last_action_spell = 168;
    view.bots[0].last_action_status = forever_module::BotActionStatus::kApplied;
    view.bots[0].last_action_sequence = 1;
    policy.Decide(view, 2000);
    CHECK(!Casts(policy.Decide(view, 60000), 10, 168, 0));
    CHECK(Casts(policy.Decide(view, 2000 + 1800 * 1000 + 100), 10, 168, 0));
}

void PartyBuffGoesToEveryMemberOneAfterTheOther()
{
    BotPolicy policy = MagePolicy("class 8\nbuff 1459 duration 1800 party\n");
    WorldTickSnapshotView view = Group(10);
    std::vector<std::uint64_t> seen;
    std::uint64_t now = 1000;
    for (int step = 0; step < 6 && seen.size() < 3; ++step)
    {
        auto const actions = policy.Decide(view, now);
        for (BotAction const& action : actions)
            if (action.bot_guid == 10 && action.type == ActionType::kCastSpell && action.spell_id == 1459)
            {
                seen.push_back(action.target_guid);
                view.bots[0].last_action_type = std::uint8_t(ActionType::kCastSpell);
                view.bots[0].last_action_spell = 1459;
                view.bots[0].last_action_status = forever_module::BotActionStatus::kApplied;
                view.bots[0].last_action_sequence = std::uint32_t(seen.size());
            }
        now += 2000;
    }
    CHECK(seen.size() == 3);
    CHECK(std::find(seen.begin(), seen.end(), 0ull) != seen.end());                          // itself
    CHECK(std::find(seen.begin(), seen.end(), MakeUnitRef(2, 0, 11)) != seen.end());
    CHECK(std::find(seen.begin(), seen.end(), MakeUnitRef(2, 0, 12)) != seen.end());
}

void NoBuffsInCombatOrBelowTheLevel()
{
    BotPolicy policy = MagePolicy("class 8\nbuff 168 level 20 duration 1800 self\n");
    WorldTickSnapshotView view = Group(10);
    CHECK(!Casts(policy.Decide(view, 1000), 10, 168, 0)); // level 10 is below the row's level 20
    view = Group(25);
    view.bots[0].in_combat = 1;
    CHECK(!Casts(policy.Decide(view, 2000), 10, 168, 0)); // in combat
    view.bots[0].in_combat = 0;
    CHECK(Casts(policy.Decide(view, 3000), 10, 168, 0));
}

void FightRowsFollowTheFileAndItsLevels()
{
    BotPolicy policy = MagePolicy("class 8\nrow 116 level 4 always target range 0 30\nrow 5143 level 30 always target range 0 30\n");
    WorldTickSnapshotView view = Group(10);
    NearbyUnit mob;
    mob.guid = MakeUnitRef(8, 7, 3);
    mob.pos_x = 20.0f;
    mob.health = 100;
    mob.is_hostile = true;
    view.nearby[0] = mob;
    view.bots[0].nearby_count = 1;
    view.bots[0].threat_target_guid = mob.guid;
    view.bots[0].in_combat = 1;
    auto actions = policy.Decide(view, 1000);
    CHECK(Casts(actions, 10, 116, mob.guid) && !Casts(actions, 10, 5143, mob.guid)); // the level 30 row waits
    view = Group(35);
    view.nearby[0] = mob;
    view.bots[0].nearby_count = 1;
    view.bots[0].threat_target_guid = mob.guid;
    view.bots[0].in_combat = 1;
    BotPolicy later = MagePolicy("class 8\nrow 5143 level 30 always target range 0 30\nrow 116 level 4 always target range 0 30\n");
    CHECK(Casts(later.Decide(view, 1000), 10, 5143, mob.guid)); // priority order is the file's
}

void ClassesMissingFromTheFileKeepTheBuiltInRows()
{
    BotPolicy policy = MagePolicy("class 5\nrow 585 always target range 0 30\n");
    WorldTickSnapshotView view = Group(10);
    NearbyUnit mob;
    mob.guid = MakeUnitRef(8, 7, 3);
    mob.pos_x = 3.0f;
    mob.health = 100;
    mob.is_hostile = true;
    view.nearby[0] = mob;
    view.bots[0].nearby_count = 1;
    view.bots[0].threat_target_guid = mob.guid;
    view.bots[0].in_combat = 1;
    CHECK(Casts(policy.Decide(view, 1000), 10, 133, mob.guid)); // the mage is not in the file: the built-in Fireball
}


// --- Specs (race:class:spec in the roster; `class N spec KEY` in the file) ---

void SpecBlocksAreParsedAndLookedUp()
{
    std::vector<std::string> warnings;
    auto const data = Parse(
        "class 3\nrow 1130 always target range 0 35\n"
        "class 3 spec bm\nrow 19434 always target range 0 35\n"
        "class 3 spec sv\nrow 14269 always target range 0 5\n"
        "class 3 spec bogus\nrow 1 always target\n"        // a hunter has no such spec: the block and its row are dropped
        "class 5 spec shadow\nrow 589 always target range 0 30\n",
        warnings);
    CHECK(warnings.size() == 2);
    CHECK(data->ByClass.size() == 1 && data->BySpec.size() == 3);
    CHECK(rotation::RowsFor(*data, 3, "bm")->front().Spell == 19434);
    CHECK(rotation::RowsFor(*data, 3, "sv")->front().Spell == 14269);
    CHECK(rotation::RowsFor(*data, 3, "mm")->front().Spell == 1130);   // a spec with no block plays the class default
    CHECK(rotation::RowsFor(*data, 3, "")->front().Spell == 1130);
    CHECK(rotation::RowsFor(*data, 5, "shadow")->front().Spell == 589);
    CHECK(rotation::RowsFor(*data, 5, "holy") == nullptr);             // no default block either: the caller uses the built-in rows
    CHECK(rotation::RowsFor(*data, 8, "arcane") == nullptr);
}

void SpecKeysAndTheirDefaults()
{
    CHECK(rotation::SpecRole(3, "bm") == 3 && rotation::SpecRole(1, "prot") == 1 && rotation::SpecRole(5, "holy") == 2 && rotation::SpecRole(9, "demonology") == 3);
    CHECK(rotation::SpecRole(5, "bm") == 0 && rotation::SpecRole(3, "") == 0 && rotation::SpecRole(11, "feral_tank") == 1 && rotation::SpecRole(11, "balance") == 3);
    CHECK(rotation::DefaultSpec(3, 3) == "bm" && rotation::DefaultSpec(1, 1) == "prot" && rotation::DefaultSpec(1, 3) == "arms");
    CHECK(rotation::DefaultSpec(2, 1) == "prot" && rotation::DefaultSpec(2, 2) == "holy" && rotation::DefaultSpec(2, 3) == "ret");
    CHECK(rotation::DefaultSpec(5, 2) == "holy" && rotation::DefaultSpec(5, 3) == "shadow" && rotation::DefaultSpec(7, 2) == "resto" && rotation::DefaultSpec(7, 3) == "enh");
    CHECK(rotation::DefaultSpec(8, 3) == "arcane" && rotation::DefaultSpec(9, 3) == "demonology" && rotation::DefaultSpec(4, 3) == "combat");
    CHECK(rotation::DefaultSpec(11, 1) == "feral_tank" && rotation::DefaultSpec(11, 2) == "resto" && rotation::DefaultSpec(11, 3) == "feral_cat");
    CHECK(rotation::DefaultSpec(3, 1).empty() && rotation::DefaultSpec(8, 2).empty()); // a hunter does not tank, a mage does not heal
}

void GearProfilesMatchTheirSpec()
{
    // Balance druid: intellect and crit. Restoration: intellect, spirit and haste. Feral dps: stamina and agility
    // (the owner's explicit example, 2026-10-28). Feral tank is its own profile (below), not the same as feral dps.
    CHECK(rotation::GearProfileFor(11, "balance") == 3 && rotation::GearProfileFor(11, "resto") == 4);
    CHECK(rotation::GearProfileFor(11, "feral_cat") == 1);
    // A caster healer (any class) is profile 4; a caster dps (any class) is profile 3; melee/physical is 1 or 0.
    CHECK(rotation::GearProfileFor(2, "holy") == 4 && rotation::GearProfileFor(5, "holy") == 4 && rotation::GearProfileFor(7, "resto") == 4);
    CHECK(rotation::GearProfileFor(5, "shadow") == 3 && rotation::GearProfileFor(7, "ele") == 3 && rotation::GearProfileFor(8, "fire") == 3 && rotation::GearProfileFor(9, "affliction") == 3);
    CHECK(rotation::GearProfileFor(3, "mm") == 1 && rotation::GearProfileFor(4, "combat") == 1 && rotation::GearProfileFor(7, "enh") == 1);
    CHECK(rotation::GearProfileFor(1, "arms") == 0 && rotation::GearProfileFor(2, "ret") == 0);
    // A shield tank (protection warrior/paladin) is profile 5 (dodge/parry/defense/block), not the same profile as its own
    // class's dps spec; a feral tank druid is profile 6 (dodge/defense, no shield so no block/parry), not the same profile
    // as feral dps -- the owner's follow-up, 2026-10-28.
    CHECK(rotation::GearProfileFor(1, "prot") == 5 && rotation::GearProfileFor(2, "prot") == 5);
    CHECK(rotation::GearProfileFor(11, "feral_tank") == 6);
    CHECK(rotation::GearProfileFor(99, "nope") == 2); // an unmapped class+spec: the generic caster bucket, never penalised for strength/agility
}

void TheNewConditionsParseAndDecide()
{
    std::vector<std::string> warnings;
    auto const data = Parse(
        "class 8\n"
        "row 1 self_aura_below 400573 2 target\n"
        "row 2 self_aura_at_least 400588 1 target\n"
        "row 3 in_party self\n"
        "row 4 no_pet self\n"
        "row 5 enemies_below 2 target\n"
        "row 6 self_aura_below 400573 target\n",      // no stack count
        warnings);
    auto const& rows = data->ByClass.at(8);
    CHECK(warnings.size() == 1 && rows.size() == 5);
    CHECK(rows[0].When.Condition == rotation::Cond::SelfAuraBelow && rows[0].When.Param == 400573 && rows[0].When.Param2 == 2);
    CHECK(rows[1].When.Condition == rotation::Cond::SelfAuraAtLeast && rows[1].When.Param == 400588 && rows[1].When.Param2 == 1);
    CHECK(rows[2].When.Condition == rotation::Cond::InParty && rows[3].When.Condition == rotation::Cond::NoPet);
    CHECK(rows[4].When.Condition == rotation::Cond::EnemiesBelow && rows[4].When.Param == 2);

    rotation::Facts facts;
    CHECK(rotation::Allowed(rows[0].When, facts) && !rotation::Allowed(rows[1].When, facts));       // no aura: below 2 stacks, no proc
    facts.AuraCount = 2;
    facts.AuraSpell[0] = 400573; facts.AuraStacks[0] = 1;
    facts.AuraSpell[1] = 400588; facts.AuraStacks[1] = 0;
    CHECK(rotation::Allowed(rows[0].When, facts));                                                  // one stack is below two
    CHECK(rotation::Allowed(rows[1].When, facts));                                                  // an aura with 0 reported stacks counts as one while it runs
    facts.AuraStacks[0] = 2;
    CHECK(!rotation::Allowed(rows[0].When, facts));
    CHECK(!rotation::Allowed(rows[2].When, facts));
    facts.InParty = true;
    CHECK(rotation::Allowed(rows[2].When, facts));
    CHECK(rotation::Allowed(rows[3].When, facts));                                                  // pet_state 0
    facts.PetState = 1;
    CHECK(!rotation::Allowed(rows[3].When, facts));
    facts.PetState = 2;
    CHECK(!rotation::Allowed(rows[3].When, facts));                                                 // a dead pet is not "no pet" (a warlock demon vanishes, a hunter's pet is revived)
    facts.EnemiesOnBot = 1;
    CHECK(rotation::Allowed(rows[4].When, facts));
    facts.EnemiesOnBot = 2;
    CHECK(!rotation::Allowed(rows[4].When, facts));
}

BotPolicy SpecPolicy(char const* text, std::function<std::string(std::uint64_t)> specOf, std::function<std::uint32_t(std::uint64_t)> classOf = ClassOf)
{
    std::vector<std::string> warnings;
    PolicyOptions options;
    options.ClassOf = std::move(classOf);
    options.SpecOf = std::move(specOf);
    options.Rotations = Parse(text, warnings);
    return BotPolicy(options);
}

NearbyUnit HostileMob(float x)
{
    NearbyUnit mob;
    mob.guid = MakeUnitRef(8, 7, 3);
    mob.pos_x = x;
    mob.health = 100;
    mob.max_health = 100;
    mob.is_hostile = true;
    return mob;
}

void AFightView(WorldTickSnapshotView& view, NearbyUnit const& mob)
{
    view.nearby[0] = mob;
    view.bots[0].nearby_count = 1;
    view.bots[0].threat_target_guid = mob.guid;
    view.bots[0].in_combat = 1;
}

void ASpecPlaysItsOwnBlock()
{
    char const* const text = "class 8\nrow 116 always target range 0 30\nclass 8 spec fire\nrow 133 always target range 0 30\n";
    NearbyUnit const mob = HostileMob(20.0f);
    WorldTickSnapshotView view = Group(10);
    AFightView(view, mob);
    BotPolicy fire = SpecPolicy(text, [](std::uint64_t guid) { return guid == 10 ? std::string("fire") : std::string(); });
    CHECK(Casts(fire.Decide(view, 1000), 10, 133, mob.guid));
    // A spec with no block plays the class default; so does a mage with no spec in its roster entry (the default spec, arcane, has no block here).
    BotPolicy frost = SpecPolicy(text, [](std::uint64_t guid) { return guid == 10 ? std::string("frost") : std::string(); });
    WorldTickSnapshotView again = Group(10);
    AFightView(again, mob);
    CHECK(Casts(frost.Decide(again, 1000), 10, 116, mob.guid));
    BotPolicy none = SpecPolicy(text, nullptr);
    WorldTickSnapshotView third = Group(10);
    AFightView(third, mob);
    CHECK(Casts(none.Decide(third, 1000), 10, 116, mob.guid));
}

void TheDefaultSpecOfTheClassPlaysItsBlock()
{
    // No spec in the roster: a mage plays arcane, a warrior in a party (party size set) the protection block.
    char const* const text = "class 8\nrow 116 always target range 0 30\nclass 8 spec arcane\nrow 133 always target range 0 30\n";
    NearbyUnit const mob = HostileMob(20.0f);
    WorldTickSnapshotView view = Group(10);
    AFightView(view, mob);
    BotPolicy policy = SpecPolicy(text, nullptr);
    CHECK(Casts(policy.Decide(view, 1000), 10, 133, mob.guid));
}

void AnArcaneBlastStackRuleAndAMissileProc()
{
    // Arcane (the guide): Arcane Blast below two stacks, Missiles on the proc, Fire Blast to reset at two stacks.
    char const* const text =
        "class 8 spec arcane\n"
        "row 5143 self_aura_at_least 400588 1 target range 0 30\n"
        "row 2136 self_aura_at_least 400573 2 target range 0 20\n"
        "row 400574 self_aura_below 400573 2 target range 0 30\n"
        "row 133 always target range 0 30\n";
    NearbyUnit const mob = HostileMob(15.0f);
    auto const decide = [&](std::uint32_t blastStacks, bool proc)
    {
        BotPolicy policy = SpecPolicy(text, [](std::uint64_t guid) { return guid == 10 ? std::string("arcane") : std::string(); });
        WorldTickSnapshotView view = Group(25);
        AFightView(view, mob);
        view.bots[0].aura_count = 0;
        if (blastStacks != 0)
            view.bots[0].auras[view.bots[0].aura_count++] = forever_module::AuraEntry{ 400573, 8000, blastStacks };
        if (proc)
            view.bots[0].auras[view.bots[0].aura_count++] = forever_module::AuraEntry{ 400588, 8000, 1 };
        return policy.Decide(view, 1000);
    };
    CHECK(Casts(decide(0, false), 10, 400574, mob.guid));          // no stacks: Arcane Blast
    CHECK(Casts(decide(1, false), 10, 400574, mob.guid));          // one stack: Arcane Blast
    CHECK(Casts(decide(2, false), 10, 2136, mob.guid));            // two stacks, no proc: Fire Blast resets them
    CHECK(Casts(decide(2, true), 10, 5143, mob.guid));             // a proc: Arcane Missiles first
    CHECK(Casts(decide(0, true), 10, 5143, mob.guid));
}

std::uint32_t WarlockTen(std::uint64_t guid)
{
    return guid == 10 ? 9u : 1u;
}

void ADemonIsSummonedOnlyWithNoPetAndFallsBack()
{
    char const* const text =
        "class 9\n"
        "buff 712 level 20 duration 3600 self no_pet\n"
        "buff 697 level 10 duration 3600 self no_pet\n"
        "buff 688 level 1 duration 3600 self no_pet\n";
    BotPolicy policy = SpecPolicy(text, nullptr, WarlockTen);
    WorldTickSnapshotView view = Group(25);
    CHECK(Casts(policy.Decide(view, 1000), 10, 712, 0));           // no pet: the Succubus
    view.bots[0].pet_state = 1;
    CHECK(!Casts(policy.Decide(view, 5000), 10, 712, 0) && !Casts(policy.Decide(view, 9000), 10, 697, 0));   // the demon is out: nothing
    view.bots[0].pet_state = 2;
    CHECK(!Casts(policy.Decide(view, 13000), 10, 712, 0));         // dead is not "no pet"
    view.bots[0].pet_state = 0;                                    // the demon is gone: summoned again at once, though the first cast's claim runs for an hour
    CHECK(Casts(policy.Decide(view, 17000), 10, 712, 0));

    // The Succubus is refused (not learned: its quest was not done): the Voidwalker, and the Imp when that is refused as well.
    BotPolicy fallback = SpecPolicy(text, nullptr, WarlockTen);
    WorldTickSnapshotView first = Group(25);
    CHECK(Casts(fallback.Decide(first, 1000), 10, 712, 0));
    WorldTickSnapshotView refused = Group(25);
    refused.bots[0].last_action_type = std::uint8_t(ActionType::kCastSpell);
    refused.bots[0].last_action_spell = 712;
    refused.bots[0].last_action_status = forever_module::BotActionStatus::kNotKnown;
    refused.bots[0].last_action_sequence = 1;
    auto const second = fallback.Decide(refused, 4000);
    CHECK(!Casts(second, 10, 712, 0) && Casts(second, 10, 697, 0));
    WorldTickSnapshotView refusedAgain = Group(25);
    refusedAgain.bots[0].last_action_type = std::uint8_t(ActionType::kCastSpell);
    refusedAgain.bots[0].last_action_spell = 697;
    refusedAgain.bots[0].last_action_status = forever_module::BotActionStatus::kNotKnown;
    refusedAgain.bots[0].last_action_sequence = 2;
    auto const third = fallback.Decide(refusedAgain, 7000);
    CHECK(Casts(third, 10, 688, 0) && !Casts(third, 10, 712, 0) && !Casts(third, 10, 697, 0));

    // Below level 20 there is no Succubus row: the Voidwalker from level 10, the Imp below.
    BotPolicy low = SpecPolicy(text, nullptr, WarlockTen);
    CHECK(Casts(low.Decide(Group(12), 1000), 10, 697, 0));
    BotPolicy lower = SpecPolicy(text, nullptr, WarlockTen);
    CHECK(Casts(lower.Decide(Group(5), 1000), 10, 688, 0));
}

void AStanceIsHeldOnlyInAParty()
{
    char const* const text = "class 1\nbuff 71 level 10 duration 120 self in_party\n";
    BotPolicy policy = SpecPolicy(text, nullptr);
    WorldTickSnapshotView view = Group(20);
    view.bots[1].group_role = 1;   // bot 11, a warrior, is in a party
    auto const actions = policy.Decide(view, 1000);
    CHECK(Casts(actions, 11, 71, 0));
    CHECK(!Casts(actions, 12, 71, 0));                              // bot 12 is alone: it stays in Battle Stance
}

// The shipped files: data/rotations.txt parses without a warning, has the nine classes and 28 specs the spec file names, and the pieces the guides need (the warlock's demon rows, the warrior's stance,
// the hunter's Summon Hawk); data/tame_beasts.txt parses and lists the beasts of the Westfall tame spot.
void TheShippedFilesLoadCleanly()
{
    std::string const root = FOREVER_BOTS_SOURCE_DIR;
    std::vector<std::string> warnings;
    rotation::RotationData const data = rotation::LoadRotationData(root + "/data/rotations.txt", warnings);
    CHECK(warnings.empty());
    CHECK(data.ByClass.size() == 9 && data.BySpec.size() == 28);
    auto const rowsOf = [&](std::uint32_t cls, char const* spec) { return rotation::RowsFor(data, cls, spec); };
    CHECK(rowsOf(3, "bm") && rowsOf(3, "sv") && rowsOf(1, "prot") && rowsOf(9, "demonology") && rowsOf(11, "feral_cat"));
    auto const has = [](std::vector<rotation::Row> const* rows, std::uint32_t spell, bool buff)
    {
        for (rotation::Row const& row : *rows)
            if (row.Spell == spell && row.Buff == buff)
                return true;
        return false;
    };
    CHECK(has(rowsOf(9, "demonology"), 712, true) && has(rowsOf(9, "demonology"), 697, true) && has(rowsOf(9, "demonology"), 688, true));  // Succubus, Voidwalker, Imp
    CHECK(has(rowsOf(9, "affliction"), 697, true) && !has(rowsOf(9, "affliction"), 712, true));                                           // the Voidwalker holds threat
    CHECK(has(rowsOf(1, "prot"), 71, true) && has(rowsOf(1, "prot"), 355, false) && has(rowsOf(1, "prot"), 2565, false) && has(rowsOf(1, "prot"), 6572, false));
    CHECK(has(rowsOf(3, "bm"), 1293241, false) && has(rowsOf(3, "bm"), 19434, false) && has(rowsOf(3, "bm"), 3044, false));
    for (auto const& [key, rows] : data.BySpec)
        for (rotation::Row const& row : rows)
            CHECK(row.Spell != 0 && (!row.Buff || row.DurationSec != 0));
    // A class that is not in the file (6 and 10 are not spawned) has no block, and the policy uses the built-in rows for it.
    CHECK(rotation::RowsFor(data, 6, "") == nullptr);

    std::vector<std::string> tameWarnings;
    tame::TameBeasts const beasts = tame::LoadTameBeasts(root + "/data/tame_beasts.txt", tameWarnings);
    CHECK(tameWarnings.empty() && beasts.ByEntry.size() > 250);
    CHECK(beasts.ByEntry.count(157) == 1 && beasts.ByEntry.count(834) == 1 && beasts.ByEntry.count(213) == 1);   // Goretusk, Coyote, Starving Dire Wolf (Westfall)
}

} // namespace

int main()
{
    ParsesRowsAndBuffs();
    ABuffTheBotCannotPayForIsNotCast();
    MaxLevelRetiresASupersededBuff();
    SelfBuffIsCastOutOfCombatThenHeldForItsDuration();
    ABuffRefusedForPowerWaitsLongBeforeItIsTriedAgain();
    PartyBuffGoesToEveryMemberOneAfterTheOther();
    NoBuffsInCombatOrBelowTheLevel();
    FightRowsFollowTheFileAndItsLevels();
    ClassesMissingFromTheFileKeepTheBuiltInRows();
    SpecBlocksAreParsedAndLookedUp();
    SpecKeysAndTheirDefaults();
    GearProfilesMatchTheirSpec();
    TheNewConditionsParseAndDecide();
    ASpecPlaysItsOwnBlock();
    TheDefaultSpecOfTheClassPlaysItsBlock();
    AnArcaneBlastStackRuleAndAMissileProc();
    ADemonIsSummonedOnlyWithNoPetAndFallsBack();
    AStanceIsHeldOnlyInAParty();
    TheShippedFilesLoadCleanly();
    if (failures == 0)
        std::printf("rotation_data_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
