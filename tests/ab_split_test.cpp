// SPDX-License-Identifier: GPL-2.0-or-later
// ArmSplit (doc/sim-ab.md): the arm of a bot, the ArmBOverrides / Arm.B.<Key> config of arm B, the arm field of the event lines, the settings of both arms,
// and the policy taking an arm B bot's turn with arm B's options. No core needed.
#include "ab_split.h"
#include "bot_policy.h"
#include "bot_settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>

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

bool Has(std::vector<std::string> const& lines, std::string const& part)
{
    return std::any_of(lines.begin(), lines.end(), [&](std::string const& line) { return line.find(part) != std::string::npos; });
}

void ArmByParity()
{
    using ab_split::Arm;
    CHECK(ab_split::ArmOf(false, 235) == Arm::A);   // split off: every bot is arm A
    CHECK(ab_split::ArmOf(true, 66) == Arm::A);
    CHECK(ab_split::ArmOf(true, 235) == Arm::B);
    CHECK(ab_split::ArmLetter(Arm::A) == 'A' && ab_split::ArmLetter(Arm::B) == 'B');
    // About half each over a run of guids.
    std::size_t b = 0;
    for (std::uint64_t guid = 1; guid <= 400; ++guid)
        b += ab_split::ArmOf(true, guid) == Arm::B ? 1 : 0;
    CHECK(b == 200);
}

void OverrideList()
{
    std::vector<std::string> warnings;
    auto const pairs = ab_split::ParseOverrides(" PathVariety=1, LevelSafeWalks = 0,,Broken, =3,GrindFile=\"data/b.txt\"", warnings);
    CHECK(pairs.size() == 3);
    if (pairs.size() == 3)
    {
        CHECK(pairs[0].first == "PathVariety" && pairs[0].second == "1");
        CHECK(pairs[1].first == "LevelSafeWalks" && pairs[1].second == "0");
        CHECK(pairs[2].first == "GrindFile" && pairs[2].second == "data/b.txt");
    }
    CHECK(warnings.size() == 2);   // "Broken" and "=3"
    std::vector<std::string> none;
    CHECK(ab_split::ParseOverrides("", none).empty() && none.empty());
}

void ArmBConfigApplies()
{
    std::unordered_map<std::string, std::string> const config = {
        { "PathVariety", "0" },
        { "LevelSafeWalks", "1" },
        { "ArmSplit", "1" },
        { "ArmBOverrides", "PathVariety=1,LevelSafeWalks=0,ArmSplit=0,TraceBots=5" },
        { "Arm.B.LevelSafeWalks", "1" },   // the key wins over the list
        { "Arm.B.Supplies", "1" },         // not set for arm A: default
    };
    std::vector<std::string> warnings;
    std::vector<std::string> applied;
    auto const b = ab_split::ArmBConfig(config, warnings, &applied);
    CHECK(b.at("PathVariety") == "1");
    CHECK(b.at("LevelSafeWalks") == "1");
    CHECK(b.at("Supplies") == "1");
    CHECK(b.at("ArmSplit") == "1");   // protected: not overridden
    CHECK(b.count("TraceBots") == 0);
    CHECK(warnings.size() == 2);      // ArmSplit and TraceBots refused
    CHECK(Has(applied, "PathVariety=1 (A: 0)"));
    CHECK(Has(applied, "Supplies=1 (A: default)"));
    CHECK(config.at("PathVariety") == "0");   // the original is untouched
}

void GuidList()
{
    std::vector<std::string> warnings;
    auto const guids = ab_split::ParseGuidList("235,66 x, 0 ,  7", warnings);
    CHECK(guids.size() == 3 && guids.count(235) && guids.count(66) && guids.count(7));
    CHECK(warnings.size() == 2);   // "x" and "0"
}

