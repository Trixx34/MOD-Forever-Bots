// SPDX-License-Identifier: GPL-2.0-or-later
// DecideScheduler (src/decide_scheduler.h, BotPolicy::Decide): tiers, the budget's carry-over, the starvation cap, the guid stagger, the never-skip rules,
// a simulated 800-bot load, and the policy integration.
#include "policy_test_support.h"
#include "decide_scheduler.h"

#include <map>
#include <random>

using decide_scheduler::BotFacts;
using decide_scheduler::Scheduler;
using decide_scheduler::Settings;
using decide_scheduler::Tier;
using decide_scheduler::TierOf;

namespace {

Settings On(std::uint32_t budgetUs = 0, std::uint32_t walk = 3, std::uint32_t idle = 6, std::uint32_t cap = 12)
{
    Settings settings;
    settings.Enabled = true;
    settings.BudgetUs = budgetUs;
    settings.TierWalk = walk;
    settings.TierIdle = idle;
    settings.MaxSkipTicks = cap;
    return settings;
}

BotFacts Idle(std::uint64_t guid)
{
    BotFacts facts;
    facts.Guid = guid;
    return facts;
}

BotFacts Walking(std::uint64_t guid)
{
    BotFacts facts = Idle(guid);
    facts.Moving = true;
    return facts;
}

BotFacts Fighting(std::uint64_t guid)
{
    BotFacts facts = Idle(guid);
    facts.InCombat = true;
    return facts;
}

// A fake clock: each turn costs what costOf says.
struct Harness
{
    Scheduler scheduler;
    std::uint64_t nowUs = 0;
    std::function<std::uint64_t(std::size_t)> costOf = [](std::size_t) { return std::uint64_t(10); };
    std::vector<int> ran;
    std::vector<int> skipped;
    std::vector<std::size_t> order;

    explicit Harness(Settings settings) : scheduler(settings) {}

