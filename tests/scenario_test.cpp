// Unit tests for the scenario director, the leader's route and auto gear (doc 19). No core needed.
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

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

std::uint32_t ClassOf(std::uint64_t guid)
{
    return guid == 1 ? 1u : 5u; // bot 1 is the warrior, the rest priests
}

WorldTickSnapshotView View(std::uint64_t tick, std::vector<BotSnapshot> const& bots)
{
    WorldTickSnapshotView view;
    view.tick_number = tick;
    view.max_nearby_units = 1;
    view.bots = bots;
    view.nearby.resize(bots.size());
    return view;
}

BotSnapshot Bot(std::uint64_t guid, std::uint32_t map, float x, float y, std::uint8_t role = 0, std::uint32_t size = 0)
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

ScenarioPlan Plan()
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

int Count(std::vector<BotAction> const& actions, ActionType type, std::uint64_t bot = 0)
{
    int n = 0;
    for (BotAction const& action : actions)
        n += action.type == type && (bot == 0 || action.bot_guid == bot);
    return n;
}

void WaitsForTheWholeRoster()
{
    ScenarioDirector director(Plan());
    std::vector<BotAction> actions;
    director.Decide(View(1, { Bot(1, 0, 0, 0), Bot(2, 0, 0, 0) }), 1000, actions);
    CHECK(director.Phase() == ScenarioPhase::Waiting);
    director.Decide(View(2, { Bot(1, 0, 0, 0), Bot(2, 0, 0, 0), Bot(3, 0, 0, 0) }), 2000, actions);
    CHECK(director.Phase() == ScenarioPhase::Dressing);
}

void DressesThenGroupsThenEnters()
{
    ScenarioDirector director(Plan());
    std::vector<BotAction> actions;
    std::uint64_t t = 1000, tick = 1;
    auto step = [&](std::vector<BotSnapshot> const& bots, std::uint64_t advance = 100)
    {
        actions.clear();
        t += advance;
        director.Decide(View(++tick, bots), t, actions);
    };
    std::vector<BotSnapshot> bots = { Bot(1, 0, 0, 0), Bot(2, 0, 0, 0), Bot(3, 0, 0, 0) };
    step(bots);                                            // roster complete
    CHECK(director.Phase() == ScenarioPhase::Dressing);
    step(bots);                                            // auto gear for everyone, the warrior as a tank
    CHECK(Count(actions, ActionType::kAutoGear) == 3);
    for (BotAction const& action : actions)
        CHECK(action.spell_id == (action.bot_guid == 1 ? 1u : 0u));
    step(bots, kDressPauseMs);                             // the equip service
    CHECK(Count(actions, ActionType::kVendor) == 3);
    step(bots, kEquipPauseMs);
    CHECK(director.Phase() == ScenarioPhase::Grouping);

    // Not grouped yet: nothing happens. Grouped: the leader (role 2) teleports.
    step(bots);
    CHECK(director.Phase() == ScenarioPhase::Grouping);
    bots = { Bot(1, 0, 0, 0, 2, 3), Bot(2, 0, 0, 0, 1, 3), Bot(3, 0, 0, 0, 1, 3) };
    step(bots);
    CHECK(director.Phase() == ScenarioPhase::Entering);
    step(bots);
    CHECK(Count(actions, ActionType::kTeleport, 1) == 1 && Count(actions, ActionType::kTeleport, 2) == 0);

    // The leader arrives: the members follow after the delay.
    bots[0].map_id = 36;
    step(bots);
    CHECK(Count(actions, ActionType::kTeleport) == 0);
    step(bots, kMembersDelayMs);
    CHECK(Count(actions, ActionType::kTeleport, 2) == 1 && Count(actions, ActionType::kTeleport, 3) == 1);

    // Everyone there: running.
    for (BotSnapshot& bot : bots)
        bot.map_id = 36;
    step(bots);
    CHECK(director.Phase() == ScenarioPhase::Running && director.RouteActive());
}

ScenarioDirector RunningDirector(std::uint64_t& t, std::uint64_t& tick, std::vector<BotSnapshot>& bots)
{
    ScenarioDirector director(Plan());
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

void CompletesWhenTheLeaderReachesTheLastPoint()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioDirector director = RunningDirector(t, tick, bots);
    CHECK(director.Phase() == ScenarioPhase::Running);
    std::vector<BotAction> actions;
    bots[0].pos_y = 100.0f;
    director.Decide(View(++tick, bots), t += 1000, actions);
    CHECK(director.Phase() == ScenarioPhase::Running);
    bots[0].pos_y = 200.0f;
    director.Decide(View(++tick, bots), t += 1000, actions);
    auto const result = director.TakeResult();
    CHECK(result && result->Outcome == "complete" && result->Furthest == 2 && result->RouteSize == 2);
    CHECK(!director.TakeResult()); // only once
}

void WipeEndsTheRunAndCountsDeaths()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioDirector director = RunningDirector(t, tick, bots);
    std::vector<BotAction> actions;
    bots[1].health = 0;
    director.Decide(View(++tick, bots), t += 1000, actions);
    CHECK(director.Phase() == ScenarioPhase::Running);
    for (BotSnapshot& bot : bots)
        bot.health = 0;
    director.Decide(View(++tick, bots), t += 1000, actions);
    auto const result = director.TakeResult();
    CHECK(result && result->Outcome == "wipe" && result->Deaths == 3 && result->Alive == 0);
}