void EventTag()
{
    CHECK(ab_split::BotOfLine("died: bot=235 life=1 map=0") == 235);
    CHECK(ab_split::BotOfLine("boss_kill: map=1") == 0);
    CHECK(ab_split::BotOfLine("x: bot=abc") == 0);
    ab_split::SetTagging(false);
    CHECK(ab_split::Tag("died: bot=235 map=0") == "died: bot=235 map=0");
    CHECK(ab_split::ArmField(235).empty());
    ab_split::SetTagging(true);
    CHECK(ab_split::Tag("died: bot=235 map=0") == "died: bot=235 map=0 arm=B");
    CHECK(ab_split::Tag("ghost: bot=66 map=0") == "ghost: bot=66 map=0 arm=A");
    CHECK(ab_split::Tag("tank_lead_off: map=1") == "tank_lead_off: map=1");
    CHECK(ab_split::ArmField(66) == " arm=A");
    ab_split::SetTagging(false);
}

void SettingsBothArms()
{
    {
        std::vector<std::string> warnings;
        BotSettings const off = ParseBotSettings({ { "PathVariety", "0" } }, warnings);
        CHECK(!off.ArmSplit && !off.ArmB && off.TraceBots.empty() && off.TraceDir.empty() && off.TraceMaxMB == 20);
    }
    {
        std::vector<std::string> warnings;
        BotSettings const on = ParseBotSettings({ { "ArmSplit", "1" }, { "ArmBOverrides", "PathVariety=1,DeathAreaCount=4" }, { "DeathAreaCount", "2" } }, warnings);
        CHECK(on.ArmSplit && on.ArmB);
        if (on.ArmB)
        {
            CHECK(on.ArmB->PathVariety.Enabled && !on.PathVariety.Enabled);
            CHECK(on.ArmB->DeathAreaCount == 4 && on.DeathAreaCount == 2);
            CHECK(!on.ArmB->ArmSplit && !on.ArmB->ArmB);
        }
        CHECK(on.ArmBApplied.size() == 2);
        CHECK(warnings.empty());
    }
    {
        // An A=B run: the split on, no override; arm B is a copy.
        std::vector<std::string> warnings;
        BotSettings const same = ParseBotSettings({ { "ArmSplit", "1" }, { "DeathAreaCount", "3" } }, warnings);
        CHECK(same.ArmB && same.ArmB->DeathAreaCount == 3 && same.ArmBApplied.empty());
    }
    {
        // A bad arm B value warns once, marked arm B; arm A's own warnings are not repeated.
        std::vector<std::string> warnings;
        BotSettings const bad = ParseBotSettings({ { "ArmSplit", "1" }, { "Arm.B.DeathAreaCount", "abc" }, { "TraceMaxMB", "0" } }, warnings);
        CHECK(bad.ArmB && bad.TraceMaxMB == 20);
        CHECK(Has(warnings, "arm B: Module.forever-bots.DeathAreaCount"));
        CHECK(std::count_if(warnings.begin(), warnings.end(), [](std::string const& w) { return w.find("TraceMaxMB") != std::string::npos; }) == 1);
    }
    {
        std::vector<std::string> warnings;
        BotSettings const trace = ParseBotSettings({ { "TraceBots", "235,66" }, { "TraceDir", "/tmp/x" }, { "TraceMaxMB", "5" } }, warnings);
        CHECK(trace.TraceBots.size() == 2 && trace.TraceDir == "/tmp/x" && trace.TraceMaxMB == 5);
    }
}

PolicyOptions Aggressive()
{
    PolicyOptions options;
    options.AggressiveStart = true;
    options.HealPlan.Enabled = false;
    return options;
}

// Two bots side by side, each with one hostile at 5 yards (its own nearby slot).
WorldTickSnapshotView TwoBots(std::uint64_t tick, std::uint64_t a, std::uint64_t b)
{
    WorldTickSnapshotView view;
    view.tick_number = tick;
    view.max_nearby_units = 4;
    view.nearby.resize(8);
    for (std::uint64_t guid : { a, b })
    {
        BotSnapshot bot;
        bot.guid = guid;
        bot.health = 1000;
        bot.max_health = 1000;
        bot.nearby_count = 1;
        view.bots.push_back(bot);
    }
    for (std::size_t i = 0; i < 2; ++i)
    {
        NearbyUnit unit;
        unit.guid = MakeUnitRef(8, 1, 2 + i);
        unit.pos_x = 5.0f;
        unit.health = 100;
        unit.is_hostile = true;
        view.nearby[i * view.max_nearby_units] = unit;
    }
    return view;
}

