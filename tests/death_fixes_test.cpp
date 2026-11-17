// SPDX-License-Identifier: GPL-2.0-or-later
// Tests of the death fixes of 2026-11-17 (branch feature/death-fixes-20261117, doc/death-fixes.md): SoloPullHpPct, PullMaxGroup, ResurrectBackoff. Hand-built snapshots.
#include "policy_test_support.h"
#include "bot_settings.h"
#include "pull_ready.h"

#include <cstdio>

namespace {

constexpr std::uint32_t kMage = 8;
constexpr std::uint32_t kFireball = 133;

WorldTickSnapshotView MageView(std::uint64_t tick, std::uint32_t hpPct, std::vector<NearbyUnit> mobs)
{
    WorldTickSnapshotView view = View(tick, 0, std::move(mobs));
    view.bots[0].level = 10;
    view.bots[0].power_type = 0;
    view.bots[0].max_power = 100;
    view.bots[0].power = 100;
    view.bots[0].health = hpPct * 10;   // max_health 1000
    return view;
}

BotPolicy MagePolicy(std::uint32_t hpPct, std::uint32_t maxGroup, std::vector<std::string>* events = nullptr)
{
    PolicyOptions options = AggressiveOptions();
    options.ClassId = kMage;
    options.WanderWhenIdle = false;
    options.SoloPullHpPct = hpPct;
    options.PullMaxGroup = maxGroup;
    if (events)
        options.OnEvent = [events](std::string const& text) { events->push_back(text); };
    return BotPolicy(options);
}

void HelpersHold()
{
    CHECK(pull_ready::SoloRestMarkPct(0, 75, false) == 75 && pull_ready::SoloRestMarkPct(0, 75, true) == 75);   // off: the old fixed mark
    CHECK(pull_ready::SoloRestMarkPct(85, 75, false) == 85 && pull_ready::SoloRestMarkPct(85, 75, true) == 95);
    CHECK(pull_ready::SoloRestMarkPct(95, 75, true) == 100);                                                      // capped
    CHECK(pull_ready::ResurrectBackoffMs(0) == 4000 && pull_ready::ResurrectBackoffMs(1) == 8000 && pull_ready::ResurrectBackoffMs(3) == 32000);
    CHECK(pull_ready::ResurrectBackoffMs(4) == 60000 && pull_ready::ResurrectBackoffMs(40) == 60000);
}

void ASoloBotAt80PercentPullsWithTheOldMarkButRestsWithSoloPullHpPct()
{
    std::vector<NearbyUnit> const mob{ Hostile(2, 12.0f, 100) };
    BotPolicy off = MagePolicy(0, 0);
    CHECK(HasCast(off.Decide(MageView(1, 80, mob), 1000), kFireball));      // 80 is above the fixed 75: it pulls (the old behaviour)
    std::vector<std::string> events;
    BotPolicy on = MagePolicy(90, 0, &events);
    CHECK(!HasCast(on.Decide(MageView(1, 80, mob), 1000), kFireball));
    CHECK(Logged(events, "pull_wait: bot=42 reason=hp"));
}

void TheHpRestEndsAboveTheMarkPlusTheGap()
{
    std::vector<NearbyUnit> const mob{ Hostile(2, 12.0f, 100) };
    BotPolicy policy = MagePolicy(80, 0);
    CHECK(!HasCast(policy.Decide(MageView(1, 70, mob), 1000), kFireball));
    CHECK(!HasCast(policy.Decide(MageView(2, 85, mob), 2000), kFireball));   // above 80, still below 90: it keeps resting
    CHECK(HasCast(policy.Decide(MageView(3, 92, mob), 3000), kFireball));
}

void ThePackCapSkipsAPairOnlyWhenSet()
{
    std::vector<NearbyUnit> const pair{ Hostile(2, 12.0f, 100), Hostile(3, 14.0f, 100) };
    BotPolicy off = MagePolicy(0, 0);
    CHECK(HasCast(off.Decide(MageView(1, 100, pair), 1000), kFireball));     // the built-in cap is 3
    BotPolicy one = MagePolicy(0, 1);
    CHECK(!HasCast(one.Decide(MageView(1, 100, pair), 1000), kFireball));    // singles only
    BotPolicy two = MagePolicy(0, 2);
    CHECK(HasCast(two.Decide(MageView(1, 100, pair), 1000), kFireball));
    std::vector<NearbyUnit> const single{ Hostile(2, 12.0f, 100), Hostile(3, 40.0f, 100) };
    CHECK(HasCast(one.Decide(MageView(2, 100, single), 2000), kFireball));   // the second one is far from the first
}

// A ghost-side offer the core keeps refusing: count the answers sent over 100 s.
int AnswersSent(bool backoff, std::vector<std::string>* events)
{
    PolicyOptions options = AggressiveOptions();
    options.ClassId = kMage;
    options.ResurrectBackoff = backoff;
    if (events)
        options.OnEvent = [events](std::string const& text) { events->push_back(text); };
    BotPolicy policy(options);
    WorldTickSnapshotView view = View(1, 0, {});
    view.bots[0].life_state = 1;
    view.bots[0].health = 1;
    view.bots[0].resurrect_from = MakeUnitRef(2, 0, 999);
    int sent = 0;
    std::uint64_t now = 1000;
    std::uint32_t sequence = 0;
    for (int step = 0; step < 40; ++step)
    {
        for (BotAction const& action : policy.Decide(view, now))
            if (action.type == ActionType::kResurrect)
            {
                ++sent;
                view.bots[0].last_action_type = std::uint8_t(ActionType::kResurrect);
                view.bots[0].last_action_status = BotActionStatus::kOutOfRange;
                view.bots[0].last_action_sequence = ++sequence;
            }
        now += 2500;
    }
    return sent;
}

void ResurrectBackoffSendsFewerAnswers()
{
    std::vector<std::string> events;
    int const withBackoff = AnswersSent(true, &events);
    int const without = AnswersSent(false, nullptr);
    CHECK(withBackoff >= 3 && withBackoff <= 5);    // 0 s, +4, +8, +16, +32 (2.5 s ticks) in 100 s
    CHECK(withBackoff <= without);
    CHECK(Logged(events, "resurrect_backoff: bot=42"));
}

// The core reports no spell for kResurrect, so the spirit healer's "out of range" used to be logged as resurrect_accept (prod 2026-11-17: 99,670 in 2 h, none pending).
void ASpiritHealerRefusalIsNotAnAcceptRefusal()
{
    for (bool backoff : { false, true })
    {
        std::vector<std::string> events;
        PolicyOptions options = AggressiveOptions();
        options.ClassId = kMage;
        options.ResurrectBackoff = backoff;
        options.OnEvent = [&events](std::string const& text) { events.push_back(text); };
        BotPolicy policy(options);
        WorldTickSnapshotView view = View(1, 0, {});
        view.bots[0].life_state = 2;
        view.bots[0].corpse_valid = 1;
        view.bots[0].corpse_map = view.bots[0].map_id;
        view.bots[0].corpse_x = 300.0f;
        view.bots[0].last_action_type = std::uint8_t(ActionType::kResurrect);
        view.bots[0].last_action_status = BotActionStatus::kOutOfRange;
        view.bots[0].last_action_spell = 0;
        view.bots[0].last_action_sequence = 7;
        policy.Decide(view, 1000);
        CHECK(Logged(events, "death_action_refused: bot=42 action=spirit_healer") == backoff);
        CHECK(Logged(events, "death_action_refused: bot=42 action=resurrect_accept") == !backoff);
    }
}

void ALevelCapSkipsAHigherCreatureOnlyWhenSet()
{
    NearbyUnit tough = Hostile(2, 12.0f, 100);
    tough.level = 12;   // the bot is level 10: two above
    PolicyOptions options = AggressiveOptions();
    options.ClassId = kMage;
    options.WanderWhenIdle = false;
    BotPolicy off(options);
    CHECK(HasCast(off.Decide(MageView(1, 100, { tough }), 1000), kFireball));
    options.PullMaxLevelAbove = 1;
    BotPolicy one(options);
    CHECK(!HasCast(one.Decide(MageView(1, 100, { tough }), 1000), kFireball));
}

void TheSettingsKeysAreBounded()
{
    std::vector<std::string> warnings;
    BotSettings const defaults = ParseBotSettings({}, warnings);
    CHECK(defaults.SoloPullHpPct == 0 && defaults.PullMaxGroup == 0 && !defaults.ResurrectBackoff && warnings.empty());
    BotSettings const set = ParseBotSettings({ { "SoloPullHpPct", "85" }, { "PullMaxGroup", "2" }, { "ResurrectBackoff", "1" } }, warnings);
    CHECK(set.SoloPullHpPct == 85 && set.PullMaxGroup == 2 && set.ResurrectBackoff && set.PullMaxLevelAbove == 2 && warnings.empty());
    BotSettings const bad = ParseBotSettings({ { "SoloPullHpPct", "99" }, { "PullMaxGroup", "9" } }, warnings);
    CHECK(bad.SoloPullHpPct == 0 && bad.PullMaxGroup == 0 && warnings.size() == 2);
}

} // namespace

int main()
{
    HelpersHold();
    ASoloBotAt80PercentPullsWithTheOldMarkButRestsWithSoloPullHpPct();
    TheHpRestEndsAboveTheMarkPlusTheGap();
    ThePackCapSkipsAPairOnlyWhenSet();
    ResurrectBackoffSendsFewerAnswers();
    ASpiritHealerRefusalIsNotAnAcceptRefusal();
    ALevelCapSkipsAHigherCreatureOnlyWhenSet();
    TheSettingsKeysAreBounded();
    if (failures == 0)
        std::printf("death_fixes_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
