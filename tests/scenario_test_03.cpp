// SPDX-License-Identifier: GPL-2.0-or-later
// Scenario tests, part 3 of 4 (split out of scenario_test.cpp; the helpers are in scenario_test_support.h).
#include "scenario_test_support.h"

namespace {

WorldTickSnapshotView MageFacingAMob(BotSnapshot bot)
{
    WorldTickSnapshotView view;
    view.tick_number = 1;
    view.max_nearby_units = 2;
    bot.nearby_count = 1;
    bot.threat_target_guid = forever_module::MakeUnitRef(8, 6, 1);
    view.bots.push_back(bot);
    view.nearby.resize(2);
    view.nearby[0].guid = forever_module::MakeUnitRef(8, 6, 1);
    view.nearby[0].pos_x = 20.0f;
    view.nearby[0].health = 100;
    view.nearby[0].max_health = 100;
    view.nearby[0].is_hostile = true;
    return view;
}

BotPolicy MagePolicy()
{
    PolicyOptions options = AggressiveOptions();
    options.ClassId = 8;
    return BotPolicy(options);
}

void NoCastWhileAnotherCastRuns()
{
    BotSnapshot bot = Bot(10, 0, 0, 0);
    bot.power = bot.max_power = 1000;
    BotPolicy policy = MagePolicy();
    CHECK(Count(policy.Decide(MageFacingAMob(bot), 1000), ActionType::kCastSpell, 10) == 1); // standing and idle: it casts
    BotPolicy busy = MagePolicy();
    bot.casting_spell = 133;
    bot.cast_remaining_ms = 1500;
    CHECK(Count(busy.Decide(MageFacingAMob(bot), 1000), ActionType::kCastSpell, 10) == 0);
}

void StopsBeforeASpellWithACastTimeWhileMoving()
{
    BotSnapshot bot = Bot(10, 0, 0, 0);
    bot.power = bot.max_power = 1000;
    bot.moving = 1;
    BotPolicy policy = MagePolicy();
    auto const actions = policy.Decide(MageFacingAMob(bot), 1000);
    CHECK(Count(actions, ActionType::kCastSpell, 10) == 0);
    CHECK(Count(actions, ActionType::kStop, 10) >= 1);
    bot.moving = 0;
    CHECK(Count(policy.Decide(MageFacingAMob(bot), 2600), ActionType::kCastSpell, 10) == 1); // standing: the cast follows
}

void RefusedAsMovingIsLearnedAndRetriedSoon()
{
    BotSnapshot bot = Bot(10, 0, 0, 0);
    bot.power = bot.max_power = 1000;
    BotPolicy policy = MagePolicy();
    auto first = policy.Decide(MageFacingAMob(bot), 1000);
    std::uint32_t spell = 0;
    for (BotAction const& action : first)
        if (action.type == ActionType::kCastSpell)
            spell = action.spell_id;
    CHECK(spell != 0);

    bot.last_action_type = std::uint8_t(ActionType::kCastSpell);
    bot.last_action_status = forever_module::BotActionStatus::kBusy;
    bot.last_action_spell = spell;
    bot.last_action_sequence = 1;
    CHECK(Count(policy.Decide(MageFacingAMob(bot), 1100), ActionType::kCastSpell, 10) == 0); // retry waits 300 ms
    bot.last_action_sequence = 1;
    // The global cooldown (1.5 s) has passed too, and the 300 ms busy retry long ago: the cast is tried again.
    CHECK(Count(policy.Decide(MageFacingAMob(bot), 3000), ActionType::kCastSpell, 10) == 1);
}

// ---- settings ----

void SettingsReadAScenario()
{
    std::unordered_map<std::string, std::string> config = {
        { "Scenario", "dm" },           { "ScenarioRoster", "1:1,1:5,7:8" }, { "ScenarioLevel", "18" },
        { "ScenarioStaging", "0 -8913 554 94" }, { "ScenarioEntry", "36 -14.5 -385.4 62.4" }, { "ScenarioDurationSec", "600" },
        { "ScenarioRoute2", "0 200 0" }, { "ScenarioRoute1", "0 100 0" }, { "ScenarioGear5", "2092, 1234" },
        { "AutoGearing", "1" },
    };
    std::vector<std::string> warnings;
    BotSettings const settings = ParseBotSettings(config, warnings);
    CHECK(settings.AutoGearing);
    ScenarioSettings const& s = settings.Scenario;
    CHECK(s.Name == "dm" && s.Roster.size() == 3 && s.Roster[2][0] == 7 && s.Roster[2][1] == 8);
    CHECK(s.Level == 18 && s.DurationSec == 600 && s.HasStaging && s.HasEntry);
    CHECK(s.Entry[0] == 36.0f && s.Staging[0] == 0.0f);
    CHECK(s.Route.size() == 2 && s.Route[0][1] == 100.0f && s.Route[1][1] == 200.0f); // in the order of n
    CHECK(s.Gear.count(5) == 1);
}

void SettingsDropAScenarioWithoutARoster()
{
    std::unordered_map<std::string, std::string> config = { { "Scenario", "dm" } };
    std::vector<std::string> warnings;
    BotSettings const settings = ParseBotSettings(config, warnings);
    CHECK(settings.Scenario.Name.empty());
    CHECK(!warnings.empty());
}

} // namespace

