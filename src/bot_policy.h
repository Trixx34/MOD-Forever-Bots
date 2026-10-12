// BotPolicy: decides what each controlled bot should do, from Tier 2 snapshots. Pure logic: it reads snapshots and returns
// actions, so it is unit-tested with hand-built snapshots (tests/policy_test.cpp).
//
// Behaviour, in order: below 30% health disengage (follow the closest player, or stand still); with a target, approach it
// when it is more than 5 yards away, otherwise attack it, and cast the class rotation (combat_rotation.h) once per global
// cooldown; with no target, follow the closest player or stand still. Decisions are sent when they change. A refused cast
// is retried after a while, and its spell is not used again before then.
#pragma once

#include "forever_module/tier2_session.h"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Options for the policy. TestSpell (0 = off) replaces the rotation: it is cast at the target every CastEveryTicks
// ticks, which exercises the cast path (milestone M1). ClassId selects the rotation (0 = none).
struct PolicyOptions
{
    std::uint32_t TestSpell = 0;
    std::uint64_t CastEveryTicks = 20;
    std::uint32_t ClassId = 0;
};

// Name of an action status, for logs.
char const* ActionStatusName(forever_module::BotActionStatus status);

class BotPolicy
{
public:
    explicit BotPolicy(PolicyOptions options = {}) : _options(options) {}

    // Actions for the bots in `view`. nowMs is a monotonic clock in milliseconds (cooldowns, retries, fight time).
    // Decisions are sent only when they change; casts follow the rotation, the global cooldown and the retries.
    std::vector<forever_module::BotAction> Decide(forever_module::WorldTickSnapshotView const& view, std::uint64_t nowMs = 0);

private:
    struct Decision
    {
        forever_module::ActionType Type = forever_module::ActionType::kStop;
        std::uint64_t Target = 0;
    };

    struct BotState
    {
        std::optional<Decision> Last;                    // the last movement or attack decision sent
        bool InFight = false;
        std::uint64_t FightStartMs = 0;
        std::uint64_t NextCastMs = 0;                    // global cooldown: no rotation cast before this
        std::uint32_t LastSequence = 0;                  // last action result seen
        std::unordered_map<std::uint32_t, std::uint64_t> RetryAtMs; // spell -> time it may be cast again
        std::optional<std::uint64_t> LastTestCastTick;
    };

    void ReadResult(BotState& state, forever_module::BotSnapshot const& bot, std::uint64_t nowMs);

    std::unordered_map<std::uint64_t, BotState> _bots; // bot guid -> state
    PolicyOptions _options;
};
