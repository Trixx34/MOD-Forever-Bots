// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the dungeon data file (dungeon_data.h) and the dungeon-clear rule of the scenario director (RequireBosses). No core needed.
#include "bot_settings.h"
#include "dungeon_data.h"
#include "scenario.h"

#include <cstdio>
#include <fstream>
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

char const* kFile = R"(# a sample
dungeon rfc
name Ragefire Chasm
levels 13 18
roster 2:1,5:5,8:8,2:9,5:4
staging 1 1629 -4373 31
portal 1 1811 -4410 -18
entry 389 3.81 -14.82 -17.84
duration 1500
wipe_recovery 300
min_alive 2
leader_dead_abandon 600
go 0 100 0
boss 11517 0 200 0 Oggleflint, the Big   # a comma in a name is removed
boss 11519 0 300 0 Bazzalan
end

dungeon broken
roster 1:1
entry 36 1 2 3
go 1 2
end

dungeon noentry
roster 1:1
go 1 2 3
end

dungeon badroster
roster 1-1
entry 36 1 2 3
go 1 2 3
end
)";

void ParsesADungeon()
{
    std::vector<std::string> warnings;
    auto const dungeons = ParseDungeons(kFile, warnings);
    CHECK(dungeons.size() == 1); // the three bad blocks are dropped, with warnings
    CHECK(warnings.size() >= 3);
    Dungeon const& d = dungeons[0];
    CHECK(d.Id == "rfc" && d.Name == "Ragefire Chasm" && d.MinLevel == 13 && d.MaxLevel == 18);
    CHECK(d.Roster == "2:1,5:5,8:8,2:9,5:4" && d.Staging && d.Portal && d.Entry[0] == 389.0f);
    CHECK(d.DurationSec == 1500 && d.WipeRecoverySec == 300 && d.MinAlive == 2 && d.LeaderDeadAbandonSec == 600);
    CHECK(d.Route.size() == 3 && d.Route[0].BossEntry == 0 && d.Route[1].BossEntry == 11517 && d.Route[1].BossName == "Oggleflint  the Big");
}

void UnknownKeysAndMissingEndAreReported()
{
    std::vector<std::string> warnings;
    auto const dungeons = ParseDungeons("dungeon a\nfoo 1\nroster 1:1\nentry 1 2 3 4\ngo 1 2 3", warnings);
    CHECK(dungeons.empty()); // no 'end'
    bool unknown = false, noEnd = false;
    for (std::string const& w : warnings)
    {
        unknown = unknown || w.find("unknown key 'foo'") != std::string::npos;
        noEnd = noEnd || w.find("no 'end'") != std::string::npos;
    }
    CHECK(unknown && noEnd);
}

void MergeFillsTheScenarioKeys()
{
    std::vector<std::string> warnings;
    auto const merged = MergeDungeon({ { "Dungeon", "rfc" } }, kFile, warnings);
    BotSettings const s = ParseBotSettings(merged, warnings);
    ScenarioSettings const& sc = s.Scenario;
    CHECK(sc.Name == "rfc" && sc.Roster.size() == 5 && sc.Level == 18 && sc.HasEntry && sc.Entry[0] == 389.0f);
    CHECK(sc.HasStaging && sc.HasPortal && sc.Portal[0] == 1.0f && sc.DurationSec == 1500);
    CHECK(sc.WipeRecovery && sc.WipeRecoverySec == 300 && sc.MinAlive == 2 && sc.LeaderDeadAbandonSec == 600);
    CHECK(sc.Route.size() == 3 && sc.Route[2][1] == 300.0f);
    CHECK(sc.Bosses.size() == 2 && sc.Bosses[0].first == 11517 && sc.Bosses[1].second == "Bazzalan" && sc.RequireBosses);
    // The bosses' route points (from 1 in the key, from 0 in the settings), so the leader holds at them.
    CHECK(sc.BossAt.size() == 2 && sc.BossAt[0].second == 11517 && sc.BossAt[1].second == 11519 && sc.BossAt[0].first < sc.BossAt[1].first && sc.BossAt[1].first < sc.Route.size());
    bool badWarning = false;
    for (std::string const& w : warnings)
        badWarning = badWarning || w.find("Module.forever-bots") != std::string::npos;
    CHECK(!badWarning); // the merged keys all parse cleanly
}

