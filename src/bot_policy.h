// BotPolicy: decides what each controlled bot should do, from Tier 2 snapshots. Pure logic: it reads snapshots and returns
// actions, so it is unit-tested with hand-built snapshots (tests/policy_test.cpp).
//
// Behaviour, in order: below 30% health disengage (follow the closest player, or stand still); with a target, approach it
// when it is more than 5 yards away, otherwise attack it, and cast the class rotation (combat_rotation.h) once per global
// cooldown; with no target, pull the closest hostile unless it is part of a group of more than three (over-pull control),
// else follow the closest player or stand still. A threat target is always fought. Decisions are sent when they change.
// A refused cast is retried after a while, and its spell is not used again before then. A spell the snapshot reports as on
// cooldown is skipped. Rend's bleed is tracked per target, since the snapshot has no auras, so it is not recast while active.
// Rest (M2): an idle bot below 90% health sits down, and stays sat until its health is full. It stands before it moves,
// follows or fights. A sit refused in combat is retried after a second.
// Parties (M8): with PartySize set, bots are cut into parties. The leader invites its members and a member accepts the
// invite, leaves any other party, follows its leader when it is more than 8 yards away, and fights the leader's target
// when it has none of its own. Party actions are sent only out of combat.
#pragma once

#include "forever_module/tier2_session.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// A place in the world, for travel (M6).
struct Point
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

// Options for the policy. TestSpell (0 = off) replaces the rotation: it is cast at the target every CastEveryTicks
// ticks, which exercises the cast path (milestone M1). ClassId selects the rotation (0 = none).
struct PolicyOptions
{
    std::uint32_t TestSpell = 0;
    std::uint64_t CastEveryTicks = 20;
    std::uint32_t ClassId = 0;
    // The class of each bot, when bots differ (random picks). Unset, or returning 0, falls back to ClassId.
    std::function<std::uint32_t(std::uint64_t botGuid)> ClassOf;
    // Vendor visits and trainer checks for an idle bot (M3, M4). A level-up checks the trainer at once. Zero turns that
    // service off; the module turns them on from its settings.
    std::uint64_t ServiceEveryMs = 0;
    std::uint64_t TrainEveryMs = 0;
    // Parties (M8). PartyCount parties of PartySize bots are cut from the lowest-guid bots, in guid order. Each party's
    // leader is its first warrior, else its lowest-guid bot. PartySize 0 turns parties off.
    std::uint32_t PartySize = 0;
    std::uint32_t PartyCount = 1;
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
        Point Dest;                                      // kMoveTo destination
    };

    // A damage-over-time effect the policy believes is on a target, from an accepted cast.
    struct Dot
    {
        std::uint64_t Target = 0;
        std::uint64_t UntilMs = 0;
    };

    struct BotState
    {
        std::optional<Decision> Last;                    // the last movement or attack decision sent
        bool InFight = false;
        std::uint64_t FightStartMs = 0;
        std::uint64_t NextCastMs = 0;                    // global cooldown: no rotation cast before this
        std::uint32_t LastSequence = 0;                  // last action result seen
        std::unordered_map<std::uint32_t, std::uint64_t> RetryAtMs; // spell -> time it may be cast again
        std::unordered_map<std::uint32_t, std::uint64_t> CastTarget; // spell -> target of the last cast sent for it
        std::unordered_map<std::uint32_t, Dot> Dots;     // spell -> effect on a target, while it lasts
        std::optional<std::uint64_t> LastTestCastTick;
        bool Sitting = false;                            // sat down to rest; stands before it moves or fights
        std::uint64_t RestRetryAtMs = 0;                 // a refused sit is not tried again before this
        std::uint64_t NextServiceMs = 0;                 // next vendor visit (sell grey, repair, equip)
        std::uint64_t NextTrainMs = 0;                   // next trainer check
        std::uint32_t ComboPoints = 0;                   // combo points on ComboTarget, from the bot's own Sinister Strikes
        std::uint64_t ComboTarget = 0;
        std::uint32_t LastTrainLevel = 0;                // the level at the last trainer check
        std::uint64_t PartyRetryAtMs = 0;                // a party action is not sent again before this
        bool Traveling = false;                          // walking to the vendor for a visit
        std::uint64_t TravelStartMs = 0;                 // when this walk began; it is dropped after kTravelTimeoutMs
        std::unordered_map<std::uint64_t, std::uint64_t> InvitedAtMs; // member guid -> when this bot last invited it
    };

    void ReadResult(BotState& state, forever_module::BotSnapshot const& bot, std::uint64_t nowMs);

    std::unordered_map<std::uint64_t, BotState> _bots; // bot guid -> state
    PolicyOptions _options;
};
