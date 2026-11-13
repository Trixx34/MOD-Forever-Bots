// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests of the run telemetry: the stall cause, the 300 s no-progress rule, the line formats, the run file, and the director's lines (party_tl:, party_stalled:,
// boss_attempt:, the stalled= fields of the result line). No core needed.
#include "scenario_test_support.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

using namespace run_telemetry;

namespace
{

bool Has(std::string const& text, std::string const& part)
{
    return text.find(part) != std::string::npos;
}

void StallCauseIsPure()
{
    StallCounters c;
    c.WindowMs = 300000;
    CHECK(StallCause(c) == "unknown");
    c.Pulls = 9;
    CHECK(StallCause(c) == "unknown");  // under the churn threshold
    c.Pulls = 12;
    CHECK(StallCause(c) == "fight_churn");
    c.RouteRefusals = 5;                // score 1.0 against 1.2
    CHECK(StallCause(c) == "fight_churn");
    c.RouteRefusals = 8;                // score 1.6
    CHECK(StallCause(c) == "route_refusals");
    c = {};
    c.WindowMs = 300000;
    c.RestMs = 119000;                  // under 40 %
    CHECK(StallCause(c) == "unknown");
    c.RestMs = 150000;
    CHECK(StallCause(c) == "resting");
    c.DeadMs = 200000;                  // the larger share wins
    CHECK(StallCause(c) == "dead_members");
    StallCounters const zero;           // a zero window never divides by zero
    CHECK(StallCause(zero) == "unknown");
}

void NoProgressRule()
{
    StallTracker tracker;
    tracker.Start(1000);
    CHECK(!tracker.Check(1000 + 299999));
    CHECK(tracker.Check(1000 + 300000) && tracker.Count() == 1);
    CHECK(!tracker.Check(1000 + 300001) && !tracker.Check(1000 + 599999)); // at most one per 300 s
    CHECK(tracker.Check(1000 + 600000) && tracker.Count() == 2);
    tracker.NoteProgress(1000 + 650000);                                   // progress restarts the clock, the count stays
    CHECK(!tracker.Check(1000 + 900000));
    CHECK(tracker.Check(1000 + 950000) && tracker.Count() == 3);
    StallTracker idle;                                                      // never started: no verdict
    CHECK(!idle.Check(10000000));
}

void LineFormats()
{
    PartyId const id{ "dm#2", "" };
    Timeline tl;
    tl.TS = 120;
    tl.Leader = 5;
    tl.Map = 36;
    tl.X = -14.5f;
    tl.Y = -380.25f;
    tl.Z = 62.0f;
    tl.Step = "walk";
    tl.Point = 3;
    tl.Points = 40;
    tl.Target = 0;
    tl.Alive = 5;
    tl.Total = 5;
    tl.InFight = 0;
    tl.Last = { true, "Rhahk'Zor", 0, 100 };
    CHECK(FormatTimeline(id, tl) == "party_tl: scenario=dm party=2 variant= t_s=120 leader=5 map=36 pos=-14.5,-380.2,62.0 step=walk route=3/40 target=0 alive=5/5 fight=0 last=boss:Rhahk'Zor@100");
    tl.Last = { false, "", 3, 90 };
    CHECK(Has(FormatTimeline({ "dm", "" }, tl), "party_tl: scenario=dm party=1 variant= t_s=120 leader=5"));
    CHECK(Has(FormatTimeline(id, tl), " last=route:3@90"));
    Stall st;
    st.TS = 900;
    st.Cause = "resting";
    st.NoProgressS = 300;
    st.Counters = { 2, 0, 210000, 0, 40000, 300000 };
    st.Count = 1;
    st.Point = 3;
    st.Points = 40;
    st.Last = { false, "", 3, 600 };
    CHECK(FormatStall(id, st) == "party_stalled: scenario=dm party=2 variant= t_s=900 cause=resting no_progress_s=300 pulls=2 route_refusals=0 rest_s=210 dead_s=0 fight_s=40 stalls=1 route=3/40 last=route:3@600");
    BossAttempt ba;
    ba.TS = 640;
    ba.Boss = "Mr. Smite";
    ba.Outcome = "open";
    CHECK(FormatBossAttempt(id, ba) == "boss_attempt: scenario=dm party=2 variant= t_s=640 boss=Mr._Smite stage=start dur_s=0 deaths=0 outcome=open");
    ba.End = true;
    ba.DurS = 80;
    ba.Deaths = 1;
    ba.Outcome = "killed";
    CHECK(FormatBossAttempt(id, ba) == "boss_attempt: scenario=dm party=2 variant= t_s=640 boss=Mr._Smite stage=end dur_s=80 deaths=1 outcome=killed");
    CHECK(FormatResultFields(0, "", 0).empty());
    CHECK(FormatResultFields(2, "resting", 600) == "stalled=2 last_stall=resting@600");
    std::string scenario;
    std::uint32_t party = 0;
    SplitName("sfk#10", scenario, party);
    CHECK(scenario == "sfk" && party == 10);
    SplitName("wc", scenario, party);
    CHECK(scenario == "wc" && party == 1);
}

void RunFileAppendsAndWarns()
{
    namespace fs = std::filesystem;
    fs::path const dir = fs::temp_directory_path() / ("run_telemetry_test_" + std::to_string(::getpid()));
    fs::create_directories(dir);
    {
        RunFile file(dir.string());
        std::string warn;
        file.Append("party_tl: a=1", 1000, 0, &warn);
        file.Append("party_tl: a=2", 2000, 0, &warn);
        file.Flush(31000);
        CHECK(warn.empty());
        std::ifstream in(dir / "run-telemetry.log");
        std::string first, second;
        std::getline(in, first);
        std::getline(in, second);
        CHECK(first == "1970-01-01T00:00:00Z party_tl: a=1");
        CHECK(second == "1970-01-01T00:00:00Z party_tl: a=2");
    }
    RunFile bad((dir / "missing" / "deeper").string());
    std::string warn;
    bad.Append("x", 1, 0, &warn);
    CHECK(Has(warn, "cannot open"));
    warn.clear();
    bad.Append("y", 2, 0, &warn);
    CHECK(warn.empty()); // one warning only
    RunFile off("");
    off.Append("z", 1, 0, nullptr);
    CHECK(!off.Enabled());
    fs::remove_all(dir);
}

ScenarioPlan LongPlan()
{
    ScenarioPlan plan = Plan();
    plan.Name = "dm#2";
    plan.DurationMs = 700000;
    plan.Bosses = { { 644, "Rhahk'Zor" } };
    return plan;
}

std::vector<std::string> Lines(ScenarioDirector& director, std::string const& kind, std::vector<std::string>& all)
{
    for (std::string const& line : director.TakeTelemetry())
        all.push_back(line);
    std::vector<std::string> out;
    for (std::string const& line : all)
        if (line.rfind(kind + ":", 0) == 0)
            out.push_back(line);
    return out;
}

void DirectorStallsAndTimeline()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioDirector director = RunningDirector(t, tick, bots, LongPlan());
    CHECK(director.Phase() == ScenarioPhase::Running);
    std::vector<BotAction> actions;
    std::vector<std::string> all;
    std::uint64_t const start = t;
    bots[1].health = 300; // a member under 90 %: the party is resting the whole time
    for (; t < start + 650000;)
        director.Decide(View(++tick, bots), t += 5000, actions);
    auto const stalls = Lines(director, "party_stalled", all);
    CHECK(stalls.size() == 2);
    if (stalls.size() == 2)
    {
        CHECK(Has(stalls[0], "party_stalled: scenario=dm party=2 variant= t_s=300 cause=resting no_progress_s=300"));
        CHECK(Has(stalls[1], "t_s=600 cause=resting") && Has(stalls[1], "stalls=2"));
    }
    auto const timeline = Lines(director, "party_tl", all);
    CHECK(timeline.size() >= 20 && timeline.size() <= 22); // one per 30 s of 650 s
    if (!timeline.empty())
        CHECK(Has(timeline[0], "party_tl: scenario=dm party=2 variant= t_s=30 leader=1 map=36 pos=0.0,0.0,0.0 step=rest route=0/2 target=0 alive=3/3 fight=0 last=route:0@0"));
    // The result line, at the timeout
    director.Decide(View(++tick, bots), t += 60000, actions);
    auto const result = director.TakeResult();
    CHECK(result && result->Stalled == 2 && result->LastStallCause == "resting" && result->LastStallS == 600);
    if (result)
    {
        std::string const line = FormatScenarioResult(*result);
        CHECK(Has(line, "outcome=timeout furthest=0/2"));
        CHECK(Has(line, " stalled=2 last_stall=resting@600"));
        CHECK(Has(line, " party=yes"));
    }
}

