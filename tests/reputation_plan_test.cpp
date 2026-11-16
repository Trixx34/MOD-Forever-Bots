// SPDX-License-Identifier: GPL-2.0-or-later
// Reputation planner (src/reputation_plan.h): parsing, target choice, source choice, the state machine, determinism, the per-call bound, the shipped data and the
// cost of planning for 800 bots.
#include "reputation_plan.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

using namespace reputation;

constexpr char kTargets[] =
    "# targets\n"
    "529|Argent Dawn|0|50|6|1|x\n"
    "576|Timbermaw Hold|0|48|4|1|x\n"
    "270|Zandalarian Emissaries|0|56|7|2|x\n"
    "9999|Horde Only|2|10|5|2|x\n"
    "broken line\n";
constexpr char kSources[] =
    "q|529|100|250|50|0|0|0\n"        // a quest, 250 points, level 50
    "q|529|101|75|55|0|1|0\n"         // a hand-in
    "q|529|102|500|58|0|0|0\n"        // above a level-55 bot
    "q|529|103|75|40|2|0|0\n"         // Horde only
    "q|529|104|150|40|0|0|5\n"        // gives reputation up to Honored only
    "k|529|200|5|4|0|1000.5|2000.5|30.0|20\n"   // a kill in the open world, up to Friendly
    "k|529|201|5|7|329|1.0|2.0|3.0|12\n"        // a kill in Stratholme: not for the open-world planner
    "q|576|300|75|48|0|0|0\n"
    "q|270|400|75|56|0|0|0\n"
    "bad|line\n";

PlanData Data(std::vector<std::string>* errors = nullptr)
{
    std::vector<std::string> e;
    auto targets = ParseTargets(kTargets, e);
    auto sources = ParseSources(kSources, e);
    if (errors)
        *errors = e;
    return BuildPlanData(std::move(targets), std::move(sources));
}

Facts Bot(std::uint32_t level, std::uint8_t side)
{
    Facts f;
    f.Level = level;
    f.Side = side;
    return f;
}

PlanSettings On()
{
    PlanSettings s;
    s.Enabled = true;
    return s;
}

void ParsingKeepsGoodLinesAndReportsBadOnes()
{
    std::vector<std::string> errors;
    PlanData const d = Data(&errors);
    CHECK(errors.size() == 2);   // the broken target line and the bad source line
    CHECK(d.Targets.size() == 4);
    CHECK(d.Sources.size() == 9);
    CHECK(d.Range.count(529) == 1 && d.Range.at(529).second - d.Range.at(529).first == 7);
    CHECK(d.Range.count(9999) == 0);   // no source: never chosen
    CHECK(d.Targets[0].GoalRank == kRevered && d.Targets[2].Tier == 2);
    std::vector<std::string> e2;
    auto bad = ParseSources("q|1|2|0|10|0|0|0\nk|1|2|5|0|0|1|2|3|0\nq|1|2|5|10|3|0|0\n", e2);
    CHECK(bad.empty() && e2.size() == 3);   // zero points, zero spawns, side 3
}

void TargetFollowsLevelSideAndTier()
{
    PlanData const d = Data();
    CHECK(!ChooseTarget(d, Bot(40, 1), 7, 0));   // below every minimum
    auto t = ChooseTarget(d, Bot(49, 1), 7, 0);
    CHECK(t && d.Targets[*t].Faction == 576);    // level 49: only Timbermaw Hold (48) fits, Argent Dawn starts at 50
    // Tier 1 holds Argent Dawn and Timbermaw: bots spread over both, never to the tier-2 Zandalarian.
    int ad = 0, tm = 0, other = 0;
    for (std::uint64_t g = 1; g <= 400; ++g)
    {
        auto c = ChooseTarget(d, Bot(60, 1), g, 0);
        if (!c) { ++other; continue; }
        std::uint32_t const f = d.Targets[*c].Faction;
        (f == 529 ? ad : f == 576 ? tm : other)++;
    }
    CHECK(other == 0 && ad > 100 && tm > 100);
    // A faction reached its goal rank: the bot goes on to the next tier.
    Facts done = Bot(60, 1);
    done.Standings = { { 529, RankStart(kRevered) }, { 576, RankStart(kFriendly) } };
    auto n = ChooseTarget(d, done, 5, 0);
    CHECK(n && d.Targets[*n].Faction == 270);
    // Same guid, same answer; the current goal is kept.
    CHECK(ChooseTarget(d, Bot(60, 1), 99, 0) == ChooseTarget(d, Bot(60, 1), 99, 0));
    auto keep = ChooseTarget(d, Bot(60, 1), 99, 576);
    CHECK(keep && d.Targets[*keep].Faction == 576);
}

