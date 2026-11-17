// SPDX-License-Identifier: GPL-2.0-or-later
// Tests of the bot AI plan items of the 2026-11-16 overnight job (branch feature/ai-plan-next): per-target damage-over-time tracking, the solo ready gate, the buff gate,
// the target switch. Hand-built snapshots, no core needed.
#include "policy_test_support.h"
#include "bot_settings.h"
#include "pull_ready.h"

#include <cstdio>

namespace {

constexpr std::uint32_t kWarlock = 9;
constexpr std::uint32_t kCorruption = 172;

std::uint64_t MobRef(std::uint64_t counter)
{
    return MakeUnitRef(8, 1, counter);
}

BotPolicy WarlockPolicy(bool perTarget)
{
    PolicyOptions options = AggressiveOptions();
    options.ClassId = kWarlock;
    options.WanderWhenIdle = false;
    options.DotPerTarget = perTarget;
    return BotPolicy(options);
}

// The warlock fights mob 2, switches to mob 3, and is back on mob 2 with Corruption (18 s) still running on it.
bool RecastsCorruptionOnTheFirstTarget(bool perTarget)
{
    BotPolicy policy = WarlockPolicy(perTarget);
    std::vector<NearbyUnit> const mobs{ Hostile(2, 12.0f, 100), Hostile(3, 14.0f, 100) };
    CHECK(CastTargetOf(policy.Decide(View(1, MobRef(2), mobs), 1000), kCorruption) == MobRef(2));
    WorldTickSnapshotView landed = View(2, MobRef(2), mobs);
    Result(landed, kCorruption, BotActionStatus::kApplied, 1);
    policy.Decide(landed, 2600);
    CHECK(CastTargetOf(policy.Decide(View(3, MobRef(3), mobs), 5000), kCorruption) == MobRef(3));
    WorldTickSnapshotView landedB = View(4, MobRef(3), mobs);
    Result(landedB, kCorruption, BotActionStatus::kApplied, 2);
    policy.Decide(landedB, 6600);
    return CastTargetOf(policy.Decide(View(5, MobRef(2), mobs), 9000), kCorruption) == MobRef(2);
}

void DotPerTargetKeepsBothEffects()
{
    CHECK(RecastsCorruptionOnTheFirstTarget(false));    // one slot per spell: back on mob 2 it is cast again (the old behaviour, bug 14)
    CHECK(!RecastsCorruptionOnTheFirstTarget(true));    // per (spell, target): mob 2 still has it
}

void DotPerTargetStillRecastsAfterItRunsOut()
{
    BotPolicy policy = WarlockPolicy(true);
    std::vector<NearbyUnit> const mobs{ Hostile(2, 12.0f, 100) };
    policy.Decide(View(1, MobRef(2), mobs), 1000);
    WorldTickSnapshotView landed = View(2, MobRef(2), mobs);
    Result(landed, kCorruption, BotActionStatus::kApplied, 1);
    policy.Decide(landed, 2600);
    CHECK(CastTargetOf(policy.Decide(View(3, MobRef(2), mobs), 8000), kCorruption) == 0);              // 18 s not over
    CHECK(CastTargetOf(policy.Decide(View(4, MobRef(2), mobs), 21000), kCorruption) == MobRef(2));     // over at 20.6 s
}

// ---- the solo ready gate ----

constexpr std::uint32_t kMage = 8;
constexpr std::uint32_t kFireball = 133;

WorldTickSnapshotView MageView(std::uint64_t tick, std::uint32_t manaPct)
{
    WorldTickSnapshotView view = View(tick, 0, { Hostile(2, 12.0f, 100) });
    view.bots[0].level = 10;
    view.bots[0].power_type = 0;
    view.bots[0].max_power = 100;
    view.bots[0].power = manaPct;
    return view;
}

BotPolicy MagePolicy(std::uint32_t soloManaPct, std::vector<std::string>* events = nullptr)
{
    PolicyOptions options = AggressiveOptions();
    options.ClassId = kMage;
    options.WanderWhenIdle = false;
    options.SoloPullManaPct = soloManaPct;
    options.PullWaitMaxMs = 60000;
    if (events)
        options.OnEvent = [events](std::string const& text) { events->push_back(text); };
    return BotPolicy(options);
}

void ASoloMageLowOnManaWaitsBeforeItPulls()
{
    std::vector<std::string> events;
    BotPolicy policy = MagePolicy(50, &events);
    auto const actions = policy.Decide(MageView(1, 20), 1000);
    CHECK(!HasCast(actions, kFireball));
    CHECK(Logged(events, "pull_wait: bot=42 reason=mana"));
    // A creature close by (12 yards) makes it walk away first; with none within 30 yards it sits down to drink.
    CHECK(!GroundMoves(actions).empty() || FindAction(actions, ActionType::kMoveTo).has_value());
    WorldTickSnapshotView far = MageView(2, 20);
    far.nearby[0] = Hostile(2, 35.0f, 100);
    auto const sat = policy.Decide(far, 2000);
    std::uint32_t spell = 0;
    CHECK(!HasCast(sat, kFireball) && RestAt(sat, spell) >= 0 && spell == 1);
}

void ASoloMageWithEnoughManaPullsAtOnce()
{
    BotPolicy policy = MagePolicy(50);
    CHECK(HasCast(policy.Decide(MageView(1, 60), 1000), kFireball));
    BotPolicy off = MagePolicy(0);
    CHECK(HasCast(off.Decide(MageView(1, 20), 1000), kFireball));      // key off: as before
}

void TheManaWaitEndsAboveTheMarkPlusTheGap()
{
    BotPolicy policy = MagePolicy(50);
    CHECK(!HasCast(policy.Decide(MageView(1, 40), 1000), kFireball));
    CHECK(!HasCast(policy.Decide(MageView(2, 65), 2000), kFireball));  // above the mark, still below mark + 20: it keeps drinking
    CHECK(HasCast(policy.Decide(MageView(3, 75), 3000), kFireball));
}

void TheManaWaitTimesOutAfterTheLimit()
{
    std::vector<std::string> events;
    BotPolicy policy = MagePolicy(50, &events);
    CHECK(!HasCast(policy.Decide(MageView(1, 20), 1000), kFireball));
    CHECK(!HasCast(policy.Decide(MageView(2, 20), 50000), kFireball));
    CHECK(HasCast(policy.Decide(MageView(3, 20), 61500), kFireball));  // 60 s: it goes
    CHECK(Logged(events, "pull_wait_timeout: bot=42 reason=mana"));
    CHECK(HasCast(policy.Decide(MageView(4, 20), 70000), kFireball));  // and does not wait again while it is still short
    CHECK(HasCast(policy.Decide(MageView(6, 90), 72000), kFireball));  // the need was over once
    CHECK(!HasCast(policy.Decide(MageView(7, 20), 73000), kFireball)); // a new wait
}

// ---- the buff gate ----

PolicyOptions DruidOptions(bool gate, std::vector<std::string>* events = nullptr)
{
    PolicyOptions options = BuffOptions();   // class 11: Wrath (5176) in the fight rows, Mark of the Wild (1126, 1800 s) as an upkeep buff
    options.PassingBuff.Enabled = false;
    options.WanderWhenIdle = false;
    options.PullBuffGate = gate;
    options.PullWaitMaxMs = 60000;
    if (events)
        options.OnEvent = [events](std::string const& text) { events->push_back(text); };
    return options;
}

WorldTickSnapshotView DruidView(std::uint64_t tick, bool marked)
{
    WorldTickSnapshotView view = View(tick, 0, { Hostile(2, 12.0f, 100) });
    view.bots[0].level = 10;
    view.bots[0].power_type = 0;
    view.bots[0].max_power = 5000;
    view.bots[0].power = 5000;
    if (marked)
        view.bots[0].auras[view.bots[0].aura_count++] = forever_module::AuraEntry{ 1126, 900000, 1 };
    return view;
}

void ASoloDruidBuffsBeforeItPulls()
{
    std::vector<std::string> events;
    BotPolicy policy(DruidOptions(true, &events));
    auto const actions = policy.Decide(DruidView(1, false), 1000);
    CHECK(!HasCast(actions, 5176));
    CHECK(HasCast(actions, 1126));
    CHECK(Logged(events, "pull_wait: bot=42 reason=buff"));
    BotPolicy marked(DruidOptions(true));
    CHECK(HasCast(marked.Decide(DruidView(1, true), 1000), 5176));      // buffed: it pulls
    BotPolicy off(DruidOptions(false));
    CHECK(HasCast(off.Decide(DruidView(1, false), 1000), 5176));        // gate off: it pulls without the buff
}

void TheBuffWaitTimesOut()
{
    std::vector<std::string> events;
    BotPolicy policy(DruidOptions(true, &events));
    CHECK(!HasCast(policy.Decide(DruidView(1, false), 1000), 5176));
    CHECK(!HasCast(policy.Decide(DruidView(2, false), 40000), 5176));
    CHECK(HasCast(policy.Decide(DruidView(3, false), 62000), 5176));    // the buff never shows on the bot: after 60 s it pulls anyway
    CHECK(Logged(events, "pull_wait_timeout: bot=42 reason=buff"));
}

void TheSettingsKeysAreBounded()
{
    std::vector<std::string> warnings;
    BotSettings const defaults = ParseBotSettings({}, warnings);
    CHECK(!defaults.DotPerTarget && defaults.SoloPullManaPct == 0 && !defaults.PullBuffGate && defaults.PullWaitMaxMs == 60000 && warnings.empty());
    BotSettings const set = ParseBotSettings({ { "DotPerTarget", "1" }, { "SoloPullManaPct", "50" }, { "PullBuffGate", "1" }, { "PullWaitMaxMs", "30000" } }, warnings);
    CHECK(set.DotPerTarget && set.SoloPullManaPct == 50 && set.PullBuffGate && set.PullWaitMaxMs == 30000 && warnings.empty());
    BotSettings const bad = ParseBotSettings({ { "SoloPullManaPct", "95" }, { "PullWaitMaxMs", "10" } }, warnings);
    CHECK(bad.SoloPullManaPct == 0 && bad.PullWaitMaxMs == 60000 && warnings.size() == 2);   // out of range: the default stays, with a warning
}

void PullReadyHelpersHold()
{
    pull_ready::Gate gate;
    bool timedOut = false;
    CHECK(pull_ready::Hold(gate, true, 1000, 5000, timedOut) && !timedOut);
    CHECK(pull_ready::Hold(gate, true, 5999, 5000, timedOut));
    CHECK(!pull_ready::Hold(gate, true, 6000, 5000, timedOut) && timedOut);
    CHECK(!pull_ready::Hold(gate, true, 7000, 5000, timedOut) && !timedOut && gate.GaveUp);
    CHECK(!pull_ready::Hold(gate, false, 8000, 5000, timedOut) && !gate.GaveUp && gate.SinceMs == 0);
    CHECK(!pull_ready::ManaNeeded(10, 100, 0, false) && !pull_ready::ManaNeeded(10, 0, 50, false));   // off, or no mana pool
    CHECK(pull_ready::ManaNeeded(49, 100, 50, false) && !pull_ready::ManaNeeded(50, 100, 50, false));
    CHECK(pull_ready::ManaNeeded(69, 100, 50, true) && !pull_ready::ManaNeeded(70, 100, 50, true));
    CHECK(!pull_ready::ManaNeeded(100, 100, 90, true));                                                // the release mark is capped at 100
}

} // namespace

int main()
{
    DotPerTargetKeepsBothEffects();
    DotPerTargetStillRecastsAfterItRunsOut();
    PullReadyHelpersHold();
    ASoloMageLowOnManaWaitsBeforeItPulls();
    ASoloMageWithEnoughManaPullsAtOnce();
    TheManaWaitEndsAboveTheMarkPlusTheGap();
    TheManaWaitTimesOutAfterTheLimit();
    ASoloDruidBuffsBeforeItPulls();
    TheBuffWaitTimesOut();
    TheSettingsKeysAreBounded();
    if (failures == 0)
        std::printf("ai_next_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
