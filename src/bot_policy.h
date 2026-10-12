// BotPolicy: decides what each controlled bot should do, from one Tier 2 snapshot. Pure logic: it reads the snapshot
// and returns actions, so it is unit-tested with hand-built snapshots (tests/policy_test.cpp).
//
// This is a placeholder behaviour, not a finished bot AI: fight the threat target or the closest hostile unit in
// range, otherwise follow the closest player, otherwise stand still. Each bot gets an action only when its decision
// changes, so a bot that keeps fighting is not re-ordered every tick.
#pragma once

#include "forever_module/tier2_session.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

// Test options for the policy. TestSpell (0 = off) is cast at the bot's current target every CastEveryTicks ticks: it
// exercises the cast path and its refusal reports (milestone M1) until the class rotations exist (M2).
struct PolicyOptions
{
    std::uint32_t TestSpell = 0;
    std::uint64_t CastEveryTicks = 20;
};

// Name of an action status, for logs.
char const* ActionStatusName(forever_module::BotActionStatus status);

class BotPolicy
{
public:
    explicit BotPolicy(PolicyOptions options = {}) : _options(options) {}

    // Actions for the bots in `view`. Decisions are sent only when they change; a test cast is sent on its own timer.
    // Bots that left the view are forgotten.
    std::vector<forever_module::BotAction> Decide(forever_module::WorldTickSnapshotView const& view);

private:
    struct Decision
    {
        forever_module::ActionType Type = forever_module::ActionType::kStop;
        std::uint64_t Target = 0;
    };

    std::unordered_map<std::uint64_t, Decision> _last; // bot guid -> the decision last sent for it
    std::unordered_map<std::uint64_t, std::uint64_t> _lastCastTick; // bot guid -> tick of its last test cast
    PolicyOptions _options;
};
