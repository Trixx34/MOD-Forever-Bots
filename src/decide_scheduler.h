// SPDX-License-Identifier: GPL-2.0-or-later
// Decide scheduler (DecideScheduler, doc/decide-scheduler.md): which bots take their full decision turn on this tick.
//
// Every bot is put in a tier by what it is doing:
//   kEvery  combat, party, dungeon, a ghost at its corpse (reviving), casting, a transport, a hostile close by: every tick, never deferred;
//   kWalk   walking, or calm questing (alive, standing, quests in the log): one tick in TierWalk;
//   kIdle   resting, idle standing, dead waiting to release, a ghost on its corpse run: one tick in TierIdle.
// A bot of a slower tier is due on the ticks its guid's stagger picks (so the load of a tier is spread over its N ticks), and is always run
// at once when it has a reaction pending: death or any change of life state, the end of a walk, a new action result (a refusal included),
// a group invite, a resurrect offer, loot, a trade or a module request (the caller says). A bot never waits more than MaxSkipTicks ticks.
//
// The time budget (BudgetUs, 0 = off): the bots that must run run first (they are never deferred and count against the budget); the due bots
// of the slower tiers then run, the longest waiting first and then round-robin from a cursor, while the time spent on this tick is below the
// budget. A due bot that did not fit is carried over: it stays due and is first in line on the next tick, and the starvation cap makes it a
// must-run once it has waited MaxSkipTicks ticks.
//
// Pure logic: no core, no clock of its own. The caller passes a microsecond clock and the function that runs one bot's turn.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace decide_scheduler
{

enum class Tier : std::uint8_t
{
    kEvery = 0,
    kWalk = 1,
    kIdle = 2,
};
constexpr std::size_t kTierCount = 3;
char const* TierName(Tier tier);

struct Settings
{
    bool Enabled = false;            // DecideScheduler (0 = off: every bot every tick, as before)
    std::uint32_t BudgetUs = 0;      // DecideBudgetUs: per-tick time for the optional turns (0 = no budget)
    std::uint32_t TierWalk = 3;      // DecideTierWalk: a walking or calm questing bot runs one tick in this many (1..10)
    std::uint32_t TierIdle = 6;      // DecideTierIdle: a resting, idle, dead or corpse-running bot runs one tick in this many (1..20)
    std::uint32_t MaxSkipTicks = 12; // DecideMaxSkipTicks: no bot waits more ticks than this (1..60)
};

// What the caller knows about one bot on this tick. The snapshot facts and the policy's own facts go in; the scheduler keeps the rest.
struct BotFacts
{
    std::uint64_t Guid = 0;
    // kEvery facts
    bool InCombat = false;        // in_combat, a threat target, or the policy's fight bookkeeping
    bool Party = false;           // a group, tank lead, a follow order
    bool Dungeon = false;         // on a dungeon map, a route or a staging hold
    bool GhostReviving = false;   // a ghost at its corpse (reclaim range) or with a resurrect offer
    bool Busy = false;            // casting, on a transport, ground effects or telegraphs, a forced heal or LoS move: needs every tick
    bool HostileNear = false;     // a living hostile unit close by
    // tier facts
    std::uint8_t LifeState = 0;   // 0 alive, 1 dead (corpse), 2 ghost
    bool Moving = false;
    bool Questing = false;        // quests in the log (a calm questing bot)
    bool Resting = false;         // sat down to rest or eating / drinking
    // reactions: run at once, whatever the tier
    std::uint32_t ActionSequence = 0;  // last_action_sequence: a new result (a refusal, an apply) is a reaction
    bool LastRefused = false;          // the last result is a refusal (counted; a new sequence already forces the turn)
    bool PendingReaction = false;      // group invite, resurrect offer, loot waiting, trade, gear or guild request, duel, gift, rest done
};

// The tier of a bot from its facts (no memory).
Tier TierOf(BotFacts const& facts);

// Deterministic phase of a guid in 0..every-1 (splitmix64 of the guid). A bot of a tier with N ticks is due when (tick + phase) % N == 0.
std::uint32_t StaggerOf(std::uint64_t guid, std::uint32_t every);

// Counts since the last TakeStats (the pump line).
struct Stats
{
    std::uint64_t Ticks = 0;
    std::uint64_t Seen[kTierCount] = {};    // bot-ticks seen per tier
    std::uint64_t Ran[kTierCount] = {};     // turns run per tier
    std::uint64_t Skipped = 0;              // turns not run because the bot was not due
    std::uint64_t Deferred = 0;             // due turns carried over by the budget
    std::uint64_t Forced = 0;               // turns forced by a reaction (new result, death, end of walk, pending reaction)
    std::uint64_t Starved = 0;              // turns forced by the starvation cap
    std::uint64_t Overruns = 0;             // ticks whose turns took longer than the budget
    std::uint64_t MaxTurnUs = 0;            // the longest single turn
    std::uint64_t MaxTickUs = 0;            // the longest tick (all turns)
    std::uint64_t TotalTickUs = 0;          // all ticks, for an average
};

// " sched=on walk=3 idle=6 budget_us=B tiers=every:S/R,walk:S/R,idle:S/R skipped=K deferred=D forced=F starved=X overruns=O max_turn_us=M max_tick_us=T"
// (S bot-ticks seen, R turns run). Empty when the scheduler is off.
std::string FormatStats(Settings const& settings, Stats const& stats);

class Scheduler
{
public:
    explicit Scheduler(Settings settings = {}) : _settings(settings) {}

    Settings const& Options() const { return _settings; }

    // One tick. `facts[i]` is bot i of the view. `run(i)` runs bot i's full turn, `skip(i)` is told about a bot that does not run (the caller
    // still has to keep it present). `clockUs()` is a monotonic clock in microseconds (only read when the scheduler is on).
    // Off: every bot runs, in view order, and nothing is counted.
    // On: the must-run bots run first in view order, then the due bots in the budget's order; the rest are skipped.
    void Tick(std::span<BotFacts const> facts, std::function<void(std::size_t)> const& run, std::function<void(std::size_t)> const& skip,
              std::function<std::uint64_t()> const& clockUs);

    // Counts since the last call, then zero.
    Stats TakeStats();
    Stats const& PeekStats() const { return _stats; }

    // Ticks since the bot last ran (0 = ran on the last tick); -1 when unknown. For the tests.
    int WaitedOf(std::uint64_t guid) const;
    std::uint64_t TickNumber() const { return _tick; }

private:
    struct Memory
    {
        std::uint32_t Waited = 0;          // ticks since the last turn
        std::uint32_t Sequence = 0;        // ActionSequence at the last turn
        std::uint8_t LifeState = 0;        // at the last turn
        bool Moving = false;               // on the last tick (run or not)
        bool Carry = false;                // due, deferred by the budget
        std::uint64_t SeenTick = 0;        // the tick it was last in the view
    };

    // Why the bot runs this tick: 0 not due, 1 due (optional), 2 must (tier kEvery or reaction), 3 must (starvation cap).
    int Need(BotFacts const& facts, Memory const* memory, Tier tier, bool& reaction) const;
    std::uint32_t EveryOf(Tier tier) const;

    Settings _settings;
    std::unordered_map<std::uint64_t, Memory> _memory;
    std::uint64_t _tick = 0;
    std::size_t _cursor = 0;               // round-robin start among the optional bots
    Stats _stats;
};

} // namespace decide_scheduler
