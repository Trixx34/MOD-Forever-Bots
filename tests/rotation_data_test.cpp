// Unit tests for rotations read from a file: the parser, fight rows with levels and durations, and out-of-combat buffs. No core needed.
#include "bot_policy.h"
#include "combat_rotation.h"

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
    if (failures == 0)
        std::printf("rotation_data_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