void SourceChoicePrefersQuestsAndFiltersByLevelSideCapAndMap()
{
    PlanData const d = Data();
    Track t;
    Facts f = Bot(55, 1);
    // Only Argent Dawn is tier 1 and 50+; run many guids: never the level-58 quest, never the Horde quest, never the Stratholme creature.
    bool sawTimbermaw = false;
    for (std::uint64_t g = 1; g <= 200; ++g)
    {
        Track tr;
        Step const s = Advance(d, On(), tr, f, g, 1000);
        CHECK(s.Valid && s.Changed && s.Src);
        if (!s.Src)
            continue;
        if (s.Faction == 576)
            sawTimbermaw = true;
        else
        {
            CHECK(s.Src->Id != 102 && s.Src->Id != 103 && s.Src->Id != 201);
            CHECK(s.Src->Id == 100 || s.Src->Id == 101 || s.Src->Id == 104);   // a quest, the biggest first: 100 (250 points) wins over the kill
            CHECK(s.Src->Id == 100);
        }
    }
    CHECK(sawTimbermaw);
    // At Honored the capped quest (up to Honored) gives nothing more; with the quests of 100 and 101 gone the open-world kill is left, but it is capped at Friendly.
    Facts h = Bot(55, 1);
    h.Standings = { { 529, RankStart(kHonored) }, { 576, RankStart(kFriendly) } };
    Track tr;
    Step const s = Advance(d, On(), tr, h, 1, 1000);
    CHECK(s.Valid && s.Src && s.Src->Id == 100);   // 100 has no cap
    // Skipped sources are not planned again.
    Track skip;
    skip.Skipped[0] = ((std::uint64_t(0) << 32) | 100) + 1;
    Step const s2 = Advance(d, On(), skip, Bot(55, 1), 3, 1000);
    CHECK(!s2.Valid || s2.Src->Id != 100);
}

void StateMachineStallsAndReplans()
{
    PlanData const d = Data();
    PlanSettings const settings = On();
    Track t;
    Facts f = Bot(55, 1);
    f.Standings = { { 529, 100 } };   // standings known
    Step s = Advance(d, settings, t, f, 4, 0);
    CHECK(s.Valid && s.Changed && t.State == Phase::kActive);
    std::uint32_t const first = t.SourceId;
    std::uint32_t const faction = t.Faction;
    f.Standings = { { 529, 100 }, { 576, 0 } };
    // Standing unchanged for the stall time: the source is given up and another one planned.
    s = Advance(d, settings, t, f, 4, settings.StallMs - 1);
    CHECK(s.Valid && !s.Changed && t.SourceId == first);
    s = Advance(d, settings, t, f, 4, settings.StallMs);
    CHECK(t.Skipped[0] != 0);
    CHECK(!s.Valid || s.Changed);
    CHECK(!s.Valid || s.Src->Id != first || s.Faction != faction);
    // Progress restarts the stall clock.
    Track p;
    f.Standings = { { 529, 0 }, { 576, 0 } };
    Step a = Advance(d, settings, p, f, 4, 0);
    CHECK(a.Valid);
    f.Standings = { { 529, 50 }, { 576, 50 } };
    a = Advance(d, settings, p, f, 4, settings.StallMs - 10);
    CHECK(a.Valid && !a.Changed && p.StartedMs == settings.StallMs - 10);
    a = Advance(d, settings, p, f, 4, settings.StallMs + 5000);
    CHECK(a.Valid && !a.Changed);
    // No standings from the core: nothing can be observed, so nothing is given up.
    Track blind;
    Facts none = Bot(55, 1);
    Advance(d, settings, blind, none, 4, 0);
    Step b = Advance(d, settings, blind, none, 4, settings.StallMs * 10);
    CHECK(b.Valid && !b.Changed && blind.Skipped[0] == 0);
    // Disabled: nothing at all.
    Track off;
    CHECK(!Advance(d, PlanSettings{}, off, f, 4, 0).Valid);
}