void DirectorProgressResetsTheClock()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioDirector director = RunningDirector(t, tick, bots, LongPlan());
    std::vector<BotAction> actions;
    std::vector<std::string> all;
    std::uint64_t const start = t;
    for (; t < start + 250000;)
        director.Decide(View(++tick, bots), t += 5000, actions);
    bots[0].pos_y = 100.0f; // the leader reaches route point 1
    director.Decide(View(++tick, bots), t += 5000, actions);
    for (; t < start + 540000;)
        director.Decide(View(++tick, bots), t += 5000, actions);
    CHECK(Lines(director, "party_stalled", all).empty()); // 285 s since the progress
    for (; t < start + 560000;)
        director.Decide(View(++tick, bots), t += 5000, actions);
    auto const stalls = Lines(director, "party_stalled", all);
    CHECK(stalls.size() == 1 && Has(stalls[0], "route=1/2") && Has(stalls[0], "last=route:1@255") && Has(stalls[0], "cause=unknown"));
}

void DirectorBossAttempt()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioDirector director = RunningDirector(t, tick, bots, LongPlan());
    std::vector<BotAction> actions;
    std::vector<std::string> all;
    director.Decide(View(++tick, bots), t += 5000, actions); // quiet
    auto const fight = [&](std::uint32_t bossHealth, bool combat)
    {
        bots[1].in_combat = combat;
        WorldTickSnapshotView view = View(++tick, bots);
        view.bots[1].nearby_count = 1;
        view.nearby[1].guid = forever_module::MakeUnitRef(8, 644, 5);
        view.nearby[1].health = bossHealth;
        view.nearby[1].max_health = 1000;
        director.Decide(view, t += 5000, actions);
    };
    fight(900, true);
    fight(500, true);
    bots[2].health = 0;
    bots[2].life_state = 1; // a death during the attempt
    fight(300, true);
    fight(0, false);        // the boss is dead
    auto const attempts = Lines(director, "boss_attempt", all);
    CHECK(attempts.size() == 2);
    if (attempts.size() == 2)
    {
        CHECK(Has(attempts[0], "boss=Rhahk'Zor stage=start dur_s=0 deaths=0 outcome=open"));
        CHECK(Has(attempts[1], "boss=Rhahk'Zor stage=end dur_s=") && Has(attempts[1], "deaths=1 outcome=killed"));
    }
}

