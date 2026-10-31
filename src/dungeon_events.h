// SPDX-License-Identifier: GPL-2.0-or-later
// Dungeon events: the things a party must do to get on in a dungeon, kept in one table for every dungeon (data/dungeon_events.txt), not in code per
// dungeon. A door that only a lever opens, a cannon fired with a charge of powder to blow another, a gate that opens when a boss dies. The leader of a
// dungeon party performs an event as a short list of steps: walk to a place, click an object (or use an item on it), wait. Pure logic with no SDK
// dependency, unit-tested in tests/dungeon_events_test.cpp; BotPolicy sends the movement and the kUseObject actions the runner asks for.
//
//   event <map> <id> <name...>      starts an event; <id> is a word, <name> free text
//   after_boss <entry>              trigger: once that boss is dead (the creature entry)
//   near <x> <y> <z> <radius>       trigger: when the leader comes within <radius> yards (2D) of the place
//   after_bosses <entry> <entry> ...  trigger: once every one of those bosses is dead (Wailing Caverns: the four Lords of the Fang)
//   at_start                        trigger: at once, when the dungeon run starts
//   use <object entry> <x> <y> <z> [item <item entry>]   step: walk to within 4 yards of the place, then click the object (nothing happens to one that
//                                   is open already) or, with `item`, use that item on it
//   talk <creature entry> <option> <x> <y> <z>   step: walk to within 4 yards of the place, then talk to the creature and pick gossip option <option>
//                                   (counted from 0), which starts a scripted event such as an escort (kGossip, SDK 0.24)
//   follow <creature entry> <seconds>   step: follow that creature (an escort walking to its goal) for that long; a fight in the meantime holds the step
//   teleport <x> <y> <z>            step: put the leader at the place (a scenario tool: DungeonTeleport, for a place the navmesh does not connect)
//   wait <seconds>                  step: stand still
//   timeout <seconds>               the event is given up after this long (default 120)
//   end
//
// Several events can have the same trigger; they run one after the other in file order. An event runs once per dungeon run.
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace dungeon_events {

struct Step
{
    enum class Kind : std::uint8_t { Use, Wait, Talk, Follow, Teleport };
    Kind What = Kind::Use;
    std::uint32_t Creature = 0;   // Talk, Follow: the creature's entry
    std::uint32_t Option = 0;     // Talk: the gossip option, counted from 0
    std::uint32_t Object = 0;     // Use: the object's entry
    std::uint32_t Item = 0;       // Use: 0 = a click, else an item entry used on the object
    float X = 0.0f, Y = 0.0f, Z = 0.0f;
    std::uint32_t WaitSec = 0;    // Wait, Follow
};

struct Event
{
    enum class Trigger : std::uint8_t { AtStart, AfterBoss, Near, AfterAllBosses };
    std::uint32_t Map = 0;
    std::string Id;
    std::string Name;
    Trigger When = Trigger::AtStart;
    std::uint32_t Boss = 0;
    std::vector<std::uint32_t> Bosses;   // AfterAllBosses: every one of these must be dead
    float X = 0.0f, Y = 0.0f, Z = 0.0f, Radius = 0.0f;
    std::vector<Step> Steps;
    std::uint32_t TimeoutSec = 120;
};

// Parses the file. A block with a problem (no steps, no trigger, a bad number) is dropped with a line in `warnings`; the others stay.
std::vector<Event> Parse(std::string_view text, std::vector<std::string>& warnings);

// The events of one map.
std::vector<Event> ForMap(std::vector<Event> const& all, std::uint32_t map);

struct Facts
{
    float X = 0.0f, Y = 0.0f, Z = 0.0f;
    bool InCombat = false;
    bool Alive = true;
    std::set<std::uint32_t> const* KilledBosses = nullptr;
};

// What the leader should do now because of an event.
struct Command
{
    enum class What : std::uint8_t
    {
        Nothing,    // no event is running: the route goes on
        Hold,       // an event is running but nothing is to be sent now: stand still
        MoveTo,     // walk to (X, Y, Z)
        Use,        // send kUseObject: Object, Item, with the place (X, Y, Z)
        Talk,       // send kGossip: Creature, Option, with the place (X, Y, Z)
        Follow,     // follow the creature Creature (kFollow); stand still when it is not in view
        Teleport,   // put the leader at (X, Y, Z)
    };
    What Do = What::Nothing;
    float X = 0.0f, Y = 0.0f, Z = 0.0f;
    std::uint32_t Object = 0;
    std::uint32_t Item = 0;
    std::uint32_t Creature = 0;
    std::uint32_t Option = 0;
};

// The result of a kUseObject or kGossip that was sent.
enum class UseResult : std::uint8_t { Applied, OutOfRange, Failed };

class Runner
{
public:
    Runner() = default;
    explicit Runner(std::vector<Event> events)
        : _events(std::move(events)), _done(_events.size(), false), _retryAfter(_events.size(), 0), _attempts(_events.size(), 0)
    {}

    bool Empty() const { return _events.empty(); }

    // One step of the clock. `nowMs` increases.
    Command Next(Facts const& facts, std::uint64_t nowMs);

    // The answer to the last Use command (the policy reads it from the bot's next snapshot).
    void OnUseResult(UseResult result, std::uint64_t nowMs);

    bool Active() const { return _active >= 0; }

    // What happened since the last call, one line each (event_start / event_done / event_failed_retry / event_timeout_retry / event_blocked): for the log.
    std::vector<std::string> TakeLog();

private:
    void Finish(std::string const& how, std::uint64_t nowMs, bool success);

    std::vector<Event> _events;
    std::vector<bool> _done;
    std::vector<std::uint64_t> _retryAfter;   // per-event: Next() skips it until nowMs reaches this (0 = may fire now)
    std::vector<std::uint32_t> _attempts;     // per-event: how many failed/timed-out cycles so far; kMaxEventAttemptCycles gives up for good
    int _active = -1;                  // index of the running event
    std::size_t _step = 0;
    std::uint64_t _startedMs = 0;
    std::uint64_t _stepSinceMs = 0;
    bool _sent = false;                // the Use of this step was sent and its answer is awaited
    std::uint64_t _sentMs = 0;
    std::uint32_t _failures = 0;
    bool _close = false;               // after "out of range": come closer than usual
    std::vector<std::string> _log;
};

constexpr float kApproachYards = 4.0f;
constexpr float kCloseYards = 1.5f;
constexpr std::uint64_t kAnswerWaitMs = 3000;
constexpr std::uint32_t kMaxFailures = 3;
constexpr std::uint32_t kMaxEventAttemptCycles = 5;   // full restarts of one event before it is given up for good (event_blocked)
constexpr std::uint64_t kEventRetryBackoffMs = 20000; // wait this long before an event that failed or timed out may start again

} // namespace dungeon_events