void ExplicitConfigWins()
{
    std::vector<std::string> warnings;
    auto const merged = MergeDungeon({ { "Dungeon", "rfc" }, { "ScenarioLevel", "15" }, { "ScenarioRoster", "1:1,1:5" }, { "ScenarioRoute1", "7 8 9" },
                                       { "ScenarioDurationSec", "60" } },
                                     kFile, warnings);
    BotSettings const s = ParseBotSettings(merged, warnings);
    CHECK(s.Scenario.Level == 15 && s.Scenario.Roster.size() == 2 && s.Scenario.DurationSec == 60);
    CHECK(s.Scenario.Route.size() == 1 && s.Scenario.Route[0][0] == 7.0f); // an own route is not mixed with the file's
    CHECK(s.Scenario.Bosses.size() == 2);                                  // the rest still comes from the file
}

char const* kTwo = R"(dungeon rfc
levels 13 18
roster 2:1,5:5,8:8,2:9,5:4
staging 1 1629 -4373 31
portal 1 1811 -4410 -18
entry 389 3.81 -14.82 -17.84
go 0 100 0
boss 11517 0 200 0 Oggleflint
end

dungeon dm
levels 18 23
roster 2:1,5:5,8:8,2:9,5:4
staging 1 1629 -4373 31
portal 0 -11208.7 1675.9 24.6
entry 36 -14.57 -385.48 62.46
duration 1800
go 1 1 1
go 2 2 2
boss 644 3 3 3 Rhahk'Zor
end
)";

// Scenario = "dm:10,rfc:10": each listed dungeon is filled from its own block, under "<key>.<id>"; the dungeon's own overrides and the shared keys win.
void MergeFillsEachListedDungeon()
{
    std::vector<std::string> warnings;
    auto const merged = MergeDungeon({ { "Scenario", "dm:10,rfc:10" }, { "ScenarioRoster.dm", "1:1,1:5,1:8,3:3,7:9" }, { "ScenarioLevel.dm", "18" },
                                       { "ScenarioStaging.dm", "0 -8913 554 94" }, { "ScenarioLevel.rfc", "17" }, { "ScenarioDurationSec", "3600" },
                                       { "Dungeon", "rfc" }, { "ScenarioRoster", "2:1,5:5,8:8,2:9,5:4" } },
                                     kTwo, warnings);
    CHECK(merged.count("ScenarioEntry.dm") == 1 && merged.count("ScenarioRoute3.dm") == 1 && merged.count("ScenarioRoute2.rfc") == 1);
    CHECK(merged.at("ScenarioRoster.dm") == "1:1,1:5,1:8,3:3,7:9");     // the override is kept
    CHECK(merged.count("ScenarioDurationSec.dm") == 0);                   // the shared key wins over the file's duration
    CHECK(merged.count("ScenarioEntry") == 0);                            // nothing plain is added in the list form
    BotSettings const s = ParseBotSettings(merged, warnings);
    CHECK(s.Scenarios.size() == 2);
    ScenarioSettings const& dm = s.Scenarios[0];
    ScenarioSettings const& rfc = s.Scenarios[1];
    CHECK(dm.Name == "dm" && dm.Parties == 10 && dm.Level == 18 && dm.Roster[0][0] == 1 && dm.Entry[0] == 36.0f && dm.Staging[0] == 0.0f);
    CHECK(dm.HasPortal && dm.Portal[0] == 0.0f && dm.Route.size() == 3 && dm.Bosses.size() == 1 && dm.Bosses[0].first == 644 && dm.BossAt[0].first == 2);
    CHECK(rfc.Name == "rfc" && rfc.Parties == 10 && rfc.Level == 17 && rfc.Roster[0][0] == 2 && rfc.Entry[0] == 389.0f && rfc.Staging[0] == 1.0f);
    CHECK(rfc.Route.size() == 2 && rfc.Bosses.size() == 1 && rfc.Bosses[0].first == 11517 && rfc.HasPortal && rfc.Portal[0] == 1.0f);
    CHECK(dm.DurationSec == 3600 && rfc.DurationSec == 3600 && dm.RequireBosses && rfc.RequireBosses);

    // A listed dungeon the file does not have is left out; the others still run.
    warnings.clear();
    BotSettings const partly = ParseBotSettings(MergeDungeon({ { "Scenario", "nope:5,rfc:2" } }, kTwo, warnings), warnings);
    CHECK(partly.Scenarios.size() == 1 && partly.Scenarios[0].Name == "rfc" && partly.Scenarios[0].Parties == 2);
    bool missing = false;
    for (std::string const& w : warnings)
        missing = missing || w.find("'nope' is not in the dungeon file") != std::string::npos;
    CHECK(missing);
}