void DirectorDamageAggregates()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioDirector director = RunningDirector(t, tick, bots, LongPlan());
    std::vector<BotAction> actions;
    std::vector<std::string> all;
    auto const hit = [](std::uint32_t entry, std::uint32_t spell, std::uint32_t amount, bool killing)
    {
        damage_events::Hit h;
        h.Entry = entry;
        h.Spell = spell;
        h.Amount = amount;
        h.Killing = killing;
        h.Overkill = killing ? 50 : 0;
        return h;
    };
    // quiet 30 s windows carry no damage fields
    for (int i = 0; i < 7; ++i)
        director.Decide(View(++tick, bots), t += 5000, actions);
    for (std::string const& line : Lines(director, "party_tl", all))
        CHECK(!Has(line, "dmg_by_"));
    all.clear();
    // hits before the attempt are in the party window only; the attempt opens, takes two more, ends with the boss dead
    director.NoteDamage(hit(100, 7, 400, false));
    auto const fight = [&](std::uint32_t bossHealth, bool combat)
    {
        bots[1].in_combat = combat;
        WorldTickSnapshotView view = View(++tick, bots);
        view.bots[1].nearby_count = 1;
        view.nearby[1].guid = forever_module::MakeUnitRef(8, 644, 5);
        view.nearby[1].health = bossHealth;
        view.nearby[1].max_health = 1000;
        director.Decide(view, t += 5000, actions);
    };
    fight(900, true);
    director.NoteDamage(hit(644, 20566, 3000, false));
    director.NoteDamage(hit(644, 20566, 1500, true));
    fight(500, true);
    fight(0, false);
    auto const attempts = Lines(director, "boss_attempt", all);
    CHECK(attempts.size() == 2);
    if (attempts.size() == 2)
    {
        CHECK(!Has(attempts[0], "dmg_by_"));
        CHECK(Has(attempts[1], " dmg_by_spell=20566:4500:2:1 dmg_by_attacker=644:4500:2:1 death_by_spell=20566:1:50 death_by_attacker=644:1:50"));
    }
    // the party window has all three hits, once
    for (int i = 0; i < 6; ++i)
        director.Decide(View(++tick, bots), t += 5000, actions);
    std::string withDamage;
    for (std::string const& line : Lines(director, "party_tl", all))
        if (Has(line, "dmg_by_spell="))
            withDamage += line + "\n";
    CHECK(Has(withDamage, "dmg_by_spell=20566:4500:2:1,7:400:1:0 dmg_by_attacker=644:4500:2:1,100:400:1:0"));
    CHECK(std::count(withDamage.begin(), withDamage.end(), '\n') == 1);
}

void DirectorBossAbort()
{
    std::uint64_t t, tick;
    std::vector<BotSnapshot> bots;
    ScenarioDirector director = RunningDirector(t, tick, bots, LongPlan());
    std::vector<BotAction> actions;
    std::vector<std::string> all;
    for (int i = 0; i < 6; ++i)
    {
        bots[1].in_combat = i < 2;
        WorldTickSnapshotView view = View(++tick, bots);
        view.bots[1].nearby_count = 1;
        view.nearby[1].guid = forever_module::MakeUnitRef(8, 644, 5);
        view.nearby[1].health = 800;
        view.nearby[1].max_health = 1000;
        director.Decide(view, t += 5000, actions);
    }
    auto const attempts = Lines(director, "boss_attempt", all);
    CHECK(attempts.size() == 2 && Has(attempts[1], "stage=end") && Has(attempts[1], "outcome=aborted"));
}

} // namespace

int main()
{
    StallCauseIsPure();
    NoProgressRule();
    LineFormats();
    RunFileAppendsAndWarns();
    DirectorStallsAndTimeline();
    DirectorProgressResetsTheClock();
    DirectorBossAttempt();
    DirectorBossAbort();
    DirectorDamageAggregates();
    if (failures == 0)
        std::printf("run_telemetry_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