namespace {


void CountsBossesSeenDead()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioPlan plan = Plan();
    plan.Bosses = { { 644, "Rhahk'Zor" }, { 646, "Mr. Smite" } };
    plan.RaceOf = [](std::uint64_t guid) { return guid == 1 ? 2u : 5u; };
    ScenarioDirector director(plan);
    std::vector<BotAction> actions;
    bots = { Bot(1, 36, 0, 0, 2, 3), Bot(2, 36, 0, 0, 1, 3), Bot(3, 36, 0, 0, 1, 3) };
    t = 1000;
    tick = 1;
    for (int i = 0; i < 12 && director.Phase() != ScenarioPhase::Running; ++i)
        director.Decide(View(++tick, bots), t += 3000, actions);
    CHECK(director.Phase() == ScenarioPhase::Running);

    // Rhahk'Zor's corpse is in bot 2's view (health 0); a living Mr. Smite does not count.
    WorldTickSnapshotView view = View(++tick, bots);
    view.bots[1].nearby_count = 1;
    view.bots[2].nearby_count = 1;
    view.nearby[1].guid = forever_module::MakeUnitRef(8, 644, 5);
    view.nearby[1].health = 0;
    view.nearby[2].guid = forever_module::MakeUnitRef(8, 646, 6);
    view.nearby[2].health = 500;
    director.Decide(view, t += 1000, actions);
    auto const fresh = director.TakeNewBossKills();
    CHECK(fresh.size() == 1 && fresh[0] == "Rhahk'Zor");
    CHECK(director.TakeNewBossKills().empty());

    bots[0].health = bots[1].health = bots[2].health = 0;
    bots[0].life_state = bots[1].life_state = bots[2].life_state = 1; // a wipe ends it
    director.Decide(View(++tick, bots), t += 1000, actions);
    auto const result = director.TakeResult();
    CHECK(result && result->BossesTotal == 2 && result->BossesKilled.size() == 1);
    CHECK(FormatScenarioResult(*result).find("bosses=1/2 killed=Rhahk'Zor") != std::string::npos);
    // boss_ms= (the kill time) sits between the boss list and the roster
    CHECK(FormatScenarioResult(*result).find(" | roster=2:1,5:5,5:5 classes=1,5,5 combo=1-5-5") != std::string::npos);
}