bool CastsTestSpell(std::vector<BotAction> const& actions, std::uint64_t guid)
{
    return std::any_of(actions.begin(), actions.end(), [guid](BotAction const& a) { return a.bot_guid == guid && a.type == ActionType::kCastSpell && a.spell_id == 585; });
}

void PolicyTakesArmBOptionsForOddGuids()
{
    PolicyOptions a = Aggressive();
    PolicyOptions b = Aggressive();
    b.TestSpell = 585;
    b.CastEveryTicks = 20;
    {
        // Split off: arm A's options for both.
        BotPolicy policy(a);
        auto const actions = policy.Decide(TwoBots(1, 42, 43), 1000);
        CHECK(!CastsTestSpell(actions, 42) && !CastsTestSpell(actions, 43));
        CHECK(!policy.ArmSplitOn());
    }
    {
        BotPolicy policy(a);
        policy.SetArmB(b);
        CHECK(policy.ArmSplitOn());
        auto const actions = policy.Decide(TwoBots(1, 42, 43), 1000);
        CHECK(!CastsTestSpell(actions, 42));   // even guid: arm A, no test spell
        CHECK(CastsTestSpell(actions, 43));    // odd guid: arm B's TestSpell
        auto const timing = policy.TakeArmTiming();
        CHECK(timing[0].Ticks == 1 && timing[0].BotTurns == 1 && timing[1].BotTurns == 1);
        CHECK(policy.TakeArmTiming()[0].Ticks == 0);   // taken once
    }
}

void DeathsTopPerArm()
{
    PolicyOptions a = Aggressive();
    a.DeathsTopEveryMs = 60000;
    BotPolicy policy(a);
    PolicyOptions b = a;
    policy.SetArmB(b);
    policy.Decide(TwoBots(1, 42, 43), 1000);
    CHECK(!policy.TakeDeathsTopLine(1000));   // starts the clock
    CHECK(policy.TakeArmDeathsTopLines().empty());
    auto const line = policy.TakeDeathsTopLine(61000);
    CHECK(line && line->rfind("deaths_top: window_min=1 deaths=0 bots_online=2", 0) == 0);
    auto const arms = policy.TakeArmDeathsTopLines();
    CHECK(arms.size() == 2);
    if (arms.size() == 2)
    {
        CHECK(arms[0].rfind("deaths_top: arm=A window_min=1 deaths=0 bots_online=1", 0) == 0);
        CHECK(arms[1].rfind("deaths_top: arm=B window_min=1 deaths=0 bots_online=1", 0) == 0);
    }
    CHECK(policy.TakeArmDeathsTopLines().empty());
}

// The survival and decide scheduler keys are policy options: ArmBOverrides sets them for arm B only, arm A keeps the base values (the arm D2 A/B, doc/sim-arm-d2.md).
void SurvivalAndSchedulerKeysPerArm()
{
    std::vector<std::string> warnings;
    BotSettings const on = ParseBotSettings({ { "ArmSplit", "1" }, { "ArmBOverrides", "SafeRevive=1,FightRiskRetreat=1,DangerMap=1,DecideScheduler=1" },
                                              { "Arm.B.FightRiskTeleport", "1" }, { "Arm.B.DecideBudgetUs", "4000" } },
                                            warnings);
    CHECK(warnings.empty());
    CHECK(on.ArmSplit && on.ArmB);
    if (!on.ArmB)
        return;
    BotSettings const& b = *on.ArmB;
    CHECK(b.SafeRevive && b.FightRiskRetreat && b.DangerMap && b.DecideScheduler && b.FightRiskTeleport && b.DecideBudgetUs == 4000);
    // The keys not overridden keep their defaults in arm B as well.
    CHECK(b.SafeReviveLevelMargin == 3 && b.FightRiskMarginPct == 100 && b.DangerMapDeathsPerHour == 6 && b.DangerMapDecayMin == 120);
    CHECK(b.DecideTierWalk == 3 && b.DecideTierIdle == 6 && b.DecideMaxSkipTicks == 12);
    // Arm A: the base values (all off).
    CHECK(!on.SafeRevive && !on.FightRiskRetreat && !on.DangerMap && !on.DecideScheduler && !on.FightRiskTeleport && on.DecideBudgetUs == 0);
    CHECK(on.ArmBApplied.size() == 6);
}

