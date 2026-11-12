// SPDX-License-Identifier: GPL-2.0-or-later
// Scenario tests, part 4 of 4 (split out of scenario_test.cpp; the helpers are in scenario_test_support.h).
#include "scenario_test_support.h"

namespace {

void Down(BotSnapshot& bot, std::uint8_t life = 1)
{
    bot.life_state = life;
    bot.health = life == 2 ? 1 : 0; // a ghost has health 1
}

void Up(BotSnapshot& bot)
{
    bot.life_state = 0;
    bot.health = 1000;
}

void RevivedPartyOutsideIsPutBackAtTheEntry()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioDirector director = RunningDirector(t, tick, bots);
    std::vector<BotAction> actions;
    // After a wipe the leader and one member are up at the graveyard on map 0; one member is still a ghost there.
    for (BotSnapshot& bot : bots)
        bot.map_id = 0;
    Down(bots[2], 2);
    director.Decide(View(++tick, bots), t += 1000, actions);
    CHECK(Count(actions, ActionType::kTeleport) == 0);                 // not at once: they get up first
    actions.clear();
    director.Decide(View(++tick, bots), t += kReenterDelayMs, actions);
    CHECK(Count(actions, ActionType::kTeleport, 1) == 1 && Count(actions, ActionType::kTeleport, 2) == 1);
    CHECK(Count(actions, ActionType::kTeleport, 3) == 0);              // the ghost is not teleported
    for (BotAction const& action : actions)
        CHECK(action.spell_id == 36 && action.dest_x == -14.0f);
    actions.clear();
    director.Decide(View(++tick, bots), t += 1000, actions);
    CHECK(Count(actions, ActionType::kTeleport) == 0);                 // and not again before the retry time
    bots[0].map_id = 36;
    bots[1].map_id = 36;
    actions.clear();
    director.Decide(View(++tick, bots), t += kEnterRetryMs, actions);
    CHECK(Count(actions, ActionType::kTeleport) == 0);                 // they are back inside
}

// 0.45.3 (gnomer#1, 2026-11-07 16:29): members killed at staging were revived outside after the leader had gone in alone; nothing put them in, so they stood at the
// portal for the rest of the run. With the leader inside, a living member outside is put at the entry (not one in combat, and not a ghost).
void LivingMembersOutsideAreSentInWhileTheLeaderIsInside()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioDirector director = RunningDirector(t, tick, bots);
    CHECK(director.Phase() == ScenarioPhase::Running);
    std::vector<BotAction> actions;
    bots[1].map_id = 0;                  // revived where it fell, outside
    bots[2].map_id = 0;
    Down(bots[2], 2);                    // still a ghost outside
    director.Decide(View(++tick, bots), t += 1000, actions);
    CHECK(Count(actions, ActionType::kTeleport) == 0);                 // not at once
    actions.clear();
    director.Decide(View(++tick, bots), t += kReenterDelayMs, actions);
    CHECK(Count(actions, ActionType::kTeleport, 2) == 1);
    CHECK(Count(actions, ActionType::kTeleport, 1) == 0 && Count(actions, ActionType::kTeleport, 3) == 0); // not the leader inside, not the ghost
    for (BotAction const& action : actions)
        CHECK(action.spell_id == 36 && action.dest_x == -14.0f);
    actions.clear();
    director.Decide(View(++tick, bots), t += 1000, actions);
    CHECK(Count(actions, ActionType::kTeleport) == 0);                 // not again before the retry time
    bots[1].in_combat = 1;                                             // a member in a fight is left to it
    actions.clear();
    director.Decide(View(++tick, bots), t += kEnterRetryMs, actions);
    CHECK(Count(actions, ActionType::kTeleport) == 0);
    bots[1].in_combat = 0;
    actions.clear();
    director.Decide(View(++tick, bots), t += kEnterRetryMs, actions);
    CHECK(Count(actions, ActionType::kTeleport, 2) == 1);              // retried
    bots[1].map_id = 36;
    actions.clear();
    director.Decide(View(++tick, bots), t += kEnterRetryMs, actions);
    CHECK(Count(actions, ActionType::kTeleport) == 0);                 // inside: done
}