// onyxia_hp=: Onyxia's health at the end, the last time a bot saw her, in the Onyxia raid only (her entry among the bosses); no bot seeing her leaves it out.
void ReportsOnyxiaHealthInTheOnyxiaRaidOnly()
{
    for (bool const onyxiaRaid : { true, false })
    {
        std::uint64_t t, tick;
        std::vector<BotSnapshot> bots;
        ScenarioPlan plan = Plan();
        plan.Bosses = { { onyxiaRaid ? 10184u : 644u, "Onyxia" } };
        ScenarioDirector director(plan);
        std::vector<BotAction> actions;
        bots = { Bot(1, 36, 0, 0, 2, 3), Bot(2, 36, 0, 0, 1, 3), Bot(3, 36, 0, 0, 1, 3) };
        t = 1000;
        tick = 1;
        for (int i = 0; i < 12 && director.Phase() != ScenarioPhase::Running; ++i)
            director.Decide(View(++tick, bots), t += 3000, actions);
        CHECK(director.Phase() == ScenarioPhase::Running);

        // Onyxia at a quarter health in bot 2's view, then at half: the later sighting is the one reported.
        WorldTickSnapshotView view = View(++tick, bots);
        view.bots[1].nearby_count = 1;
        view.nearby[1].guid = forever_module::MakeUnitRef(8, 10184, 5);
        view.nearby[1].health = 250;
        view.nearby[1].max_health = 1000;
        director.Decide(view, t += 1000, actions);
        view.tick_number = ++tick;
        view.nearby[1].health = 500;
        director.Decide(view, t += 1000, actions);

        bots[0].health = bots[1].health = bots[2].health = 0;
        bots[0].life_state = bots[1].life_state = bots[2].life_state = 1; // a wipe ends it
        director.Decide(View(++tick, bots), t += 1000, actions);
        auto const result = director.TakeResult();
        CHECK(result);
        std::string const line = FormatScenarioResult(*result);
        if (onyxiaRaid)
            CHECK(result->OnyxiaHealthPct == 50u && line.find(" onyxia_hp=50") != std::string::npos);
        else
            CHECK(!result->OnyxiaHealthPct && line.find("onyxia_hp") == std::string::npos);
    }
}

// time_s=: each tick of the Running phase goes to one state, fight first, then walk, dead, rest, far, idle.
void CountsWhereTheTimeGoes()
{
    ScenarioDirector director(Plan());
    std::vector<BotAction> actions;
    std::vector<BotSnapshot> bots = { Bot(1, 36, 0, 0, 2, 3), Bot(2, 36, 0, 0, 1, 3), Bot(3, 36, 0, 0, 1, 3) };
    std::uint64_t t = 1000, tick = 1;
    for (int i = 0; i < 12 && director.Phase() != ScenarioPhase::Running; ++i)
        director.Decide(View(++tick, bots), t += 3000, actions);
    CHECK(director.Phase() == ScenarioPhase::Running);
    for (BotSnapshot& bot : bots)
    {
        bot.max_health = 1000;
        bot.health = 1000;
        bot.max_power = 0;
        bot.in_combat = 0;
        bot.moving = 0;
    }
    auto step = [&](std::uint64_t advance)
    {
        actions.clear();
        director.Decide(View(++tick, bots), t += advance, actions);
    };
    step(1000);                                  // the first tick only starts the clock
    bots[1].in_combat = 1;
    step(2000);                                  // fight 2 s
    bots[1].in_combat = 0;
    bots[0].moving = 1;
    step(3000);                                  // walk 3 s
    bots[0].moving = 0;
    bots[2].life_state = 1;
    bots[2].health = 0;
    step(4000);                                  // dead 4 s
    bots[2].life_state = 0;
    bots[2].health = 1000;
    bots[1].health = 500;
    step(5000);                                  // rest 5 s
    bots[1].health = 1000;
    bots[1].pos_x = bots[0].pos_x + 100.0f;
    step(4000);                                  // far 4 s
    bots[1].pos_x = bots[0].pos_x;
    step(5000);                                  // idle 5 s (a step over kTimeStateMaxGapMs, 5 s, is not counted)
    step(60000);                                 // a gap over kTimeStateMaxGapMs is not counted
    bots[0].health = bots[1].health = bots[2].health = 0;
    bots[0].life_state = bots[1].life_state = bots[2].life_state = 1;
    step(1000);                                  // a wipe ends the run; this last second counts as dead
    auto const result = director.TakeResult();
    CHECK(result.has_value());
    if (result)
    {
        std::printf("%s\n", FormatScenarioResult(*result).c_str());
        CHECK(FormatScenarioResult(*result).find(" time_s=fight:2,walk:3,dead:5,rest:5,far:4,idle:5") != std::string::npos);
    }
}

} // namespace

void RunScenarioTestsPart3()
{
    NoCastWhileAnotherCastRuns();
    StopsBeforeASpellWithACastTimeWhileMoving();
    RefusedAsMovingIsLearnedAndRetriedSoon();
    SettingsReadAScenario();
    SettingsDropAScenarioWithoutARoster();
    CountsBossesSeenDead();
    ReportsOnyxiaHealthInTheOnyxiaRaidOnly();
    CountsWhereTheTimeGoes();
}
