// Scenario (doc 19): a repeatable test situation for the bots. A fixed roster is spawned at a staging place, dressed with a given
// gear list, formed into a party, teleported into a dungeon, and let loose on a route of waypoints for a set time. The result is one
// line: how far the party got, how many died, how it ended. Pure logic over snapshots, unit-tested in tests/scenario_test.cpp.
//
// Phases, in order:
//   Waiting  until the whole roster is in the world and alive
//   Dressing one grant per bot per tick for each item of its class's gear list, then an equip service, then a pause
//   Grouping until the policy's party logic has put everyone in one party (or kGroupingTimeoutMs)
//   Entering the leader teleports to the entry; kMembersDelayMs after it arrives the members follow; all there -> Running
//   Running  the route is active; ends on the time limit, a wipe (all bots down; see WipeRecovery), or the leader reaching the last waypoint
//   Done
#pragma once

#include "forever_module/tier2_session.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct ScenarioPoint
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

struct ScenarioPlan
{
    std::string Name;
    std::uint32_t RosterSize = 5;
    std::uint32_t EntryMap = 0;
    ScenarioPoint Entry;
    std::vector<ScenarioPoint> Route;
    std::uint64_t DurationMs = 1200000;
    std::map<std::uint32_t, std::vector<std::uint32_t>> Gear; // class id -> item entries to grant and equip
    std::vector<std::pair<std::uint32_t, std::string>> Bosses; // creature entry and name of each boss; one seen dead counts as killed
    std::function<std::uint32_t(std::uint64_t)> ClassOf;      // bot guid -> class id
    // Dead-bot recovery. With WipeRecovery on, an all-down state starts a timer of WipeRecoveryMs: if a bot is alive again before it ends,
    // the wipe is cancelled and counted in Wipes; else the run ends as "wipe". Off (default): all down ends the run at once.
    bool WipeRecovery = false;
    std::uint64_t WipeRecoveryMs = 300000;
    // The run ends as "understrength" when fewer than MinAlive bots (but at least one) are alive for kUnderstrengthGraceMs (0 = off).
    std::uint32_t MinAlive = 0;
    // The run ends as "abandoned_dead_leader" when the party leader has been down this long (0 = off).
    std::uint64_t LeaderDeadAbandonMs = 0;
};

enum class ScenarioPhase
{
    Waiting,
    Dressing,
    Grouping,
    Entering,
    Running,
    Done,
};

struct ScenarioResult
{
    std::string Name;
    std::string Outcome;            // complete, wipe, timeout, enter_failed, understrength, abandoned_dead_leader
    std::uint64_t RunMs = 0;        // time spent in Running
    std::uint32_t Furthest = 0;     // route points the leader reached (0..RouteSize)
    std::uint32_t RouteSize = 0;
    std::uint32_t Deaths = 0;       // bots that were seen down (corpse or ghost) at least once
    std::uint32_t Alive = 0;        // bots with life_state 0
    std::uint32_t DeathEvents = 0;  // every alive -> down change; a bot that dies twice counts twice
    std::uint32_t Revived = 0;      // every down -> alive change (a resurrect accepted, or a corpse reclaimed)
    std::uint32_t Wipes = 0;        // all-down episodes that recovered (needs WipeRecovery)
    std::uint32_t Ghosts = 0;       // bots that are ghosts at the end
    std::uint32_t RosterSize = 0;
    bool PartyFormed = true;        // false when the grouping phase timed out
    std::uint32_t BossesTotal = 0;  // bosses the plan lists
    std::vector<std::string> BossesKilled; // names of the ones seen dead, in the order they were seen
};

constexpr std::uint64_t kDressPauseMs = 1500;
constexpr std::uint64_t kEquipPauseMs = 3000;
constexpr std::uint64_t kGroupingTimeoutMs = 120000;
constexpr std::uint64_t kMembersDelayMs = 3000;
constexpr std::uint64_t kEnterRetryMs = 10000;
constexpr std::uint64_t kEnterTimeoutMs = 90000;
constexpr std::uint64_t kUnderstrengthGraceMs = 180000; // fewer than MinAlive alive this long: the run is given up
constexpr float kRoutePointReachYards = 15.0f; // the leader counts as having reached a waypoint this close to it

class ScenarioDirector
{
public:
    explicit ScenarioDirector(ScenarioPlan plan) : _plan(std::move(plan)) {}

    // Call once per snapshot tick. Appends the director's actions for this tick.
    void Decide(forever_module::WorldTickSnapshotView const& view, std::uint64_t nowMs, std::vector<forever_module::BotAction>& actions);

    ScenarioPhase Phase() const { return _phase; }
    bool RouteActive() const { return _phase == ScenarioPhase::Running; }

    // The result, once, after the scenario has ended.
    std::optional<ScenarioResult> TakeResult();

    // Bosses seen dead since the last call, by name (for the log as they happen).
    std::vector<std::string> TakeNewBossKills();

private:
    void Finish(std::string outcome, std::uint64_t nowMs);

    ScenarioPlan _plan;
    ScenarioPhase _phase = ScenarioPhase::Waiting;
    std::uint64_t _phaseSinceMs = 0;
    bool _phaseStarted = false;
    std::map<std::uint64_t, std::size_t> _nextGrant;   // bot -> index of the next gear item to grant
    bool _equipSent = false;
    std::uint64_t _lastEnterMs = 0;
    std::uint64_t _leaderArrivedMs = 0;
    std::uint64_t _runStartMs = 0;
    std::map<std::uint64_t, bool> _everDead;
    std::map<std::uint64_t, bool> _wasDown;            // bot -> down at the last tick (a bot never seen counts as up)
    std::uint32_t _deathEvents = 0;
    std::uint32_t _revived = 0;
    std::uint32_t _wipes = 0;
    bool _allDown = false;                             // the whole party is down and the wipe recovery timer runs
    std::uint64_t _allDownSinceMs = 0;
    std::uint64_t _fewAliveSinceMs = 0;
    bool _fewAlive = false;
    std::uint64_t _leaderDownSinceMs = 0;
    bool _leaderDown = false;
    std::uint32_t _furthest = 0;
    std::vector<std::string> _bossKills;    // names, in the order seen dead
    std::size_t _bossKillsReported = 0;
    bool _partyFormed = true;
    std::optional<ScenarioResult> _result;
    bool _resultTaken = false;
    std::uint64_t _lastTick = ~0ull;
};

// One line of text for the log: "scenario <name>: outcome=... furthest=3/7 deaths=2 alive=3/5 events=3 revived=1 wipes=0 ghosts=0 run=412s".
std::string FormatScenarioResult(ScenarioResult const& result);