void GhostCountsAsNotAlive()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioDirector director = RunningDirector(t, tick, bots);
    std::vector<BotAction> actions;
    for (BotSnapshot& bot : bots)
        Down(bot, 2);
    director.Decide(View(++tick, bots), t += 1000, actions);
    auto const result = director.TakeResult();
    CHECK(result && result->Outcome == "wipe" && result->Alive == 0 && result->Ghosts == 3);
}

void SingleDeathKeepsTheRunGoing()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioDirector director = RunningDirector(t, tick, bots);
    std::vector<BotAction> actions;
    Down(bots[1]);
    director.Decide(View(++tick, bots), t += 1000, actions);
    CHECK(director.Phase() == ScenarioPhase::Running);
}

void DeadLeaderDoesNotFreezeFurthest()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioDirector director = RunningDirector(t, tick, bots);
    std::vector<BotAction> actions;
    Down(bots[0]);
    bots[1].pos_y = 100.0f; // a living member reaches the first point
    director.Decide(View(++tick, bots), t += 1000, actions);
    bots[1].pos_y = 200.0f; // and the last
    director.Decide(View(++tick, bots), t += 1000, actions);
    auto const result = director.TakeResult();
    CHECK(result && result->Outcome == "complete");
}

void DeathEventsCountARepeatDeathTwice()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioDirector director = RunningDirector(t, tick, bots);
    std::vector<BotAction> actions;
    Down(bots[1]);
    director.Decide(View(++tick, bots), t += 1000, actions);
    Up(bots[1]);
    director.Decide(View(++tick, bots), t += 1000, actions);
    Down(bots[1], 2);
    director.Decide(View(++tick, bots), t += 1000, actions);
    Up(bots[1]);
    director.Decide(View(++tick, bots), t += 1000, actions);
    bots[0].pos_y = 200.0f;
    director.Decide(View(++tick, bots), t += 1000, actions);
    auto const result = director.TakeResult();
    CHECK(result && result->DeathEvents == 2 && result->Revived == 2 && result->Deaths == 1); // Deaths: bots that died at least once
}

void RevivedBotIsCountedOnceInDeaths()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioDirector director = RunningDirector(t, tick, bots);
    std::vector<BotAction> actions;
    Down(bots[2]);
    director.Decide(View(++tick, bots), t += 1000, actions);
    Up(bots[2]);
    bots[0].pos_y = 200.0f;
    director.Decide(View(++tick, bots), t += 1000, actions);
    auto const result = director.TakeResult();
    CHECK(result && result->Deaths == 1 && result->Alive == 3);
}

ScenarioPlan RecoveryPlan()
{
    ScenarioPlan plan = Plan();
    plan.WipeRecovery = true;
    plan.WipeRecoveryMs = 60000;
    return plan;
}

void WipeRecoveredByAReclaimingGhost()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioDirector director = RunningDirector(t, tick, bots, RecoveryPlan());
    std::vector<BotAction> actions;
    for (BotSnapshot& bot : bots)
        Down(bot, 2);
    director.Decide(View(++tick, bots), t += 1000, actions);
    CHECK(director.Phase() == ScenarioPhase::Running); // not over yet
    director.Decide(View(++tick, bots), t += 30000, actions);
    CHECK(director.Phase() == ScenarioPhase::Running);
    Up(bots[1]);
    director.Decide(View(++tick, bots), t += 1000, actions);
    Up(bots[0]);
    bots[0].pos_y = 200.0f;
    director.Decide(View(++tick, bots), t += 1000, actions);
    auto const result = director.TakeResult();
    CHECK(result && result->Outcome == "complete" && result->Wipes == 1);
}

void WipeRecoveryTimesOutToWipe()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioDirector director = RunningDirector(t, tick, bots, RecoveryPlan());
    std::vector<BotAction> actions;
    for (BotSnapshot& bot : bots)
        Down(bot, 2);
    director.Decide(View(++tick, bots), t += 1000, actions);
    director.Decide(View(++tick, bots), t += 61000, actions);
    auto const result = director.TakeResult();
    CHECK(result && result->Outcome == "wipe" && result->Wipes == 0);
}

void UnderstrengthEndsTheRun()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioPlan plan = Plan();
    plan.MinAlive = 2;
    plan.DurationMs = 10000000;
    ScenarioDirector director = RunningDirector(t, tick, bots, plan);
    std::vector<BotAction> actions;
    Down(bots[1]);
    Down(bots[2], 2);
    director.Decide(View(++tick, bots), t += 1000, actions);
    CHECK(director.Phase() == ScenarioPhase::Running);
    director.Decide(View(++tick, bots), t += kUnderstrengthGraceMs - 1000, actions);
    CHECK(director.Phase() == ScenarioPhase::Running);
    director.Decide(View(++tick, bots), t += 2000, actions);
    auto const result = director.TakeResult();
    CHECK(result && result->Outcome == "understrength");
}

