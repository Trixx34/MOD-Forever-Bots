// SPDX-License-Identifier: GPL-2.0-or-later
// Run telemetry of a dungeon or raid scenario (doc/dungeon-strategy.md, "Telemetry"): the pure parts. One party_tl: line per party every 30 s of scenario time, a
// party_stalled: line when a party made no progress for 300 s (with the dominant cause), and boss_attempt: lines around a boss fight. The ScenarioDirector fills the
// structs and queues the lines (ScenarioDirector::TakeTelemetry); the pump logs them as events and appends them to <RunTelemetryDir>/run-telemetry.log.
// Every line is "<kind>: key=value key=value ...", in a fixed field order (new fields are appended at the end).
#pragma once

#include <cstdint>
#include <fstream>
#include <string>

namespace run_telemetry
{

constexpr std::uint64_t kTimelineEveryMs = 30000; // one party_tl: per party this often (scenario time)
constexpr std::uint64_t kStallAfterMs = 300000;   // no progress for this long is a stall; the verdict is repeated at this interval
constexpr std::uint64_t kAttemptEndIdleMs = 10000; // a boss attempt ends (aborted) after this long with nobody in a fight
constexpr std::uint64_t kFlushEveryMs = 30000;    // the run file is flushed this often
// Thresholds of StallCause, per no-progress window.
constexpr std::uint32_t kChurnPulls = 10;          // fight starts: "many pull events"
constexpr std::uint32_t kRefusalsMin = 5;          // refused or no-route moves of the leader
constexpr std::uint64_t kShareDenominator = 5;     // resting and dead time count from 2/5 (40 %) of the window

// What the director counted since the party last made progress (or since the last verdict).
struct StallCounters
{
    std::uint32_t Pulls = 0;         // fight starts: a member going from out of combat into combat (cf. NotePullChurn, bot_policy_units.cpp)
    std::uint32_t RouteRefusals = 0; // moves of the leader the core refused or found no route for
    std::uint64_t RestMs = 0;        // time in the rest state (a member below the ready health or mana: the regroup wait)
    std::uint64_t DeadMs = 0;        // time in the dead state (a member down, party not fighting or walking)
    std::uint64_t FightMs = 0;
    std::uint64_t WindowMs = 0;      // how long the window is
};

// The dominant cause of a stall, a pure function of the counters: "dead_members", "fight_churn", "route_refusals", "resting" or "unknown".
// Each candidate gets a score (its counter over its threshold); the highest score of 1 or more wins, ties in the order of that list.
std::string StallCause(StallCounters const& counters);

// The 300 s no-progress rule. Progress is a boss kill or a route point advanced.
class StallTracker
{
public:
    void Start(std::uint64_t nowMs);
    void NoteProgress(std::uint64_t nowMs);
    // True once per kStallAfterMs without progress: the first time kStallAfterMs after the last progress, then every kStallAfterMs after the last verdict.
    bool Check(std::uint64_t nowMs);
    std::uint32_t Count() const { return _count; }
    std::uint64_t LastProgressMs() const { return _progressMs; }

private:
    std::uint64_t _progressMs = 0;
    std::uint64_t _verdictMs = 0;
    std::uint32_t _count = 0;
};

struct PartyId
{
    std::string Name;        // the scenario name as the director has it: "dm#2", or "dm" for a single party
    std::string Variant;     // reserved for a later strategy variant: empty for now
};

struct Progress
{
    bool IsBoss = false;     // the last progress was a boss kill (Name), else a route point (Point)
    std::string Name;
    std::uint32_t Point = 0;
    std::uint64_t AtS = 0;   // seconds into the run
};

struct Timeline
{
    std::uint64_t TS = 0;
    std::uint64_t Leader = 0;
    std::uint32_t Map = 0;
    float X = 0, Y = 0, Z = 0;
    std::string Step;        // fight, walk, dead, rest, far, idle (the time states of the result line)
    std::uint32_t Point = 0, Points = 0;
    std::uint32_t Target = 0;
    std::uint32_t Alive = 0, Total = 0, InFight = 0;
    Progress Last;
};

struct Stall
{
    std::uint64_t TS = 0;
    std::string Cause;
    std::uint64_t NoProgressS = 0;
    StallCounters Counters;
    std::uint32_t Count = 0;
    std::uint32_t Point = 0, Points = 0;
    Progress Last;
};

struct BossAttempt
{
    std::uint64_t TS = 0;
    std::string Boss;
    bool End = false;
    std::uint64_t DurS = 0;
    std::uint32_t Deaths = 0;
    std::string Outcome;     // "open" at the start, "killed" or "aborted" at the end
};

// "party_tl: scenario=dm party=2 variant= t_s=120 leader=5 map=36 pos=-14.5,-380.2,62.0 step=walk route=3/40 target=0 alive=5/5 fight=0 last=boss:Rhahk'Zor@100"
std::string FormatTimeline(PartyId const& id, Timeline const& line);
// "party_stalled: scenario=dm party=2 variant= t_s=900 cause=resting no_progress_s=300 pulls=2 route_refusals=0 rest_s=210 dead_s=0 fight_s=40 stalls=1 route=3/40 last=route:3@600"
std::string FormatStall(PartyId const& id, Stall const& line);
// "boss_attempt: scenario=dm party=2 variant= t_s=640 boss=Mr._Smite stage=end dur_s=80 deaths=1 outcome=killed"
std::string FormatBossAttempt(PartyId const& id, BossAttempt const& line);
// "scenario=dm party=2" from "dm#2" (party=1 for a name without a number): the fields every line starts with.
void SplitName(std::string const& name, std::string& scenario, std::uint32_t& party);
// "stalled=N last_stall=<cause>@<t_s>" for the result line; empty when there was no stall.
std::string FormatResultFields(std::uint32_t stalls, std::string const& lastCause, std::uint64_t lastTS);

// The run file <dir>/run-telemetry.log: opened once (lazily), plain buffered appends, flushed every kFlushEveryMs, never throws. A directory that cannot be opened
// gives one warning (through `warn`) and the file stays off.
class RunFile
{
public:
    explicit RunFile(std::string dir) : _dir(std::move(dir)) {}
    bool Enabled() const { return !_dir.empty(); }
    // Appends "<ISO time> <line>\n". nowMs is the pump's clock (for the flush), epochSeconds the wall clock (for the prefix).
    void Append(std::string const& line, std::uint64_t nowMs, std::int64_t epochSeconds, std::string* warn = nullptr) noexcept;
    void Flush(std::uint64_t nowMs) noexcept; // flushes when kFlushEveryMs have passed since the last flush
    static std::string IsoTime(std::int64_t epochSeconds);

private:
    std::string _dir;
    std::ofstream _out;
    bool _tried = false;
    std::uint64_t _flushedMs = 0;
    bool _dirty = false;
};

} // namespace run_telemetry
