// SPDX-License-Identifier: GPL-2.0-or-later
// Dungeon clear reliability (doc/dungeons.md): the scenario director's config guard (ScenarioPlan::VerifyConfig) and the boss kill times in the result line.
// Written against scenario.h as it reads now; built and run by Jenkins (no local builds).
#include "scenario.h"

#include <cstdio>
#include <string>
#include <vector>

using forever_module::ActionType;
using forever_module::BotAction;
using forever_module::BotActionStatus;
using forever_module::BotSnapshot;
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

WorldTickSnapshotView View(std::uint64_t tick, std::vector<BotSnapshot> const& bots)
{
    WorldTickSnapshotView view;
    view.tick_number = tick;
    view.max_nearby_units = 1;
    view.bots = bots;
    view.nearby.resize(bots.size());
    return view;
}

BotSnapshot Bot(std::uint64_t guid, std::uint32_t map, float x, float y)
{
    BotSnapshot bot;
    bot.guid = guid;
    bot.health = 1000;
    bot.max_health = 1000;
    bot.map_id = map;
    bot.pos_x = x;
    bot.pos_y = y;
    return bot;
}

ScenarioPlan GuardPlan(bool verify)
{
    ScenarioPlan plan;
    plan.Name = "guard";
    plan.RosterSize = 3;
    plan.EntryMap = 36;
    plan.Entry = { -14.0f, -385.0f, 62.0f };
    plan.VerifyConfig = verify;
    return plan;
}

std::vector<BotSnapshot> Party()
{
    return { Bot(1, 0, 10.0f, 10.0f), Bot(2, 0, 12.0f, 10.0f), Bot(3, 0, 14.0f, 10.0f) };
}

// One tick at `seconds` with the bots as they are; returns the actions the director sent.
std::vector<BotAction> Tick(ScenarioDirector& director, std::uint64_t seconds, std::vector<BotSnapshot> const& bots)
{
    std::vector<BotAction> actions;
    director.Decide(View(seconds, bots), seconds * 1000, actions);
    return actions;
}

// The core's answer to a bot's last action: a new sequence, with its type and status.
void Answer(BotSnapshot& bot, ActionType type, BotActionStatus status)
{
    ++bot.last_action_sequence;
    bot.last_action_type = static_cast<std::uint8_t>(type);
    bot.last_action_status = status;
}

std::size_t Count(std::vector<BotAction> const& actions, ActionType type)
{
    std::size_t n = 0;
    for (BotAction const& action : actions)
        if (action.type == type)
            ++n;
    return n;
}

std::string OutcomeOf(ScenarioDirector& director)
{
    auto const result = director.TakeResult();
    return result ? result->Outcome : std::string("(none)");
}

// Three refused teleport probes in a row end the run as config_refused; each refusal is probed again at once.
void RefusedProbesEndRun()
{
    ScenarioDirector director(GuardPlan(true));
    std::vector<BotSnapshot> bots = Party();
    Tick(director, 1, bots);                       // Waiting -> Dressing
    CHECK(Count(Tick(director, 2, bots), ActionType::kTeleport) == 1); // the probe
    for (std::uint64_t t = 3; t <= 5; ++t)
    {
        Answer(bots[0], ActionType::kTeleport, BotActionStatus::kRefused);
        Tick(director, t, bots);
    }
    CHECK(director.Phase() == ScenarioPhase::Done);
    CHECK(OutcomeOf(director) == "config_refused");
}

// An applied probe lets the dressing finish: the party gets to Grouping.
void AppliedProbeLetsDressingEnd()
{
    ScenarioDirector director(GuardPlan(true));
    std::vector<BotSnapshot> bots = Party();
    Tick(director, 1, bots);
    Tick(director, 2, bots);
    Answer(bots[0], ActionType::kTeleport, BotActionStatus::kApplied);
    for (std::uint64_t t = 3; t <= 20; ++t)
        Tick(director, t, bots);
    CHECK(director.Phase() == ScenarioPhase::Grouping);
}

// No answer to the probe within kConfigProbeTimeoutMs (60 s): config_unknown, and the dressing has not ended before that.
void UnansweredProbeTimesOut()
{
    ScenarioDirector director(GuardPlan(true));
    std::vector<BotSnapshot> bots = Party();
    Tick(director, 1, bots);
    for (std::uint64_t t = 2; t <= 20; ++t)
        Tick(director, t, bots);
    CHECK(director.Phase() == ScenarioPhase::Dressing);
    for (std::uint64_t t = 21; t <= 65; ++t)
        Tick(director, t, bots);
    CHECK(director.Phase() == ScenarioPhase::Done);
    CHECK(OutcomeOf(director) == "config_unknown");
}

// Grants refused for two bots with none applied: config_refused, before any probe is answered.
void RefusedGrantsEndRun()
{
    ScenarioDirector director(GuardPlan(true));
    std::vector<BotSnapshot> bots = Party();
    Tick(director, 1, bots);
    Tick(director, 2, bots);                          // baseline of every bot's sequence, and the probe
    Answer(bots[1], ActionType::kGrant, BotActionStatus::kRefused);
    Answer(bots[2], ActionType::kGrant, BotActionStatus::kRefused);
    Tick(director, 3, bots);
    CHECK(director.Phase() == ScenarioPhase::Done);
    CHECK(OutcomeOf(director) == "config_refused");
}

// The guard is off by default: no probe, and the run is not ended by it.
void GuardOffSendsNoProbe()
{
    ScenarioDirector director(GuardPlan(false));
    std::vector<BotSnapshot> bots = Party();
    Tick(director, 1, bots);
    std::size_t teleports = 0;
    for (std::uint64_t t = 2; t <= 60; ++t)
        teleports += Count(Tick(director, t, bots), ActionType::kTeleport);
    CHECK(teleports == 0);
    CHECK(OutcomeOf(director) == "(none)");
}

// The boss kill times are printed after the killed= list, only when there are some.
void BossKillTimesInResultLine()
{
    ScenarioResult result;
    result.Name = "dm";
    result.Outcome = "bosses_missing";
    result.RouteSize = 3;
    result.RosterSize = 5;
    result.BossesTotal = 2;
    result.BossesKilled = { "Edwin VanCleef", "Cookie" };
    result.BossKillMs = { { "Edwin VanCleef", 612000 }, { "Cookie", 655500 } };
    std::string const line = FormatScenarioResult(result);
    CHECK(line.find(" killed=Edwin VanCleef,Cookie boss_ms=Edwin VanCleef:612000,Cookie:655500") != std::string::npos);

    ScenarioResult none = result;
    none.BossKillMs.clear();
    CHECK(FormatScenarioResult(none).find("boss_ms") == std::string::npos);
}

} // namespace

int main()
{
    RefusedProbesEndRun();
    AppliedProbeLetsDressingEnd();
    UnansweredProbeTimesOut();
    RefusedGrantsEndRun();
    GuardOffSendsNoProbe();
    BossKillTimesInResultLine();
    if (failures == 0)
    {
        std::printf("dungeon_clear_test: all checks pass\n");
        return 0;
    }
    std::printf("dungeon_clear_test: %d failure(s)\n", failures);
    return 1;
}
