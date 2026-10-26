// Unit tests for hunter taming: the beast list, the choice of the beast to tame, and the policy's routine (walk up, cast Tame Beast, stand still through the channel, give up).
#include "bot_policy.h"
#include "tame_beasts.h"

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

constexpr char const* kBeasts =
    "# beasts\n"
    "beast 299 1 1 1   # Young Wolf\n"
    "beast 834 1 10 11   # Coyote\n"
    "beast 157 5 14 15   # Goretusk\n"
    "beast 2031 2 1 1   # Young Nightsaber\n"
    "beast 213 1 19 20   # Starving Dire Wolf\n"
    "beast 565 1 20 21   # Rabid Dire Wolf\n"
    "beast 9 x\n"                       // broken
    "monster 5 1 1 1\n";                // unknown record

std::shared_ptr<tame::TameBeasts const> Beasts(std::vector<std::string>* warnings = nullptr)
{
    std::vector<std::string> local;
    std::istringstream in(kBeasts);
    return std::make_shared<tame::TameBeasts>(tame::ParseTameBeasts(in, warnings ? *warnings : local));
}

BotSnapshot Hunter(std::uint32_t level)
{
    BotSnapshot bot;
    bot.guid = 10;
    bot.level = level;
    bot.health = 1000;
    bot.max_health = 1000;
    return bot;
}

NearbyUnit Beast(std::uint32_t entry, std::uint64_t counter, float x, std::uint32_t level, bool hostile = false)
{
    NearbyUnit unit;
    unit.guid = MakeUnitRef(8, entry, counter);
    unit.pos_x = x;
    unit.health = 100;
    unit.max_health = 100;
    unit.level = level;
    unit.is_hostile = hostile;
    return unit;
}

void TheListIsParsed()
{
    std::vector<std::string> warnings;
    auto const beasts = Beasts(&warnings);
    CHECK(beasts->ByEntry.size() == 6);
    CHECK(warnings.size() == 2);
    CHECK(beasts->ByEntry.at(157).Family == 5 && beasts->ByEntry.at(157).LevelMin == 14 && beasts->ByEntry.at(157).LevelMax == 15);
    std::vector<std::string> missing;
    CHECK(tame::LoadTameBeasts("/nonexistent/tame_beasts.txt", missing).ByEntry.empty() && missing.size() == 1);
}

void TheClosestLevelWinsThenNeutralThenFamily()
{
    auto const beasts = Beasts();
    BotSnapshot const bot = Hunter(20);
    std::vector<NearbyUnit> nearby = { Beast(834, 1, 10.0f, 10), Beast(157, 2, 30.0f, 14), Beast(213, 3, 40.0f, 19) };
    NearbyUnit const* pick = tame::PickTameTarget(bot, nearby, *beasts, 60.0f);
    CHECK(pick && pick->guid == nearby[2].guid);                       // the pet takes the beast's level: 19 is the closest to 20, though it is the farthest away

    nearby = { Beast(213, 1, 20.0f, 19, true), Beast(565, 2, 30.0f, 19) };   // the same level: the neutral one over the hostile one
    pick = tame::PickTameTarget(bot, nearby, *beasts, 60.0f);
    CHECK(pick && pick->guid == nearby[1].guid);

    nearby = { Beast(157, 1, 20.0f, 15), Beast(834, 2, 30.0f, 15) };   // the same level, both neutral: a wolf (family 1) over a boar (family 5)
    pick = tame::PickTameTarget(bot, nearby, *beasts, 60.0f);
    CHECK(pick && pick->guid == nearby[1].guid);

    nearby = { Beast(565, 1, 20.0f, 21), Beast(157, 2, 50.0f, 15) };   // above the hunter's level: refused by the core, so not picked
    pick = tame::PickTameTarget(bot, nearby, *beasts, 60.0f);
    CHECK(pick && pick->guid == nearby[1].guid);

    nearby = { Beast(157, 1, 90.0f, 15) };                              // too far
    CHECK(tame::PickTameTarget(bot, nearby, *beasts, 60.0f) == nullptr);
    nearby = { Beast(4242, 1, 10.0f, 5) };                              // not on the list
    CHECK(tame::PickTameTarget(bot, nearby, *beasts, 60.0f) == nullptr);
    nearby = { Beast(157, 1, 10.0f, 15) };
    nearby[0].health = 0;                                               // dead
    CHECK(tame::PickTameTarget(bot, nearby, *beasts, 60.0f) == nullptr);
    nearby = { Beast(157, 1, 10.0f, 0) };                               // no level in the snapshot: the list's highest level for the entry counts
    CHECK(tame::PickTameTarget(Hunter(14), nearby, *beasts, 60.0f) == nullptr && tame::PickTameTarget(Hunter(15), nearby, *beasts, 60.0f) != nullptr);
}

