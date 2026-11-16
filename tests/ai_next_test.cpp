// SPDX-License-Identifier: GPL-2.0-or-later
// Tests of the bot AI plan items of the 2026-11-16 overnight job (branch feature/ai-plan-next). Hand-built snapshots, no core needed.
#include "policy_test_support.h"
#include "bot_settings.h"

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

void DotPerTargetSettings()
{
    std::vector<std::string> warnings;
    CHECK(!ParseBotSettings({}, warnings).DotPerTarget && warnings.empty());
    CHECK(ParseBotSettings({ { "DotPerTarget", "1" } }, warnings).DotPerTarget && warnings.empty());
}

} // namespace

int main()
{
    DotPerTargetKeepsBothEffects();
    DotPerTargetStillRecastsAfterItRunsOut();
    DotPerTargetSettings();
    if (failures == 0)
        std::printf("ai_next_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
