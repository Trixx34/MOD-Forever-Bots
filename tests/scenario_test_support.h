// SPDX-License-Identifier: GPL-2.0-or-later
// Shared helpers of the scenario tests (split out of scenario_test.cpp); every scenario_test_NN.cpp includes this header.
#pragma once
#include "bot_policy.h"
#include "bot_settings.h"
#include "scenario.h"

#include <cstdio>
#include <string>

using forever_module::ActionType;
using forever_module::BotAction;
using forever_module::BotSnapshot;
using forever_module::WorldTickSnapshotView;

namespace {

// The pull and combat tests were written for bots that start Aggressive; the defensive default has its own test.
inline PolicyOptions AggressiveOptions()
{
    PolicyOptions options;
    options.AggressiveStart = true;
    return options;
}

// One counter for all the part files.
}  // namespace
inline int failures = 0;
namespace {

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

inline std::uint32_t ClassOf(std::uint64_t guid)
{
    return guid == 1 ? 1u : 5u; // bot 1 is the warrior, the rest priests
}

inline WorldTickSnapshotView View(std::uint64_t tick, std::vector<BotSnapshot> const& bots)
{
    WorldTickSnapshotView view;
    view.tick_number = tick;
    view.max_nearby_units = 1;
    view.bots = bots;
    view.nearby.resize(bots.size());
    return view;
}

inline BotSnapshot Bot(std::uint64_t guid, std::uint32_t map, float x, float y, std::uint8_t role = 0, std::uint32_t size = 0)
{
    BotSnapshot bot;
    bot.guid = guid;
    bot.health = 1000;
    bot.max_health = 1000;
    bot.map_id = map;
    bot.pos_x = x;
    bot.pos_y = y;
    bot.group_role = role;
    bot.group_size = size;
    return bot;
}

inline ScenarioPlan Plan()
{
    ScenarioPlan plan;
    plan.Name = "t";
    plan.RosterSize = 3;
    plan.EntryMap = 36;
    plan.Entry = { -14.0f, -385.0f, 62.0f };
    plan.Route = { { 0.0f, 100.0f, 0.0f }, { 0.0f, 200.0f, 0.0f } };
    plan.DurationMs = 100000;
    plan.ClassOf = ClassOf;
    return plan;
}

inline int Count(std::vector<BotAction> const& actions, ActionType type, std::uint64_t bot = 0)
{
    int n = 0;
    for (BotAction const& action : actions)
        n += action.type == type && (bot == 0 || action.bot_guid == bot);
    return n;
}

inline int GrantCount(std::vector<BotAction> const& actions, std::uint64_t bot, std::uint32_t spellId)
{
    int n = 0;
    for (BotAction const& action : actions)
        n += action.type == ActionType::kGrant && action.bot_guid == bot && action.spell_id == spellId;
    return n;
}


inline ScenarioDirector RunningDirector(std::uint64_t& t, std::uint64_t& tick, std::vector<BotSnapshot>& bots, ScenarioPlan plan = Plan())
{
    ScenarioDirector director(plan);
    std::vector<BotAction> actions;
    bots = { Bot(1, 36, 0, 0, 2, 3), Bot(2, 36, 0, 0, 1, 3), Bot(3, 36, 0, 0, 1, 3) };
    t = 1000;
    tick = 1;
    for (int i = 0; i < 4; ++i)
        director.Decide(View(++tick, bots), t += 3000, actions); // waiting -> dressing -> equip
    for (int i = 0; i < 6 && director.Phase() != ScenarioPhase::Running; ++i)
        director.Decide(View(++tick, bots), t += 3000, actions);
    return director;
}

} // namespace

void RunScenarioTestsPart1();
void RunScenarioTestsPart2();
void RunScenarioTestsPart3();
void RunScenarioTestsPart4();