void DeadLeaderAbandonsAfterTheCap()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioPlan plan = Plan();
    plan.LeaderDeadAbandonMs = 120000;
    plan.DurationMs = 10000000;
    ScenarioDirector director = RunningDirector(t, tick, bots, plan);
    std::vector<BotAction> actions;
    Down(bots[0], 2);
    director.Decide(View(++tick, bots), t += 1000, actions);
    director.Decide(View(++tick, bots), t += 100000, actions);
    CHECK(director.Phase() == ScenarioPhase::Running);
    director.Decide(View(++tick, bots), t += 30000, actions);
    auto const result = director.TakeResult();
    CHECK(result && result->Outcome == "abandoned_dead_leader");
}

void EnteringSkipsGhostsAndCorpses()
{
    ScenarioDirector director(Plan());
    std::vector<BotAction> actions;
    std::vector<BotSnapshot> bots = { Bot(1, 36, 0, 0, 2, 3), Bot(2, 0, 0, 0, 1, 3), Bot(3, 0, 0, 0, 1, 3) };
    Down(bots[1], 2);
    std::uint64_t t = 1000, tick = 1;
    for (int i = 0; i < 20; ++i)
        director.Decide(View(++tick, bots), t += 3000, actions);
    for (BotAction const& action : actions)
        CHECK(!(action.type == ActionType::kTeleport && action.bot_guid == 2));
}

void FormatShowsEventsRevivedWipes()
{
    ScenarioResult result;
    result.DeathEvents = 4;
    result.Revived = 2;
    result.Wipes = 1;
    result.Ghosts = 1;
    std::string const text = FormatScenarioResult(result);
    CHECK(text.find("events=4") != std::string::npos && text.find("revived=2") != std::string::npos);
    CHECK(text.find("wipes=1") != std::string::npos && text.find("ghosts=1") != std::string::npos);
}

// 40-man raid support (doc 108): the result line carries "raid=1 groups=8" for a raid scenario, and nothing extra (dungeon
// result lines unchanged) when the scenario is not a raid.
void FormatShowsRaidAndGroupsOnlyForARaidScenario()
{
    ScenarioResult result;
    result.Outcome = "complete";
    result.Raid = true;
    result.Groups = 8;
    std::string const raidText = FormatScenarioResult(result);
    CHECK(raidText.find("raid=1 groups=8") != std::string::npos);

    ScenarioResult dungeon;
    dungeon.Outcome = "complete";
    std::string const dungeonText = FormatScenarioResult(dungeon);
    CHECK(dungeonText.find("raid=") == std::string::npos && dungeonText.find("groups=") == std::string::npos);
}


// --- Ammunition, class supplies, class quests and the Taming phase ---

std::uint32_t HunterWarlockOrPriest(std::uint64_t guid)
{
    return guid == 1 ? 3u : guid == 2 ? 9u : 5u; // bot 1 is a hunter, bot 2 a warlock, bot 3 a priest
}

void Reach(ScenarioDirector& director, std::vector<BotSnapshot>& bots, std::uint64_t& t, std::uint64_t& tick, std::vector<BotAction>& actions)
{
    auto const step = [&](std::uint64_t advance)
    {
        actions.clear();
        t += advance;
        director.Decide(View(++tick, bots), t, actions);
    };
    step(100);   // the roster is complete
    step(100);   // the dressing: auto gear
}