// Two bots standing idle, nothing near: the scheduler's idle tier.
WorldTickSnapshotView TwoIdleBots(std::uint64_t tick, std::uint64_t a, std::uint64_t b)
{
    WorldTickSnapshotView view;
    view.tick_number = tick;
    view.max_nearby_units = 4;
    view.nearby.resize(8);
    for (std::uint64_t guid : { a, b })
    {
        BotSnapshot bot;
        bot.guid = guid;
        bot.health = 1000;
        bot.max_health = 1000;
        bot.level = 10;
        view.bots.push_back(bot);
    }
    return view;
}

// DecideScheduler per arm: each arm's scheduler runs over its own bots with its own keys; an arm with the scheduler off runs its bots every tick.
void SchedulerPerArm()
{
    for (bool armB : { true, false })
    {
        PolicyOptions a = Aggressive();
        a.WanderWhenIdle = false;
        a.DecideClockUs = [] { return std::uint64_t(0); };
        PolicyOptions b = a;
        PolicyOptions& on = armB ? b : a;
        on.DecideScheduler.Enabled = true;
        on.DecideScheduler.TierIdle = 6;
        BotPolicy policy(a);
        policy.SetArmB(b);
        std::uint64_t const scheduled = armB ? 43 : 42;
        std::uint64_t const every = armB ? 42 : 43;
        int maxWaited = 0;
        for (std::uint64_t tick = 1; tick <= 8; ++tick)
        {
            policy.Decide(TwoIdleBots(tick, 42, 43), 1000 * tick);
            maxWaited = std::max(maxWaited, policy.SchedulerWaitedOf(scheduled));
            CHECK(policy.SchedulerWaitedOf(every) == -1);   // its arm's scheduler is off: no memory, it runs every tick
        }
        CHECK(maxWaited > 0);
        std::string const onLine = policy.TakeSchedulerLine(armB ? 1 : 0);
        CHECK(onLine.rfind(" sched=on walk=3 idle=6", 0) == 0);
        CHECK(onLine.find("idle:8/") != std::string::npos);   // one bot seen on 8 ticks
        CHECK(policy.TakeSchedulerLine(armB ? 0 : 1).empty());
    }
}

// ---- ArmSplitMode = strata (the 2026-11-20 A=B calibration: guid parity put 1,670 Skyborne deaths in arm A against 1,049 in arm B) ----

// 400 bots, Skyborne heavy (races 95/96 about 45 %), mixed classes, guids shuffled.
std::vector<ab_split::Strata::Bot> SyntheticBots(unsigned seed)
{
    std::vector<ab_split::Strata::Bot> bots;
    std::mt19937 rng(seed);
    std::uint32_t const races[] = { 95, 96, 95, 96, 95, 96, 95, 96, 95, 1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 4 };
    std::uint32_t const classes[] = { 1, 2, 3, 4, 5, 7, 8, 9, 11 };
    for (std::uint64_t guid = 1000; guid < 1400; ++guid)
        bots.push_back({ guid, races[rng() % 20], classes[rng() % 9] });
    std::shuffle(bots.begin(), bots.end(), rng);
    return bots;
}

long Gap(std::array<std::size_t, 2> const& count) { return std::labs(long(count[0]) - long(count[1])); }

bool Balanced(ab_split::Strata const& strata)
{
    bool ok = Gap(strata.Totals()) <= 1;
    for (auto const& [key, count] : strata.PerStratum())
        ok = ok && Gap(count) <= 1;
    return ok;
}