BotPolicy HunterPolicy()
{
    PolicyOptions options;
    options.ClassId = 3;
    options.TameBeasts = Beasts();
    return BotPolicy(options);
}

WorldTickSnapshotView View(BotSnapshot const& bot, std::vector<NearbyUnit> const& units, std::uint64_t tick)
{
    WorldTickSnapshotView view;
    view.tick_number = tick;
    view.max_nearby_units = 4;
    view.bots.push_back(bot);
    view.bots[0].nearby_count = std::uint32_t(units.size());
    view.nearby.resize(4);
    for (std::size_t i = 0; i < units.size(); ++i)
        view.nearby[i] = units[i];
    return view;
}

bool Has(std::vector<BotAction> const& actions, ActionType type, std::uint32_t spell = 0, std::uint64_t target = 0)
{
    for (BotAction const& action : actions)
        if (action.type == type && (spell == 0 || action.spell_id == spell) && (target == 0 || action.target_guid == target))
            return true;
    return false;
}

void AHunterWalksToTheBeastCastsAndHoldsStill()
{
    BotPolicy policy = HunterPolicy();
    NearbyUnit const beast = Beast(157, 5, 40.0f, 15);
    BotSnapshot bot = Hunter(20);
    // Far: it walks toward the beast.
    auto actions = policy.Decide(View(bot, { beast }, 1), 1000);
    CHECK(Has(actions, ActionType::kFollow, 0, beast.guid) && !Has(actions, ActionType::kCastSpell));
    // Close enough, standing: Tame Beast on it.
    bot.pos_x = 25.0f;
    actions = policy.Decide(View(bot, { beast }, 2), 4000);
    CHECK(Has(actions, ActionType::kCastSpell, 1515, beast.guid));
    // The channel runs 20 s: nothing is sent, however the beast moves.
    bot.casting_spell = 1515;
    CHECK(policy.Decide(View(bot, { beast }, 3), 6000).empty());
    CHECK(policy.Decide(View(bot, { beast }, 4), 15000).empty());     // not even a wander: the policy's own movement is held too
    // The core made the pet: the hold ends, and nothing more is cast to tame.
    bot.casting_spell = 0;
    bot.pet_state = 1;
    CHECK(!Has(policy.Decide(View(bot, { beast }, 5), 26000), ActionType::kCastSpell, 1515));
    CHECK(!Has(policy.Decide(View(bot, { beast }, 6), 40000), ActionType::kCastSpell, 1515));
}

void ANoPetAfterTheChannelIsAFailureWithARetryPause()
{
    BotPolicy policy = HunterPolicy();
    NearbyUnit const beast = Beast(157, 5, 10.0f, 15);
    BotSnapshot bot = Hunter(20);
    CHECK(Has(policy.Decide(View(bot, { beast }, 1), 1000), ActionType::kCastSpell, 1515));
    CHECK(policy.Decide(View(bot, { beast }, 2), 10000).empty());                       // still holding
    CHECK(!Has(policy.Decide(View(bot, { beast }, 3), 24000), ActionType::kCastSpell, 1515));   // the hold ended with no pet; the retry pause (20 s) has not
    CHECK(Has(policy.Decide(View(bot, { beast }, 4), 46000), ActionType::kCastSpell, 1515));    // tried again
}

void ARefusedCastEndsTheHoldAtOnce()
{
    BotPolicy policy = HunterPolicy();
    NearbyUnit const beast = Beast(157, 5, 10.0f, 15);
    BotSnapshot bot = Hunter(20);
    CHECK(Has(policy.Decide(View(bot, { beast }, 1), 1000), ActionType::kCastSpell, 1515));
    bot.last_action_type = std::uint8_t(ActionType::kCastSpell);
    bot.last_action_spell = 1515;
    bot.last_action_status = forever_module::BotActionStatus::kRefused;   // Call Pet is not known: the core refuses with "too many pets"
    bot.last_action_sequence = 1;
    auto const actions = policy.Decide(View(bot, { beast }, 2), 4500);
    CHECK(!Has(actions, ActionType::kCastSpell, 1515));
    bot.last_action_sequence = 1;
    CHECK(!Has(policy.Decide(View(bot, { beast }, 3), 9000), ActionType::kCastSpell, 1515));   // not hammering a refused spell
}