void TimeoutEndsTheRun()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioDirector director = RunningDirector(t, tick, bots);
    std::vector<BotAction> actions;
    director.Decide(View(++tick, bots), t += 200000, actions);
    auto const result = director.TakeResult();
    CHECK(result && result->Outcome == "timeout");
    CHECK(FormatScenarioResult(*result).find("outcome=timeout") != std::string::npos);
}

void EnteringTimesOut()
{
    ScenarioDirector director(Plan());
    std::vector<BotAction> actions;
    std::vector<BotSnapshot> bots = { Bot(1, 0, 0, 0, 2, 3), Bot(2, 0, 0, 0, 1, 3), Bot(3, 0, 0, 0, 1, 3) };
    std::uint64_t t = 1000, tick = 1;
    for (int i = 0; i < 6; ++i)
        director.Decide(View(++tick, bots), t += 3000, actions);
    CHECK(director.Phase() == ScenarioPhase::Entering);
    director.Decide(View(++tick, bots), t += kEnterTimeoutMs + 1000, actions);
    auto const result = director.TakeResult();
    CHECK(result && result->Outcome == "enter_failed");
}

// ---- the leader's route (policy) ----

BotPolicy RoutePolicy()
{
    PolicyOptions options;
    options.PartySize = 2;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t guid) { return guid == 12 ? 1u : 5u; };
    options.Route = { { 0.0f, 100.0f, 0.0f }, { 0.0f, 200.0f, 0.0f } };
    return BotPolicy(options);
}

void LeaderWalksTheRouteWhileTheRouteIsActive()
{
    BotPolicy policy = RoutePolicy();
    std::vector<BotSnapshot> bots = { Bot(12, 36, 0, 0, 2, 2), Bot(13, 36, 5, 0, 1, 2) };
    auto const idle = policy.Decide(View(1, bots), 1000);
    CHECK(Count(idle, ActionType::kMoveTo, 12) == 0);

    policy.SetRouteActive(true);
    auto const walking = policy.Decide(View(2, bots), 2000);
    bool toFirst = false;
    for (BotAction const& action : walking)
        if (action.type == ActionType::kMoveTo && action.bot_guid == 12)
            toFirst = action.dest_y == 100.0f;
    CHECK(toFirst);
}

void LeaderWaitsForAMemberWhoFellBehind()
{
    BotPolicy policy = RoutePolicy();
    policy.SetRouteActive(true);
    std::vector<BotSnapshot> bots = { Bot(12, 36, 0, 0, 2, 2), Bot(13, 36, 0, -80, 1, 2) }; // the member is 80 yards back
    auto const actions = policy.Decide(View(1, bots), 1000);
    CHECK(Count(actions, ActionType::kMoveTo, 12) == 0);
}

void LeaderMovesOnAfterTheFirstPoint()
{
    BotPolicy policy = RoutePolicy();
    policy.SetRouteActive(true);
    std::vector<BotSnapshot> bots = { Bot(12, 36, 0, 100, 2, 2), Bot(13, 36, 0, 95, 1, 2) }; // at the first point
    auto const actions = policy.Decide(View(1, bots), 1000);
    bool toSecond = false;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kMoveTo && action.bot_guid == 12)
            toSecond = action.dest_y == 200.0f;
    CHECK(toSecond);
}

// ---- auto gear (policy) ----

void AutoGearingDressesEachBotOnceAndAgainAfterLevels()
{
    PolicyOptions options;
    options.AutoGearing = true;
    options.AutoGearEveryLevels = 5;
    BotPolicy policy(options);
    std::vector<BotSnapshot> bots = { Bot(10, 0, 0, 0) };
    bots[0].level = 3;
    CHECK(Count(policy.Decide(View(1, bots), 1000), ActionType::kAutoGear, 10) == 1);
    CHECK(Count(policy.Decide(View(2, bots), 20000), ActionType::kAutoGear, 10) == 0);
    bots[0].level = 7;
    CHECK(Count(policy.Decide(View(3, bots), 40000), ActionType::kAutoGear, 10) == 0); // 4 levels: not yet
    bots[0].level = 8;
    CHECK(Count(policy.Decide(View(4, bots), 60000), ActionType::kAutoGear, 10) == 1);
}

void WithoutAutoGearingOnlyTheCommandDressesABot()
{
    BotPolicy policy;
    std::vector<BotSnapshot> bots = { Bot(10, 0, 0, 0) };
    CHECK(Count(policy.Decide(View(1, bots), 1000), ActionType::kAutoGear) == 0);
    policy.RequestAutoGear(10);
    CHECK(Count(policy.Decide(View(2, bots), 2000), ActionType::kAutoGear, 10) == 1);
    CHECK(Count(policy.Decide(View(3, bots), 30000), ActionType::kAutoGear, 10) == 0); // once per request
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

int main()
{
    WaitsForTheWholeRoster();
    DressesThenGroupsThenEnters();
    CompletesWhenTheLeaderReachesTheLastPoint();
    WipeEndsTheRunAndCountsDeaths();
    TimeoutEndsTheRun();
    EnteringTimesOut();
    LeaderWalksTheRouteWhileTheRouteIsActive();
    LeaderWaitsForAMemberWhoFellBehind();
    LeaderMovesOnAfterTheFirstPoint();
    AutoGearingDressesEachBotOnceAndAgainAfterLevels();
    WithoutAutoGearingOnlyTheCommandDressesABot();
    SettingsReadAScenario();
    SettingsDropAScenarioWithoutARoster();
    if (failures == 0)
        std::printf("scenario_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