void StrataBalancedOneBatch()
{
    using ab_split::Arm;
    std::vector<ab_split::Strata::Bot> const bots = SyntheticBots(20261120);
    ab_split::Strata strata;
    strata.Assign(bots);
    auto const totals = strata.Totals();
    CHECK(totals[0] + totals[1] == 400);
    CHECK(Gap(totals) <= 1);
    CHECK(Balanced(strata));
    // Skyborne (95 and 96) is one group per class: the 95 and 96 bots of a class share a stratum.
    CHECK(ab_split::Strata::KeyOf(95, 4) == ab_split::Strata::KeyOf(96, 4));
    CHECK(ab_split::Strata::KeyOf(1, 4) != ab_split::Strata::KeyOf(3, 4) && ab_split::Strata::KeyOf(1, 4) != ab_split::Strata::KeyOf(1, 8));
    // Skyborne as a whole: within one bot per class stratum, so within the number of classes.
    std::array<std::size_t, 2> skyborne{};
    for (auto const& bot : bots)
    {
        Arm arm = Arm::A;
        CHECK(strata.Find(bot.Guid, arm));
        if (bot.Race == 95 || bot.Race == 96)
            ++skyborne[std::size_t(arm)];
    }
    CHECK(skyborne[0] + skyborne[1] > 150 && Gap(skyborne) <= 9);
    // Stable: the same batch again, a single call, a bot back with other facts: no arm changes, no counts change.
    std::unordered_map<std::uint64_t, Arm> before;
    for (auto const& bot : bots)
        strata.Find(bot.Guid, before[bot.Guid]);
    strata.Assign(bots);
    for (auto const& bot : bots)
    {
        CHECK(strata.Assign(bot.Guid, bot.Race, bot.Class) == before[bot.Guid]);
        CHECK(strata.Assign(bot.Guid, 1, 1) == before[bot.Guid]);
    }
    CHECK(strata.Totals() == totals);
    // Order independent: the same bots in another order give the same arms.
    std::vector<ab_split::Strata::Bot> reordered = bots;
    std::reverse(reordered.begin(), reordered.end());
    ab_split::Strata other;
    other.Assign(reordered);
    for (auto const& bot : bots)
    {
        Arm arm = Arm::A;
        CHECK(other.Find(bot.Guid, arm) && arm == before[bot.Guid]);
    }
}

// Bots that come in small batches (logins over time): every stratum stays within one bot after every batch.
void StrataBalancedInBatches()
{
    std::vector<ab_split::Strata::Bot> const bots = SyntheticBots(7);
    ab_split::Strata strata;
    std::mt19937 rng(11);
    std::size_t at = 0;
    bool always = true;
    while (at < bots.size())
    {
        std::size_t const n = std::min<std::size_t>(1 + rng() % 12, bots.size() - at);
        strata.Assign(std::vector<ab_split::Strata::Bot>(bots.begin() + long(at), bots.begin() + long(at + n)));
        at += n;
        for (auto const& [key, count] : strata.PerStratum())
            always = always && Gap(count) <= 1;
    }
    CHECK(always);
    auto const totals = strata.Totals();
    CHECK(totals[0] + totals[1] == 400);
    CHECK(Gap(totals) <= 2);
}

// The process-wide mode: guid mode is the parity split as before; strata mode reads the stored arm (parity for a bot not seen yet).
void ArmOfByMode()
{
    using ab_split::Arm;
    CHECK(ab_split::Mode() == ab_split::SplitMode::Guid);   // the default
    ab_split::SplitMode mode = ab_split::SplitMode::Guid;
    CHECK(ab_split::ParseSplitMode("strata", mode) && mode == ab_split::SplitMode::Strata);
    CHECK(ab_split::ParseSplitMode(" guid ", mode) && mode == ab_split::SplitMode::Guid);
    CHECK(!ab_split::ParseSplitMode("race", mode) && mode == ab_split::SplitMode::Guid);
    ab_split::ProcessStrata().Clear();
    ab_split::ProcessStrata().Assign(std::vector<ab_split::Strata::Bot>{ { 10, 95, 1 }, { 12, 95, 1 }, { 21, 1, 1 }, { 23, 1, 1 } });
    // Guid mode ignores the strata.
    for (std::uint64_t guid : { 10, 12, 21, 23, 99 })
        CHECK(ab_split::ArmOf(true, guid) == ab_split::ParityArm(guid));
    ab_split::SetMode(ab_split::SplitMode::Strata);
    CHECK(ab_split::ArmOf(true, 10) == Arm::A && ab_split::ArmOf(true, 12) == Arm::B);   // one per arm, in guid order
    CHECK(ab_split::ArmOf(true, 21) == Arm::A && ab_split::ArmOf(true, 23) == Arm::B);
    CHECK(ab_split::ArmOf(true, 99) == Arm::B && ab_split::ArmOf(true, 98) == Arm::A);   // not seen yet: parity
    CHECK(ab_split::ArmOf(false, 12) == Arm::A);                                         // split off: arm A
    ab_split::SetTagging(true);
    CHECK(ab_split::ArmField(12) == " arm=B" && ab_split::Tag("death: bot=10 map=2991") == "death: bot=10 map=2991 arm=A");
    ab_split::SetTagging(false);
    ab_split::SetMode(ab_split::SplitMode::Guid);
    ab_split::ProcessStrata().Clear();
}