    void Tick(std::vector<BotFacts> const& facts)
    {
        ran.assign(facts.size(), 0);
        skipped.assign(facts.size(), 0);
        order.clear();
        scheduler.Tick(
            facts,
            [&](std::size_t i) {
                ++ran[i];
                order.push_back(i);
                nowUs += costOf(i);
            },
            [&](std::size_t i) { ++skipped[i]; }, [&] { return nowUs; });
    }
};

void TierChoice()
{
    CHECK(TierOf(Idle(1)) == Tier::kIdle);
    CHECK(TierOf(Walking(1)) == Tier::kWalk);
    BotFacts f = Idle(1);
    f.Questing = true;
    CHECK(TierOf(f) == Tier::kWalk);   // calm questing
    f.Resting = true;
    CHECK(TierOf(f) == Tier::kIdle);   // resting wins over the quest log
    CHECK(TierOf(Fighting(1)) == Tier::kEvery);
    f = Walking(1);
    f.Party = true;
    CHECK(TierOf(f) == Tier::kEvery);
    f = Walking(1);
    f.Dungeon = true;
    CHECK(TierOf(f) == Tier::kEvery);
    f = Walking(1);
    f.HostileNear = true;
    CHECK(TierOf(f) == Tier::kEvery);
    f = Walking(1);
    f.Busy = true;
    CHECK(TierOf(f) == Tier::kEvery);
    f = Idle(1);
    f.LifeState = 1;
    CHECK(TierOf(f) == Tier::kIdle);   // dead, waiting to release
    f.LifeState = 2;
    f.Moving = true;
    CHECK(TierOf(f) == Tier::kIdle);   // corpse run
    f.GhostReviving = true;
    CHECK(TierOf(f) == Tier::kEvery);  // at its corpse: every tick
}

void OffRunsEveryBotInOrder()
{
    Harness h(Settings{});
    std::vector<BotFacts> facts = { Idle(1), Walking(2), Fighting(3) };
    for (int t = 0; t < 5; ++t)
    {
        h.Tick(facts);
        CHECK(h.ran == std::vector<int>({ 1, 1, 1 }));
        CHECK(h.order == std::vector<std::size_t>({ 0, 1, 2 }));
    }
    CHECK(h.scheduler.PeekStats().Ticks == 0);
    CHECK(decide_scheduler::FormatStats(Settings{}, h.scheduler.PeekStats()).empty());
}

void TiersRunOnTheirCadence()
{
    Harness h(On());
    std::vector<BotFacts> facts = { Idle(101), Walking(202), Fighting(303) };
    h.Tick(facts);
    CHECK(h.ran == std::vector<int>({ 1, 1, 1 }));   // first sight: everyone runs
    int idleRuns = 0, walkRuns = 0, fightRuns = 0;
    for (int t = 0; t < 60; ++t)
    {
        h.Tick(facts);
        idleRuns += h.ran[0];
        walkRuns += h.ran[1];
        fightRuns += h.ran[2];
        CHECK(h.ran[0] + h.skipped[0] == 1);
    }
    CHECK(idleRuns == 10);
    CHECK(walkRuns == 20);
    CHECK(fightRuns == 60);
    decide_scheduler::Stats const stats = h.scheduler.TakeStats();
    CHECK(stats.Skipped == 50 + 40);
    CHECK(stats.Deferred == 0 && stats.Overruns == 0);
    CHECK(h.scheduler.PeekStats().Ticks == 0);   // taken
}

void NeverSkipRules()
{
    // A budget that is used up before the first optional turn: only the never-skip rules let a bot run.
    Harness h(On(1, 6, 6, 60));
    h.costOf = [](std::size_t) { return std::uint64_t(5); };
    std::vector<BotFacts> facts = { Fighting(100) };   // a fighting bot uses the budget up first
    for (std::uint64_t g = 1; g <= 7; ++g)
        facts.push_back(Walking(g));
    h.Tick(facts);   // first sight
    for (int t = 0; t < 7; ++t)
        h.Tick(facts);
    auto const runsWith = [&](std::function<void(BotFacts&)> change) {
        std::vector<BotFacts> changed = facts;
        change(changed[3]);
        h.Tick(changed);
        bool const ran = h.ran[3] == 1;
        h.Tick(changed);   // the scheduler remembers what it saw at the turn
        facts = changed;
        return ran;
    };
    CHECK(runsWith([](BotFacts& f) { f.InCombat = true; }));
    CHECK(runsWith([](BotFacts& f) { f.InCombat = false; f.ActionSequence = 7; f.LastRefused = true; }));   // a refusal
    CHECK(runsWith([](BotFacts& f) { f.ActionSequence = 8; f.LastRefused = false; }));                       // any new result
    CHECK(runsWith([](BotFacts& f) { f.LifeState = 1; f.Moving = false; }));                                  // death
    CHECK(runsWith([](BotFacts& f) { f.LifeState = 2; }));                                                    // released
    CHECK(runsWith([](BotFacts& f) { f.LifeState = 0; }));                                                    // resurrected
    runsWith([](BotFacts& f) { f.Moving = true; });                                                           // starting a walk is not a reaction
    CHECK(runsWith([](BotFacts& f) { f.Moving = false; }));                                                   // the walk ended
    CHECK(runsWith([](BotFacts& f) { f.PendingReaction = true; }));                                           // invite, loot, trade, request
    CHECK(runsWith([](BotFacts& f) { f.PendingReaction = true; }));                                           // still pending: still every tick
    CHECK(runsWith([](BotFacts& f) { f.PendingReaction = false; f.GhostReviving = true; }));
    CHECK(runsWith([](BotFacts& f) { f.GhostReviving = false; f.Party = true; }));
    CHECK(runsWith([](BotFacts& f) { f.Party = false; f.Dungeon = true; }));
    // The rest of the bots were deferred by the budget the whole time, and none waited longer than the cap.
    for (std::uint64_t g = 1; g <= 7; ++g)
        CHECK(h.scheduler.WaitedOf(g) <= 60);
    CHECK(h.scheduler.PeekStats().Deferred > 0);
    CHECK(h.scheduler.PeekStats().Forced > 0);
}

void BudgetCarriesOverRoundRobin()
{
    // 50 walkers due every tick (TierWalk 1), 100 us each, 1000 us budget: 10 per tick, every bot once in 5 ticks.
    Harness h(On(1000, 1, 1, 60));
    h.costOf = [](std::size_t) { return std::uint64_t(100); };
    std::vector<BotFacts> facts;
    for (std::uint64_t g = 1; g <= 50; ++g)
        facts.push_back(Walking(g));
    h.Tick(facts);   // first sight: all must run (50 turns, one overrun)
    CHECK(h.scheduler.TakeStats().Overruns == 1);
    std::vector<int> total(50, 0);
    int maxWait = 0;
    for (int t = 0; t < 50; ++t)
    {
        h.Tick(facts);
        int ranNow = 0;
        for (std::size_t i = 0; i < 50; ++i)
        {
            total[i] += h.ran[i];
            ranNow += h.ran[i];
            maxWait = std::max(maxWait, h.scheduler.WaitedOf(facts[i].Guid));
        }
        CHECK(ranNow == 10);
    }
    for (int runs : total)
        CHECK(runs == 10);   // fair: each bot exactly one turn in five ticks
    CHECK(maxWait <= 4);
    decide_scheduler::Stats const stats = h.scheduler.TakeStats();
    CHECK(stats.Deferred == 50 * 40);
    CHECK(stats.Overruns == 0);   // the last turn ends exactly at the budget
    CHECK(stats.MaxTurnUs == 100);
}

void StarvationCap()
{
    // One fighting bot alone takes 2000 us of a 1000 us budget: every optional turn is deferred, until the cap forces it.
    Harness h(On(1000, 2, 4, 5));
    h.costOf = [](std::size_t i) { return std::uint64_t(i == 0 ? 2000 : 10); };
    std::vector<BotFacts> facts = { Fighting(1) };
    for (std::uint64_t g = 2; g <= 21; ++g)
        facts.push_back(g % 2 ? Walking(g) : Idle(g));
    h.Tick(facts);
    h.scheduler.TakeStats();
    int maxWait = 0;
    std::vector<int> total(facts.size(), 0);
    for (int t = 0; t < 60; ++t)
    {
        h.Tick(facts);
        CHECK(h.ran[0] == 1);
        for (std::size_t i = 0; i < facts.size(); ++i)
        {
            total[i] += h.ran[i];
            maxWait = std::max(maxWait, h.scheduler.WaitedOf(facts[i].Guid));
        }
    }
    CHECK(maxWait <= 5);
    for (std::size_t i = 1; i < facts.size(); ++i)
        CHECK(total[i] >= 60 / 6);   // at least once every cap + 1 ticks
    decide_scheduler::Stats const stats = h.scheduler.TakeStats();
    CHECK(stats.Starved > 0);
    CHECK(stats.Overruns == 60);
}

void StaggerSpreadsTheLoad()
{
    for (std::uint32_t every : { 3u, 6u })
    {
        // Sequential guid counters (how the core hands them out) and spread ones.
        for (std::uint64_t base : { std::uint64_t(1), std::uint64_t(1u << 20), forever_module::MakeUnitRef(4, 1, 5000) })
        {
            std::vector<int> buckets(every, 0);
            for (std::uint64_t g = 0; g < 800; ++g)
            {
                std::uint32_t const phase = decide_scheduler::StaggerOf(base + g, every);
                CHECK(phase < every);
                CHECK(phase == decide_scheduler::StaggerOf(base + g, every));   // deterministic
                ++buckets[phase];
            }
            double const mean = 800.0 / every;
            for (int count : buckets)
                CHECK(count > mean * 0.8 && count < mean * 1.2);
        }
    }
    CHECK(decide_scheduler::StaggerOf(42, 1) == 0);
    // With the stagger the idle tier's turns are spread: 600 idle bots at TierIdle 6 run about 100 per tick, not 600 on one tick.
    Harness h(On(0, 3, 6, 12));
    std::vector<BotFacts> facts;
    for (std::uint64_t g = 1; g <= 600; ++g)
        facts.push_back(Idle(g));
    h.Tick(facts);
    for (int t = 0; t < 12; ++t)
    {
        h.Tick(facts);
        int ranNow = 0;
        for (int r : h.ran)
            ranNow += r;
        CHECK(ranNow > 70 && ranNow < 130);
    }
}

// 800 bots: 20% kEvery (combat, party, dungeon), 40% walking or questing, 40% resting, idle, dead or corpse run. Turn costs from the arm C pump line
// (11.2 ms at 400 bots = 28 us a bot on average): 40 us for a kEvery bot, 25 us walking, 18 us idle, with +-50% noise; 2% of the slower bots get a new
// action result each tick. Without the scheduler a tick costs about 20 ms; with tiers and a 12 ms budget the average stays under the budget.
void Simulated800Bots()
{
    std::mt19937 rng(20261119);
    std::vector<BotFacts> facts;
    std::vector<std::uint64_t> baseCost;
    for (std::uint64_t g = 0; g < 800; ++g)
    {
        BotFacts f = Idle(5000 + g);
        std::uint32_t const kind = std::uint32_t(g % 10);
        if (kind < 2)
        {
            f.InCombat = true;
            baseCost.push_back(40);
        }
        else if (kind < 6)
        {
            f.Moving = kind % 2 == 0;
            f.Questing = true;
            baseCost.push_back(25);
        }
        else
        {
            f.Resting = kind == 6;
            f.LifeState = kind == 8 ? 1 : kind == 9 ? 2 : 0;
            baseCost.push_back(18);
        }
        facts.push_back(f);
    }
    std::uniform_real_distribution<double> noise(0.5, 1.5);
    std::uniform_int_distribution<int> percent(0, 99);
    auto const run = [&](Settings settings, int ticks, decide_scheduler::Stats& stats, int& maxWait) {
        Harness h(settings);
        h.costOf = [&](std::size_t i) { return std::uint64_t(double(baseCost[i]) * noise(rng)); };
        std::vector<BotFacts> now = facts;
        std::uint64_t totalUs = 0;
        h.Tick(now);   // first sight, not counted
        h.scheduler.TakeStats();
        maxWait = 0;
        for (int t = 0; t < ticks; ++t)
        {
            for (BotFacts& f : now)
                if (!f.InCombat && percent(rng) < 2)
                    ++f.ActionSequence;
            std::uint64_t const before = h.nowUs;
            h.Tick(now);
            totalUs += h.nowUs - before;
            for (BotFacts const& f : now)
                maxWait = std::max(maxWait, h.scheduler.WaitedOf(f.Guid));
        }
        stats = h.scheduler.TakeStats();
        return totalUs / std::uint64_t(ticks);
    };
    decide_scheduler::Stats stats;
    int maxWait = 0;
    std::uint64_t const allUs = run(Settings{ true, 0, 1, 1, 12 }, 300, stats, maxWait);   // every bot every tick
    std::uint64_t const tieredUs = run(On(0), 300, stats, maxWait);
    CHECK(maxWait <= 6);
    std::uint64_t const budgetUs = run(On(12000), 300, stats, maxWait);
    CHECK(maxWait <= 6);
    CHECK(stats.Ran[0] == stats.Seen[0]);
    // A budget that binds (the kEvery bots alone take about 6.7 ms): the optional turns are cut and carried over, the cap still holds.
    decide_scheduler::Stats tight;
    int tightWait = 0;
    std::uint64_t const tightUs = run(On(8000), 300, tight, tightWait);
    std::printf("decide_scheduler_test: 800 bots, 8000 us budget: avg tick %llu us, deferred %llu, starved %llu, overruns %llu, max wait %d\n",
                (unsigned long long)tightUs, (unsigned long long)tight.Deferred, (unsigned long long)tight.Starved, (unsigned long long)tight.Overruns, tightWait);
    CHECK(tight.Deferred > 0);
    CHECK(tightUs <= 8000 + 100);         // at most one turn past the budget on average
    CHECK(tightWait <= 12);
    CHECK(tight.Ran[0] == tight.Seen[0]);
    std::printf("decide_scheduler_test: 800 bots, avg tick %llu us all, %llu us tiered, %llu us with a 12000 us budget (deferred %llu, overruns %llu, max wait %d)\n",
                (unsigned long long)allUs, (unsigned long long)tieredUs, (unsigned long long)budgetUs, (unsigned long long)stats.Deferred,
                (unsigned long long)stats.Overruns, maxWait);
    CHECK(allUs > 18000);                 // about 20 ms a tick without tiers (the arm C trend at 800 bots)
    CHECK(tieredUs < allUs * 6 / 10);     // tiers alone cut it by more than 40%
    CHECK(budgetUs <= 12000);             // the average under the budget
}

// The policy: off by default (no scheduler memory), on: a fighting bot runs every tick, a standing bot with nothing to do waits, an invite runs it at once,
// and a waiting bot keeps its state (it is still present).
void PolicyIntegration()
{
    {
        BotPolicy policy{ AggressiveOptions() };
        WorldTickSnapshotView view = OnMap(1);
        policy.Decide(view, 0);
        policy.Decide(view, 1000);
        CHECK(policy.SchedulerWaitedOf(10) == -1);
        CHECK(policy.TakeSchedulerLine().empty());
    }
    PolicyOptions options = AggressiveOptions();
    options.DecideScheduler = On(0, 3, 6, 12);
    std::uint64_t fakeUs = 0;
    options.DecideClockUs = [&] { return fakeUs += 7; };
    BotPolicy policy(options);
    WorldTickSnapshotView view = OnMap(1);
    std::uint64_t nowMs = 0;
    policy.Decide(view, nowMs);
    CHECK(policy.SchedulerWaitedOf(10) == 0);
    int waitedTicks = 0;
    int maxWait = 0;
    for (int t = 0; t < 12; ++t)
    {
        policy.Decide(view, nowMs += 1000);
        int const waited = policy.SchedulerWaitedOf(10);
        CHECK(waited >= 0);   // still known: present, not dropped
        waitedTicks += waited > 0;
        maxWait = std::max(maxWait, waited);
    }
    CHECK(waitedTicks > 0);
    CHECK(maxWait <= 12);
    view.bots[0].group_invite_from = MakeUnitRef(4, 1, 77);
    policy.Decide(view, nowMs += 1000);
    CHECK(policy.SchedulerWaitedOf(10) == 0);   // invite: at once
    view.bots[0].group_invite_from = 0;
    view.bots[0].in_combat = 1;
    for (int t = 0; t < 6; ++t)
    {
        policy.Decide(view, nowMs += 1000);
        CHECK(policy.SchedulerWaitedOf(10) == 0);   // combat: every tick
    }
    std::string const line = policy.TakeSchedulerLine();
    CHECK(line.find(" sched=on walk=3 idle=6 budget_us=0 tiers=every:") == 0);
    CHECK(line.find(" skipped=") != std::string::npos && line.find(" overruns=0") != std::string::npos && line.find(" max_turn_us=") != std::string::npos);
}

} // namespace

int main()
{
    TierChoice();
    OffRunsEveryBotInOrder();
    TiersRunOnTheirCadence();
    NeverSkipRules();
    BudgetCarriesOverRoundRobin();
    StarvationCap();
    StaggerSpreadsTheLoad();
    Simulated800Bots();
    PolicyIntegration();
    if (failures == 0)
        std::printf("decide_scheduler_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