void AHunterWithoutAmmunitionIsGivenSome()
{
    ScenarioPlan plan = Plan();
    plan.ClassOf = HunterWarlockOrPriest;
    ScenarioDirector director(plan);
    std::vector<BotAction> actions;
    std::uint64_t t = 1000, tick = 1;
    std::vector<BotSnapshot> bots = { Bot(1, 0, 0, 0), Bot(2, 0, 0, 0), Bot(3, 0, 0, 0) };
    bots[0].ammo_entry = 2515;   // the core's recommended arrow for its bow; none carried
    bots[0].ammo_count = 0;
    Reach(director, bots, t, tick, actions);
    int grants = 0;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kGrant && action.bot_guid == 1 && action.spell_id == 2515 && action.dest_x == 200.0f)
            ++grants;
    CHECK(grants == 1);
    // Not again at once (the snapshot has not shown it yet), again after the retry gap, never once it carries enough.
    actions.clear();
    director.Decide(View(++tick, bots), t += 100, actions);
    CHECK(Count(actions, ActionType::kGrant, 1) == 0);
    actions.clear();
    director.Decide(View(++tick, bots), t += kAmmoRetryMs, actions);
    CHECK(Count(actions, ActionType::kGrant, 1) == 1);
    bots[0].ammo_count = 1000;
    actions.clear();
    director.Decide(View(++tick, bots), t += kAmmoRetryMs, actions);
    CHECK(Count(actions, ActionType::kGrant, 1) == 0);
    // A plan with no ammunition count: nothing.
    ScenarioPlan none = plan;
    none.AmmoCount = 0;
    ScenarioDirector quiet(none);
    std::vector<BotSnapshot> hungry = { Bot(1, 0, 0, 0), Bot(2, 0, 0, 0), Bot(3, 0, 0, 0) };
    hungry[0].ammo_entry = 2515;
    std::uint64_t t2 = 1000, tick2 = 1;
    Reach(quiet, hungry, t2, tick2, actions);
    CHECK(Count(actions, ActionType::kGrant, 1) == 0);
    // A bot whose weapon takes no ammunition (ammo_entry 0) is given none.
    ScenarioDirector melee(plan);
    std::vector<BotSnapshot> plain = { Bot(1, 0, 0, 0), Bot(2, 0, 0, 0), Bot(3, 0, 0, 0) };
    std::uint64_t t3 = 1000, tick3 = 1;
    Reach(melee, plain, t3, tick3, actions);
    CHECK(Count(actions, ActionType::kGrant) == 0);
}

void AnAmmunitionGrantIsOneStackAtMostAndStopsWhenItDoesNothing()
{
    // The core refuses a grant above the item's stack size (200 for arrows and shot): 0.33.0 asked for 1000 and 164 grants were refused.
    ScenarioPlan plan = Plan();
    plan.ClassOf = HunterWarlockOrPriest;
    plan.AmmoCount = 1000;
    ScenarioDirector director(plan);
    std::vector<BotAction> actions;
    std::uint64_t t = 1000, tick = 1;
    std::vector<BotSnapshot> bots = { Bot(1, 0, 0, 0), Bot(2, 0, 0, 0), Bot(3, 0, 0, 0) };
    bots[0].ammo_entry = 2516;
    bots[0].ammo_count = 0;
    Reach(director, bots, t, tick, actions);
    bool stack = false;
    for (BotAction const& action : actions)
        stack = stack || (action.type == ActionType::kGrant && action.bot_guid == 1 && action.spell_id == 2516 && action.dest_x == 200.0f);
    CHECK(stack);
    // Refused every time (the count never rises): three grants, then the bot is left alone.
    int grants = 1;
    for (int i = 0; i < 12; ++i)
    {
        actions.clear();
        director.Decide(View(++tick, bots), t += kAmmoRetryMs, actions);
        grants += Count(actions, ActionType::kGrant, 1);
    }
    CHECK(grants >= 3 && grants <= 4);
    // A grant that works keeps the hunter supplied: the count rises, so the next low count is topped up again.
    ScenarioDirector working(plan);
    std::vector<BotSnapshot> good = { Bot(1, 0, 0, 0), Bot(2, 0, 0, 0), Bot(3, 0, 0, 0) };
    good[0].ammo_entry = 2516;
    std::uint64_t t2 = 1000, tick2 = 1;
    Reach(working, good, t2, tick2, actions);
    int given = Count(actions, ActionType::kGrant, 1);
    for (int i = 0; i < 6; ++i)
    {
        good[0].ammo_count = 200;                       // the grant landed
        actions.clear();
        working.Decide(View(++tick2, good), t2 += kAmmoRetryMs, actions);
        given += Count(actions, ActionType::kGrant, 1);
        good[0].ammo_count = 20;                        // and the hunter shot most of it
        actions.clear();
        working.Decide(View(++tick2, good), t2 += kAmmoRetryMs, actions);
        given += Count(actions, ActionType::kGrant, 1);
    }
    CHECK(given >= 6);
}