// The sim's single-dungeon config through the file: one dungeon, as before the list form.
void SingleDungeonMergeIsUnchanged()
{
    std::vector<std::string> warnings;
    auto const merged = MergeDungeon({ { "Scenario", "rfc" }, { "Dungeon", "rfc" }, { "ScenarioParties", "20" }, { "ScenarioLevel", "17" },
                                       { "ScenarioRoster", "2:1,5:5,8:8,2:9,5:4" }, { "ScenarioDurationSec", "3600" }, { "DungeonTeleport", "1" } },
                                     kFile, warnings);
    BotSettings const s = ParseBotSettings(merged, warnings);
    CHECK(s.Scenarios.size() == 1 && s.Scenario.Name == "rfc" && s.Scenario.Parties == 20 && s.Scenario.Level == 17 && s.Scenario.DurationSec == 3600);
    CHECK(s.Scenario.Entry[0] == 389.0f && s.Scenario.Route.size() == 3 && s.Scenario.Bosses.size() == 2 && s.Scenario.HasPortal);
    bool settingsWarning = false;
    for (std::string const& w : warnings)
        settingsWarning = settingsWarning || w.find("Module.forever-bots") != std::string::npos;
    CHECK(!settingsWarning);
}

void UnknownDungeonOrNoDungeonChangesNothing()
{
    std::vector<std::string> warnings;
    std::unordered_map<std::string, std::string> const none{ { "MaxBots", "3" } };
    CHECK(MergeDungeon(none, kFile, warnings) == none && warnings.empty());
    std::unordered_map<std::string, std::string> const other{ { "Dungeon", "nope" } };
    CHECK(MergeDungeon(other, kFile, warnings) == other && !warnings.empty() &&
          warnings.back().find("'nope' is not in the dungeon file") != std::string::npos);
    warnings.clear();
    std::unordered_map<std::string, std::string> const missing{ { "Dungeon", "rfc" }, { "DungeonFile", "/nonexistent/dungeons.txt" } };
    CHECK(MergeDungeonFile(missing, warnings) == missing && warnings.size() == 1);
    CHECK(ParseBotSettings(missing, warnings).Scenario.Name.empty()); // no dungeon, no scenario
}

void ReadsTheFileFromDisk()
{
    std::string const path = "dungeon_test_tmp.txt";
    {
        std::ofstream out(path);
        out << kFile;
    }
    std::vector<std::string> warnings;
    auto const merged = MergeDungeonFile({ { "Dungeon", "rfc" }, { "DungeonFile", path } }, warnings);
    CHECK(merged.count("ScenarioRoute3") == 1 && merged.at("Scenario") == "rfc");
    std::remove(path.c_str());
}

// --- the director's dungeon-clear rule ---

WorldTickSnapshotView View(std::uint64_t tick, std::vector<BotSnapshot> const& bots)
{
    WorldTickSnapshotView view;
    view.tick_number = tick;
    view.max_nearby_units = 1;
    view.bots = bots;
    view.nearby.resize(bots.size());
    return view;
}

BotSnapshot Bot(std::uint64_t guid, std::uint8_t role)
{
    BotSnapshot bot;
    bot.guid = guid;
    bot.health = bot.max_health = 1000;
    bot.map_id = 36;
    bot.group_role = role;
    bot.group_size = 2;
    return bot;
}