void AHunterTamesInsteadOfPullingAHostileMobStandingNearby()
{
    // Westfall: aggressive Fleshrippers around the Goretusks. The hunter's own pull logic used to take the mob first and the hunter fought (0.33.0, 2026-10-26).
    PolicyOptions options;
    options.ClassId = 3;
    options.AggressiveStart = true;
    options.TameBeasts = Beasts();
    BotPolicy policy(options);
    NearbyUnit mob = Beast(4242, 9, 12.0f, 14, true);        // a hostile creature that is not on the list
    NearbyUnit const beast = Beast(157, 5, 10.0f, 15);
    auto const actions = policy.Decide(View(Hunter(20), { mob, beast }, 1), 1000);
    CHECK(Has(actions, ActionType::kCastSpell, 1515, beast.guid));
    CHECK(!Has(actions, ActionType::kAttackTarget, 0, mob.guid) && !Has(actions, ActionType::kFollow, 0, mob.guid));
    // With nothing to tame in view the hunter pulls as before (a city, or a place without beasts).
    BotPolicy plain(options);
    auto const pulls = plain.Decide(View(Hunter(20), { mob }, 1), 1000);
    CHECK(Has(pulls, ActionType::kAttackTarget, 0, mob.guid) || Has(pulls, ActionType::kFollow, 0, mob.guid) || Has(pulls, ActionType::kCastSpell));
}

void ACombatDuringTheChannelDoesNotEndTheHoldUntilTheHunterIsHurt()
{
    BotPolicy policy = HunterPolicy();
    NearbyUnit const beast = Beast(157, 5, 10.0f, 15);
    BotSnapshot bot = Hunter(20);
    CHECK(Has(policy.Decide(View(bot, { beast }, 1), 1000), ActionType::kCastSpell, 1515));
    bot.casting_spell = 1515;
    bot.in_combat = 1;                                     // a Fleshripper bites; the channel runs on
    CHECK(policy.Decide(View(bot, { beast }, 2), 6000).empty());
    bot.health = 400;                                      // under half: the hold ends and the hunter looks after itself
    auto const hurt = policy.Decide(View(bot, { beast }, 3), 9000);
    CHECK(!Has(hurt, ActionType::kCastSpell, 1515));
    bot.health = 1000;
    bot.casting_spell = 0;                                 // not casting any more, still in combat: the hold ended
    CHECK(!Has(policy.Decide(View(bot, { beast }, 4), 12000), ActionType::kCastSpell, 1515));
}

void NoTamingInAPartyBelowLevel10WithAPetOrInCombat()
{
    NearbyUnit const beast = Beast(157, 5, 10.0f, 15);
    {
        BotPolicy policy = HunterPolicy();
        BotSnapshot bot = Hunter(20);
        bot.group_role = 1;
        CHECK(!Has(policy.Decide(View(bot, { beast }, 1), 1000), ActionType::kCastSpell, 1515));   // in a party: it runs the dungeon, it does not wander off
    }
    {
        BotPolicy policy = HunterPolicy();
        CHECK(!Has(policy.Decide(View(Hunter(9), { Beast(299, 5, 10.0f, 1) }, 1), 1000), ActionType::kCastSpell, 1515));   // the pet quest is at level 10
    }
    {
        BotPolicy policy = HunterPolicy();
        BotSnapshot bot = Hunter(20);
        bot.pet_state = 1;
        CHECK(!Has(policy.Decide(View(bot, { beast }, 1), 1000), ActionType::kCastSpell, 1515));   // it has a pet
    }
    {
        BotPolicy policy = HunterPolicy();
        BotSnapshot bot = Hunter(20);
        bot.in_combat = 1;
        CHECK(!Has(policy.Decide(View(bot, { beast }, 1), 1000), ActionType::kCastSpell, 1515));
    }
    {
        PolicyOptions options;     // no list: no taming
        options.ClassId = 3;
        BotPolicy policy(options);
        CHECK(!Has(policy.Decide(View(Hunter(20), { beast }, 1), 1000), ActionType::kCastSpell, 1515));
    }
    {
        PolicyOptions options;     // a warrior never tames
        options.ClassId = 1;
        options.TameBeasts = Beasts();
        BotPolicy policy(options);
        CHECK(!Has(policy.Decide(View(Hunter(20), { beast }, 1), 1000), ActionType::kCastSpell, 1515));
    }
}

} // namespace

int main()
{
    TheListIsParsed();
    TheClosestLevelWinsThenNeutralThenFamily();
    AHunterWalksToTheBeastCastsAndHoldsStill();
    ANoPetAfterTheChannelIsAFailureWithARetryPause();
    ARefusedCastEndsTheHoldAtOnce();
    AHunterTamesInsteadOfPullingAHostileMobStandingNearby();
    ACombatDuringTheChannelDoesNotEndTheHoldUntilTheHunterIsHurt();
    NoTamingInAPartyBelowLevel10WithAPetOrInCombat();
    if (failures == 0)
        std::printf("tame_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