// --- Supplies (food index 0, drink index 1): the dressing's first grant, then kept topped up like ammunition ---

void ASupplyBelowHalfItsCountIsRegrantedOneAboveItIsNot()
{
    ScenarioPlan plan = Plan();
    plan.Supplies = { { 4540, 10 }, { 1205, 6 } }; // food (Tough Jerky), drink (Ice Cold Milk), by their doc order
    ScenarioDirector director(plan);
    std::vector<BotAction> actions;
    std::uint64_t t = 1000, tick = 1;
    std::vector<BotSnapshot> bots = { Bot(1, 0, 0, 0), Bot(2, 0, 0, 0), Bot(3, 0, 0, 0) };
    Reach(director, bots, t, tick, actions);
    // The dressing's first grant: both items, at their configured count.
    bool food = false, drink = false;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kGrant && action.bot_guid == 1)
        {
            food = food || (action.spell_id == 4540 && action.dest_x == 10.0f);
            drink = drink || (action.spell_id == 1205 && action.dest_x == 6.0f);
        }
    CHECK(food && drink);
    // Plenty of food (10, half of 10 is 5: not low), almost no drink (1, half of 6 is 3: low): only the drink is topped up again.
    bots[0].food_count = 10;
    bots[0].drink_count = 1;
    actions.clear();
    director.Decide(View(++tick, bots), t += kSupplyRetryMs, actions);
    CHECK(GrantCount(actions, 1, 4540) == 0);
    CHECK(GrantCount(actions, 1, 1205) == 1);
    // Flipped: almost no food, plenty of drink: only the food is topped up.
    bots[0].food_count = 2;
    bots[0].drink_count = 6;
    actions.clear();
    director.Decide(View(++tick, bots), t += kSupplyRetryMs, actions);
    CHECK(GrantCount(actions, 1, 4540) == 1);
    CHECK(GrantCount(actions, 1, 1205) == 0);
    // Plenty of both: neither.
    bots[0].food_count = 10;
    bots[0].drink_count = 6;
    actions.clear();
    director.Decide(View(++tick, bots), t += kSupplyRetryMs, actions);
    CHECK(Count(actions, ActionType::kGrant, 1) == 0);
}

void ASupplyGrantThatNeverRaisesItsCountStopsAfterTheUselessCapAndDoesNotAffectTheOtherItem()
{
    // Food never rises (the core keeps refusing it: a full bag of it, say): it stops being retried after kSupplyUselessMax in a row, the
    // same way ammunition does. Drink, granted alongside it on the same bot, rises each time and falls again (as if drunk) and keeps being
    // topped up the whole time: food's cap does not touch it.
    ScenarioPlan plan = Plan();
    plan.Supplies = { { 4540, 10 }, { 1205, 6 } };
    ScenarioDirector director(plan);
    std::vector<BotAction> actions;
    std::uint64_t t = 1000, tick = 1;
    std::vector<BotSnapshot> bots = { Bot(1, 0, 0, 0), Bot(2, 0, 0, 0), Bot(3, 0, 0, 0) };
    Reach(director, bots, t, tick, actions);
    int food = GrantCount(actions, 1, 4540);
    int drink = GrantCount(actions, 1, 1205);
    CHECK(food == 1 && drink == 1);
    for (int i = 0; i < 6; ++i)
    {
        bots[0].drink_count = 6; // the previous grant landed: not low, nothing sent this tick
        actions.clear();
        director.Decide(View(++tick, bots), t += kSupplyRetryMs, actions);
        food += GrantCount(actions, 1, 4540);
        drink += GrantCount(actions, 1, 1205);
        bots[0].drink_count = 1; // and most of it is drunk again: low, topped back up
        actions.clear();
        director.Decide(View(++tick, bots), t += kSupplyRetryMs, actions);
        food += GrantCount(actions, 1, 4540);
        drink += GrantCount(actions, 1, 1205);
    }
    CHECK(food == 3);  // the dressing's grant plus two retries, then the useless cap leaves it alone for the rest of the run
    CHECK(drink == 7); // the dressing's grant plus once per loop, never stopped
}