struct Run
{
    ScenarioDirector director;
    std::vector<BotSnapshot> bots{ Bot(1, 2), Bot(2, 1) };
    std::uint64_t t = 1000, tick = 1;
    std::vector<BotAction> actions;

    explicit Run(ScenarioPlan plan) : director(std::move(plan))
    {
        for (int i = 0; i < 12 && director.Phase() != ScenarioPhase::Running; ++i)
            director.Decide(View(++tick, bots), t += 3000, actions);
    }
    void Step(std::uint64_t advance, std::uint32_t deadBossEntry = 0)
    {
        WorldTickSnapshotView view = View(++tick, bots);
        if (deadBossEntry != 0)
        {
            view.bots[1].nearby_count = 1;
            view.nearby[1].guid = forever_module::MakeUnitRef(8, deadBossEntry, 5);
            view.nearby[1].health = 0;
        }
        director.Decide(view, t += advance, actions);
    }
};

ScenarioPlan ClearPlan()
{
    ScenarioPlan plan;
    plan.Name = "rfc";
    plan.RosterSize = 2;
    plan.EntryMap = 36;
    plan.Route = { { 0.0f, 100.0f, 0.0f }, { 0.0f, 200.0f, 0.0f } };
    plan.DurationMs = 1000000;
    plan.Bosses = { { 11517, "Oggleflint" }, { 11519, "Bazzalan" } };
    plan.RequireBosses = true;
    return plan;
}

void ClearNeedsEveryBoss()
{
    Run run(ClearPlan());
    CHECK(run.director.Phase() == ScenarioPhase::Running);
    run.bots[0].pos_y = 200.0f; // the end of the route alone is not a clear
    run.Step(1000);
    CHECK(run.director.Phase() == ScenarioPhase::Running);
    run.Step(1000, 11517);
    CHECK(run.director.Phase() == ScenarioPhase::Running); // one of two
    run.Step(1000, 11519);
    auto const result = run.director.TakeResult();
    CHECK(result && result->Outcome == "complete" && result->BossesKilled.size() == 2 && result->Furthest == 2);
}

void BossesAreEnoughEvenBeforeTheRouteEnds()
{
    Run run(ClearPlan());
    run.Step(1000, 11517);
    run.Step(1000, 11519);
    auto const result = run.director.TakeResult();
    CHECK(result && result->Outcome == "complete" && result->Furthest == 0);
}

void MissingBossesAfterTheRouteEndGiveUp()
{
    Run run(ClearPlan());
    run.bots[0].pos_y = 200.0f;
    run.Step(1000);
    run.Step(kBossGraceMs - 2000);
    CHECK(run.director.Phase() == ScenarioPhase::Running);
    run.Step(2500);
    auto const result = run.director.TakeResult();
    CHECK(result && result->Outcome == "bosses_missing");
}

void WithoutRequireBossesTheRouteEndStillCompletes()
{
    ScenarioPlan plan = ClearPlan();
    plan.RequireBosses = false;
    Run run(plan);
    run.bots[0].pos_y = 200.0f;
    run.Step(1000);
    auto const result = run.director.TakeResult();
    CHECK(result && result->Outcome == "complete");
}

void AWipeStillEndsAClear()
{
    Run run(ClearPlan());
    for (BotSnapshot& bot : run.bots)
        bot.life_state = 1, bot.health = 0;
    run.Step(1000);
    auto const result = run.director.TakeResult();
    CHECK(result && result->Outcome == "wipe");
}

} // namespace

int main()
{
    ParsesADungeon();
    UnknownKeysAndMissingEndAreReported();
    MergeFillsTheScenarioKeys();
    ExplicitConfigWins();
    UnknownDungeonOrNoDungeonChangesNothing();
    MergeFillsEachListedDungeon();
    SingleDungeonMergeIsUnchanged();
    ReadsTheFileFromDisk();
    ClearNeedsEveryBoss();
    BossesAreEnoughEvenBeforeTheRouteEnds();
    MissingBossesAfterTheRouteEndGiveUp();
    WithoutRequireBossesTheRouteEndStillCompletes();
    AWipeStillEndsAClear();
    if (failures == 0)
        std::printf("dungeon_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