void ReplanIsRateLimitedAndBudgetBounded()
{
    PlanData const d = Data();
    PlanSettings settings = On();
    Track t;
    Facts f = Bot(30, 1);   // nothing fits
    std::uint32_t examined = 0;
    CHECK(!Advance(d, settings, t, f, 1, 0, &examined).Valid);
    CHECK(t.NextPlanMs == settings.ReplanMs);
    f.Level = 60;
    CHECK(!Advance(d, settings, t, f, 1, settings.ReplanMs - 1, &examined).Valid);   // too early: it does not even look
    CHECK(examined == 0);
    settings.Budget = 3;
    CHECK(Advance(d, settings, t, f, 1, settings.ReplanMs, &examined).Valid);
    CHECK(examined <= 3);
}

std::string Slurp(std::string const& name)
{
    std::ifstream in(std::string(FOREVER_BOTS_DATA_DIR) + "/" + name);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void ShippedDataParsesAndCostsLittle()
{
    std::vector<std::string> errors;
    PlanData const d = BuildPlanData(ParseTargets(Slurp("reputation_targets.txt"), errors), ParseSources(Slurp("reputation_sources.txt"), errors));
    CHECK(errors.empty());
    CHECK(d.Targets.size() >= 7);
    CHECK(d.Sources.size() > 300);
    for (Target const& t : d.Targets)
        CHECK(d.Range.count(t.Faction) == 1);   // every target has sources

    // 800 bots of levels 50..60 and both sides, one planning call each, then one steady-state call each (an active plan).
    PlanSettings const settings = On();
    std::vector<Track> tracks(800);
    std::vector<Facts> facts(800);
    for (std::size_t i = 0; i < 800; ++i)
        facts[i] = Bot(std::uint32_t(50 + i % 11), std::uint8_t(1 + i % 2));
    using Clock = std::chrono::steady_clock;
    std::uint32_t examined = 0;
    std::size_t valid = 0;
    auto t0 = Clock::now();
    for (std::size_t i = 0; i < 800; ++i)
        valid += Advance(d, settings, tracks[i], facts[i], 1000 + i, 5000, &examined).Valid;
    auto t1 = Clock::now();
    constexpr int kRounds = 200;   // 200 ticks of steady state
    for (int r = 0; r < kRounds; ++r)
        for (std::size_t i = 0; i < 800; ++i)
            valid += Advance(d, settings, tracks[i], facts[i], 1000 + i, 6000 + r * 1000).Valid;
    auto t2 = Clock::now();
    double const planUs = std::chrono::duration<double, std::micro>(t1 - t0).count() / 800;
    double const tickUs = std::chrono::duration<double, std::micro>(t2 - t1).count() / (800.0 * kRounds);
    std::printf("reputation planner: first plan %.2f us per bot (%u sources examined for 800 bots), steady state %.3f us per bot and tick, %.1f us per tick for 800 bots\n", planUs,
                examined, tickUs, tickUs * 800);
    CHECK(valid > 800);
    CHECK(examined <= 800u * settings.Budget);
    CHECK(tickUs < 20.0);   // 800 bots x 20 us = 16 ms; the real figure is far lower (a tick is 100 ms or more)
}

} // namespace

int main()
{
    ParsingKeepsGoodLinesAndReportsBadOnes();
    TargetFollowsLevelSideAndTier();
    SourceChoicePrefersQuestsAndFiltersByLevelSideCapAndMap();
    StateMachineStallsAndReplans();
    ReplanIsRateLimitedAndBudgetBounded();
    ShippedDataParsesAndCostsLittle();
    if (failures == 0)
        std::printf("reputation_plan_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