void ClassSuppliesAndClassQuestsAreSentOnceEach()
{
    ScenarioPlan plan = Plan();
    plan.ClassOf = HunterWarlockOrPriest;
    plan.ClassSupplies[9] = { { 6265, 10 } };          // Soul Shards for the warlock
    plan.ClassQuests[9] = { 1504, 1474 };               // the Voidwalker and Succubus quests
    ScenarioDirector director(plan);
    std::vector<BotAction> actions;
    std::uint64_t t = 1000, tick = 1;
    std::vector<BotSnapshot> bots = { Bot(1, 0, 0, 0), Bot(2, 0, 0, 0), Bot(3, 0, 0, 0) };
    int shards = 0, quest1504 = 0, quest1474 = 0, other = 0;
    for (int i = 0; i < 8; ++i)
    {
        actions.clear();
        director.Decide(View(++tick, bots), t += 100, actions);
        for (BotAction const& action : actions)
        {
            if (action.type == ActionType::kGrant && action.bot_guid == 2 && action.spell_id == 6265 && action.dest_x == 10.0f)
                ++shards;
            else if (action.type == ActionType::kQuest && action.bot_guid == 2 && action.spell_id == ((4u << 24) | 1504u))
                ++quest1504;
            else if (action.type == ActionType::kQuest && action.bot_guid == 2 && action.spell_id == ((4u << 24) | 1474u))
                ++quest1474;
            else if ((action.type == ActionType::kGrant || action.type == ActionType::kQuest) && action.bot_guid != 2)
                ++other;
        }
    }
    CHECK(shards == 1 && quest1504 == 1 && quest1474 == 1 && other == 0);
}

ScenarioPlan TamingPlan()
{
    ScenarioPlan plan = Plan();
    plan.ClassOf = HunterWarlockOrPriest;
    plan.TameEnabled = true;
    plan.TameMap = 0;
    plan.TameSpot = { -10439.0f, 929.0f, 39.0f };
    return plan;
}

void IntoTaming(ScenarioDirector& director, std::vector<BotSnapshot>& bots, std::uint64_t& t, std::uint64_t& tick, std::vector<BotAction>& actions)
{
    for (int i = 0; i < 12 && director.Phase() != ScenarioPhase::Taming && director.Phase() != ScenarioPhase::Grouping; ++i)
    {
        actions.clear();
        director.Decide(View(++tick, bots), t += 3000, actions);
    }
}

void HuntersWithoutAPetAreTakenToTameOneAndBack()
{
    ScenarioDirector director(TamingPlan());
    std::vector<BotAction> actions;
    std::uint64_t t = 1000, tick = 1;
    std::vector<BotSnapshot> bots = { Bot(1, 0, -8913, 554), Bot(2, 0, -8913, 554), Bot(3, 0, -8913, 554) };
    for (BotSnapshot& bot : bots)
        bot.level = 20;
    IntoTaming(director, bots, t, tick, actions);
    CHECK(director.Phase() == ScenarioPhase::Taming && director.Taming());
    // The hunter is sent to the spot, the others stay.
    actions.clear();
    director.Decide(View(++tick, bots), t += 100, actions);
    int away = 0;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kTeleport)
            away += action.bot_guid == 1 && action.spell_id == 0 && action.dest_x == -10439.0f && action.dest_y == 929.0f;
    CHECK(away == 1 && Count(actions, ActionType::kTeleport) == 1);
    // It arrives, spends a while; nothing more is sent, the phase stays.
    bots[0].pos_x = -10439.0f;
    bots[0].pos_y = 929.0f;
    actions.clear();
    director.Decide(View(++tick, bots), t += 30000, actions);
    CHECK(director.Phase() == ScenarioPhase::Taming && Count(actions, ActionType::kTeleport) == 0);
    // It has a pet: back to where it stood, and then the grouping.
    bots[0].pet_state = 1;
    actions.clear();
    director.Decide(View(++tick, bots), t += 1000, actions);
    int back = 0;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kTeleport)
            back += action.bot_guid == 1 && action.dest_x == -8913.0f && action.dest_y == 554.0f;
    CHECK(back == 1);
    bots[0].pos_x = -8913.0f;
    bots[0].pos_y = 554.0f;
    actions.clear();
    director.Decide(View(++tick, bots), t += 1000, actions);
    CHECK(director.Phase() == ScenarioPhase::Grouping);
}