// The policy assigns the arms of the bots it sees first (one batch per tick) and an arm B bot takes arm B's options: two even guids of one stratum.
void PolicyAssignsStrataAtFirstSight()
{
    PolicyOptions a = Aggressive();
    a.RaceOf = [](std::uint64_t) { return std::uint32_t(95); };
    a.ClassOf = [](std::uint64_t) { return std::uint32_t(1); };
    PolicyOptions b = a;
    b.TestSpell = 585;
    b.CastEveryTicks = 20;
    ab_split::ProcessStrata().Clear();
    ab_split::SetMode(ab_split::SplitMode::Strata);
    {
        BotPolicy policy(a);
        policy.SetArmB(b);
        auto const actions = policy.Decide(TwoBots(1, 44, 42), 1000);
        CHECK(!CastsTestSpell(actions, 42));   // lower guid of the stratum: arm A
        CHECK(CastsTestSpell(actions, 44));    // even guid, yet arm B
        auto const timing = policy.TakeArmTiming();
        CHECK(timing[0].BotTurns == 1 && timing[1].BotTurns == 1);
        ab_split::Arm arm = ab_split::Arm::A;
        CHECK(ab_split::ProcessStrata().Find(44, arm) && arm == ab_split::Arm::B);
    }
    // Without the split the policy assigns nothing.
    ab_split::ProcessStrata().Clear();
    {
        BotPolicy policy(a);
        policy.Decide(TwoBots(1, 44, 42), 1000);
        CHECK(ab_split::ProcessStrata().Totals()[0] + ab_split::ProcessStrata().Totals()[1] == 0);
    }
    ab_split::SetMode(ab_split::SplitMode::Guid);
    ab_split::ProcessStrata().Clear();
}

void SettingsSplitMode()
{
    std::vector<std::string> warnings;
    CHECK(ParseBotSettings({ { "ArmSplit", "1" } }, warnings).ArmSplitMode == ab_split::SplitMode::Guid);   // default
    BotSettings const on = ParseBotSettings({ { "ArmSplit", "1" }, { "ArmSplitMode", "strata" }, { "ArmBOverrides", "ArmSplitMode=guid" } }, warnings);
    CHECK(on.ArmSplitMode == ab_split::SplitMode::Strata);
    CHECK(warnings.size() == 1);   // arm B cannot change the split mode (Arm* keys are protected)
    warnings.clear();
    CHECK(ParseBotSettings({ { "ArmSplitMode", "race" } }, warnings).ArmSplitMode == ab_split::SplitMode::Guid);
    CHECK(warnings.size() == 1);
}

} // namespace

int main()
{
    ArmByParity();
    OverrideList();
    ArmBConfigApplies();
    GuidList();
    EventTag();
    SettingsBothArms();
    PolicyTakesArmBOptionsForOddGuids();
    DeathsTopPerArm();
    SurvivalAndSchedulerKeysPerArm();
    SchedulerPerArm();
    StrataBalancedOneBatch();
    StrataBalancedInBatches();
    ArmOfByMode();
    PolicyAssignsStrataAtFirstSight();
    SettingsSplitMode();
    if (failures == 0)
        std::printf("ab_split_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
