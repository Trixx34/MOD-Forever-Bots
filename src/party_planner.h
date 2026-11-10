// The party planner (Phase 2 of the 2026-11-10 bot AI plan): a plan for one party, made once per interval from the whole party's view, instead
// of each bot taking the first free job in guid order. Plain structs and free functions, no SDK dependency, unit-tested with hand-built input
// (tests/party_planner_test.cpp). The Turn* steps read the plan first and fall back to their own scan when there is none, so a party with the
// planner off (Module.forever-bots.PartyPlanner = 0; it is on by default) behaves as before.
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace party_planner {

struct Settings
{
    bool Enabled = false;
    std::uint64_t PlanIntervalMs = 400;     // a party's plan is recomputed at most this often
    std::uint64_t BackupDelayMs = 700;      // a backup kicker may fire only once the cast has been in view this long without a kick
    std::size_t MaxCcAssignments = 2;
};

// One party member as the planner sees it.
struct Member
{
    std::uint64_t Guid = 0;
    bool Alive = true;
    bool Tank = false;
    float X = 0.0f, Y = 0.0f;
    bool CanInterrupt = false;              // the class has an interrupt row
    bool InterruptReady = true;             // not on cooldown (the caller's NextCast / retry state)
    std::uint32_t CcSpell = 0;              // the class's crowd control spell for its level, 0 for none
    bool CcReady = true;
    std::uint64_t Holding = 0;              // the unit it holds right now (its crowd control still running), 0 for none
};

// One hostile unit in the party's merged view.
struct Enemy
{
    std::uint64_t Ref = 0;
    float X = 0.0f, Y = 0.0f;
    bool Casting = false;
    bool Engaged = false;                   // already fought by the party
    bool Protected = false;                 // never held: the dungeon's kill priority, a boss
    std::vector<std::uint32_t> CcSpells;    // the crowd control spells that may hold it (creature type, not a boss, not the kill priority, in the tank's pack)
};

struct InterruptAssignment
{
    std::uint64_t Bot = 0;
    std::uint64_t Target = 0;
    std::uint8_t Rank = 0;                  // 0 first choice, 1 backup
};

struct CcAssignment
{
    std::uint64_t Bot = 0;
    std::uint64_t Target = 0;
};

// One party's plan, kept across ticks under its leader.
struct Order
{
    std::uint64_t MainTank = 0;             // kept while the bot is alive and a member; otherwise the first tank
    std::vector<InterruptAssignment> Interrupts;
    std::vector<CcAssignment> CrowdControl;
    std::unordered_map<std::uint64_t, std::uint64_t> CastSeenMs;   // casting target -> when the plan first saw it casting
    std::uint64_t LastPlannedMs = 0;
};

// Rebuilds `order` from the party's members and the hostile units in view. A no-op (false) until the interval has passed since the last plan.
bool Plan(Settings const& settings, std::vector<Member> const& members, std::vector<Enemy> const& enemies, std::uint64_t nowMs, Order& order);

// Whether `bot` may send an interrupt at `target` now: true with no plan for that target (the claim map decides), true for its rank 0
// assignment, true for a rank 1 backup once the cast has been in view for settings.BackupDelayMs, false for a bot that is not assigned.
bool MayInterrupt(Settings const& settings, Order const* order, std::uint64_t bot, std::uint64_t target, std::uint64_t nowMs);

// Whether the plan names `bot` for this target at all (any rank): used by targeting to turn on the caster only for the assigned kickers.
bool IsAssignedInterrupt(Order const* order, std::uint64_t bot, std::uint64_t target);

// The unit a bot is to hold, 0 if the plan holds none for it.
std::uint64_t CcTargetFor(Order const* order, std::uint64_t bot);

} // namespace party_planner