void TheTamingPhaseGivesUpAtTheTimeout()
{
    ScenarioDirector director(TamingPlan());
    std::vector<BotAction> actions;
    std::uint64_t t = 1000, tick = 1;
    std::vector<BotSnapshot> bots = { Bot(1, 0, -8913, 554), Bot(2, 0, -8913, 554), Bot(3, 0, -8913, 554) };
    for (BotSnapshot& bot : bots)
        bot.level = 20;
    IntoTaming(director, bots, t, tick, actions);
    CHECK(director.Phase() == ScenarioPhase::Taming);
    bots[0].pos_x = -10439.0f;                  // there, and never gets a pet
    bots[0].pos_y = 929.0f;
    actions.clear();
    director.Decide(View(++tick, bots), t += kTameTimeoutMs - 5000, actions);
    CHECK(director.Phase() == ScenarioPhase::Taming);
    actions.clear();
    director.Decide(View(++tick, bots), t += 7000, actions);      // the timeout: sent back
    CHECK(Count(actions, ActionType::kTeleport, 1) == 1);
    actions.clear();
    director.Decide(View(++tick, bots), t += kTameBackMs, actions);   // not back yet, but the grouping goes on
    CHECK(director.Phase() == ScenarioPhase::Grouping);
}

void NoTamingWithoutAHunterThatNeedsAPetOrWithoutASpot()
{
    // A hunter that has a pet: straight to the grouping.
    {
        ScenarioDirector director(TamingPlan());
        std::vector<BotAction> actions;
        std::uint64_t t = 1000, tick = 1;
        std::vector<BotSnapshot> bots = { Bot(1, 0, 0, 0), Bot(2, 0, 0, 0), Bot(3, 0, 0, 0) };
        for (BotSnapshot& bot : bots)
            bot.level = 20;
        bots[0].pet_state = 1;
        IntoTaming(director, bots, t, tick, actions);
        CHECK(director.Phase() == ScenarioPhase::Grouping);
    }
    // No tame spot in the plan: a hunter with no pet is not sent anywhere.
    {
        ScenarioPlan plan = TamingPlan();
        plan.TameEnabled = false;
        ScenarioDirector director(plan);
        std::vector<BotAction> actions;
        std::uint64_t t = 1000, tick = 1;
        std::vector<BotSnapshot> bots = { Bot(1, 0, 0, 0), Bot(2, 0, 0, 0), Bot(3, 0, 0, 0) };
        for (BotSnapshot& bot : bots)
            bot.level = 20;
        IntoTaming(director, bots, t, tick, actions);
        CHECK(director.Phase() == ScenarioPhase::Grouping);
    }
    // A hunter below the pet level is not sent either.
    {
        ScenarioDirector director(TamingPlan());
        std::vector<BotAction> actions;
        std::uint64_t t = 1000, tick = 1;
        std::vector<BotSnapshot> bots = { Bot(1, 0, 0, 0), Bot(2, 0, 0, 0), Bot(3, 0, 0, 0) };
        for (BotSnapshot& bot : bots)
            bot.level = 9;
        IntoTaming(director, bots, t, tick, actions);
        CHECK(director.Phase() == ScenarioPhase::Grouping);
    }
}

