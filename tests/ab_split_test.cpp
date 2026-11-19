// SPDX-License-Identifier: GPL-2.0-or-later
// ArmSplit (doc/sim-ab.md): the arm of a bot, the ArmBOverrides / Arm.B.<Key> config of arm B, the arm field of the event lines, the settings of both arms,
// and the policy taking an arm B bot's turn with arm B's options. No core needed.
#include "ab_split.h"
#include "bot_policy.h"
#include "bot_settings.h"

#include <algorithm>
#include <cstdio>

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
    if (failures == 0)
        std::printf("ab_split_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