void SettingsReadRosterSpecsClassSuppliesAndTheTameSpot()
{
    std::unordered_map<std::string, std::string> config = {
        { "Scenario", "dm" }, { "ScenarioRoster", "1:1:prot,1:5,3:3:bm,7:9:demonology" }, { "ScenarioTameSpot", "0 -10439 929 39" }, { "ScenarioEntry", "36 -14.5 -385.4 62.4" },
        { "ScenarioClassSupplies", "9:6265:10,5:17026:5" }, { "ScenarioClassQuests", "9:1504,3:6061" }, { "ScenarioAmmo", "600" },
    };
    std::vector<std::string> warnings;
    BotSettings const settings = ParseBotSettings(config, warnings);
    ScenarioSettings const& s = settings.Scenario;
    CHECK(s.Roster.size() == 4 && s.RosterSpec.size() == 4);
    CHECK(s.RosterSpec[0] == "prot" && s.RosterSpec[1].empty() && s.RosterSpec[2] == "bm" && s.RosterSpec[3] == "demonology");
    CHECK(s.Roster[2][0] == 3 && s.Roster[2][1] == 3 && s.Roster[3][1] == 9);
    CHECK(s.HasTameSpot && s.TameSpot[0] == 0.0f && s.TameSpot[1] == -10439.0f && s.TameSpot[3] == 39.0f);
    CHECK(s.ClassSupplies.size() == 2 && s.ClassSupplies.at(9)[0].first == 6265 && s.ClassSupplies.at(9)[0].second == 10 && s.ClassSupplies.at(5)[0].first == 17026);
    CHECK(s.ClassQuests.size() == 2 && s.ClassQuests.at(9)[0] == 1504 && s.ClassQuests.at(3)[0] == 6061);
    CHECK(s.Ammo == 600);
    CHECK(warnings.empty());

    // A broken spec is dropped with a warning; the defaults give a warlock shards and send the pet quests; a default BotSettings loads data/rotations.txt.
    std::unordered_map<std::string, std::string> broken = { { "Scenario", "dm" }, { "ScenarioRoster", "1:1:Prot!,1:5" }, { "ScenarioEntry", "36 -14.5 -385.4 62.4" } };
    std::vector<std::string> brokenWarnings;
    BotSettings const second = ParseBotSettings(broken, brokenWarnings);
    CHECK(second.Scenario.Roster.size() == 1 && brokenWarnings.size() == 1);
    CHECK(second.Scenario.ClassSupplies.count(9) == 1 && second.Scenario.ClassSupplies.at(9)[0].first == 6265);
    CHECK(second.Scenario.ClassQuests.empty() && second.Scenario.Ammo == 200);   // class quests are opt-in: the core refuses them until it adds the quest first
    CHECK(second.RotationFile == "data/rotations.txt");
    std::unordered_map<std::string, std::string> off = { { "RotationFile", "" } };
    std::vector<std::string> offWarnings;
    CHECK(ParseBotSettings(off, offWarnings).RotationFile.empty());

    ScenarioSlot const slot = ScenarioSlotOf({ s }, 2);
    CHECK(slot.Valid && slot.Class == 3 && slot.Spec == "bm");
    CHECK(ScenarioSlotOf({ s }, 1).Spec.empty());
}

void FormatAppendsTheRosterAfterTheBosses()
{
    ScenarioResult result;
    result.Name = "rfc#2";
    result.Outcome = "timeout";
    result.BossesTotal = 4;
    result.BossesKilled = { "Oggleflint", "Taragaman the Hungerer" };
    result.Party = { { 2, 1 }, { 5, 5 }, { 8, 8 }, { 2, 9 }, { 5, 4 } };
    std::string const text = FormatScenarioResult(result);
    CHECK(text.find("killed=Oggleflint,Taragaman the Hungerer | roster=2:1,5:5,8:8,2:9,5:4 classes=1,4,5,8,9 combo=1-4-5-8-9") != std::string::npos);
    CHECK(text.find(" | ") == text.rfind(" | "));
    result.Party.clear();
    CHECK(FormatScenarioResult(result).find(" | ") == std::string::npos);
}

} // namespace

void RunScenarioTestsPart4()
{
    RevivedPartyOutsideIsPutBackAtTheEntry();
    LivingMembersOutsideAreSentInWhileTheLeaderIsInside();
    GhostCountsAsNotAlive();
    SingleDeathKeepsTheRunGoing();
    DeadLeaderDoesNotFreezeFurthest();
    DeathEventsCountARepeatDeathTwice();
    RevivedBotIsCountedOnceInDeaths();
    WipeRecoveredByAReclaimingGhost();
    WipeRecoveryTimesOutToWipe();
    UnderstrengthEndsTheRun();
    DeadLeaderAbandonsAfterTheCap();
    EnteringSkipsGhostsAndCorpses();
    FormatShowsEventsRevivedWipes();
    FormatShowsRaidAndGroupsOnlyForARaidScenario();
    AHunterWithoutAmmunitionIsGivenSome();
    AnAmmunitionGrantIsOneStackAtMostAndStopsWhenItDoesNothing();
    ASupplyBelowHalfItsCountIsRegrantedOneAboveItIsNot();
    ASupplyGrantThatNeverRaisesItsCountStopsAfterTheUselessCapAndDoesNotAffectTheOtherItem();
    ClassSuppliesAndClassQuestsAreSentOnceEach();
    HuntersWithoutAPetAreTakenToTameOneAndBack();
    TheTamingPhaseGivesUpAtTheTimeout();
    NoTamingWithoutAHunterThatNeedsAPetOrWithoutASpot();
    SettingsReadRosterSpecsClassSuppliesAndTheTameSpot();
    FormatAppendsTheRosterAfterTheBosses();
}
