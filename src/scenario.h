// Scenario (doc 19): a repeatable test situation for the bots. A fixed roster is spawned at a staging place, dressed with a given
// gear list, formed into a party, teleported into a dungeon, and let loose on a route of waypoints for a set time. The result is one
// line: how far the party got, how many died, how it ended. Pure logic over snapshots, unit-tested in tests/scenario_test.cpp.
//
// Phases, in order:
//   Waiting  until the whole roster is in the world and alive
//   Dressing one grant per bot per tick for each item of its class's gear list, then an equip service, then a pause. Ammunition and
//            supplies (food, drink) are granted and kept topped up for the whole run from here on, alongside the phases, not gating them.
//   Grouping until the policy's party logic has put everyone in one party (or kGroupingTimeoutMs)
//   Entering the leader teleports to the entry; kMembersDelayMs after it arrives the members follow; all there -> Running
//   Running  the route is active; ends on the time limit, a wipe (all bots down; see WipeRecovery), or the leader reaching the last waypoint
//   Done
#pragma once

#include "forever_module/tier2_session.h"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
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
    // Item entry and count granted to every bot after its gear: index 0 is food, index 1 is drink (the order the config doc asks for; an
    // index beyond that is granted once, in the dressing, and not tracked further -- the snapshot has no bag count for it to watch). The
    // first grant of food and drink is in the dressing, like gear; after that each is kept topped up for the whole run the same way as
    // ammunition (see AmmoCount below) -- a bot with fewer than half of its index's count is given that many more of it (BotSnapshot's
    // food_count or drink_count says how many it carries; there is no per-item-entry count), at most kSupplyGrantsMax times and never
    // closer together than kSupplyRetryMs. A grant that does not raise the total, kSupplyUselessMax times running, stops that one of the
    // two being retried; the other is unaffected.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> Supplies;
    // Per class, after the gear: items (a warlock's Soul Shards, which the Voidwalker and Succubus summons use up) and class quests completed with their
    // reward (core quest operation 4, the quests that teach the pets' spells). A quest the core refuses (not in the log, wrong race) is not tried again.
    std::map<std::uint32_t, std::vector<std::pair<std::uint32_t, std::uint32_t>>> ClassSupplies;
    std::map<std::uint32_t, std::vector<std::uint32_t>> ClassQuests;
    // A hunter whose ranged weapon takes ammunition and who carries none is given this many of the ammunition the snapshot names (ammo_entry), in every phase
    // from the dressing on, and again when the count falls below kAmmoLow (an hour of Auto Shot is about 1800 shots: 40 grants of a stack are 8000). 0 = no ammunition is granted.
    // A grant is one stack at most, kAmmoStackMax (200): the core refuses a larger count.
    std::uint32_t AmmoCount = 200;
    std::vector<std::pair<std::uint32_t, std::string>> Bosses; // creature entry and name of each boss; one seen dead counts as killed
    std::function<std::uint32_t(std::uint64_t)> ClassOf;      // bot guid -> class id
    std::function<std::uint32_t(std::uint64_t)> RaceOf;       // bot guid -> race id (for the roster in the result line)
    // Dead-bot recovery. With WipeRecovery on, an all-down state starts a timer of WipeRecoveryMs: if a bot is alive again before it ends,
    // the wipe is cancelled and counted in Wipes; else the run ends as "wipe". Off (default): all down ends the run at once.
    bool WipeRecovery = false;
    std::uint64_t WipeRecoveryMs = 300000;
    // The run ends as "understrength" when fewer than MinAlive bots (but at least one) are alive for kUnderstrengthGraceMs (0 = off).
    std::uint32_t MinAlive = 0;
    // The run ends as "abandoned_dead_leader" when the party leader has been down this long (0 = off).
    std::uint64_t LeaderDeadAbandonMs = 0;
    // A dungeon clear (doc/dungeons.md): with RequireBosses on (and bosses listed) the run is "complete" when every boss has been seen dead,
    // not when the leader touches the last waypoint; a leader at the last waypoint with bosses still alive gets kBossGraceMs more, then the
    // run ends as "bosses_missing".
    bool RequireBosses = false;
    // Taming (the Taming phase, between the dressing and the grouping): the hunters of the roster that have no pet are taken to TameSpot (map TameMap), where the policy walks them to a
    // beast and tames it, and taken back to where they stood. The phase ends when every such hunter has a pet or after kTameTimeoutMs; a hunter that failed stays in the party without one.
    bool TameEnabled = false;
    std::uint32_t TameMap = 0;
    ScenarioPoint TameSpot;
};

enum class ScenarioPhase
{
    Waiting,
    Dressing,
    Taming,
    Grouping,
    Entering,
    Running,
    Done,
};

struct ScenarioResult
{
    std::string Name;
    std::string Outcome;            // complete, wipe, timeout, enter_failed, understrength, abandoned_dead_leader, bosses_missing
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
    std::vector<std::pair<std::uint32_t, std::uint32_t>> Party; // race and class id of each member, in the order first seen
};

constexpr std::uint64_t kDressPauseMs = 1500;
constexpr std::uint64_t kEquipPauseMs = 3000;
constexpr std::uint64_t kTameTimeoutMs = 300000;    // the Taming phase gives the hunters this long to get a pet
constexpr std::uint64_t kTameRetryMs = 6000;        // a teleport that was refused (in combat) is sent again after this
constexpr std::uint64_t kTameBackMs = 20000;        // the hunters are given this long to be back before the grouping starts anyway
constexpr float kTameArrivedYards = 150.0f;         // a hunter this close to the spot has arrived
constexpr float kTameHomeYards = 40.0f;             // and one this close to where it stood is back
constexpr std::uint32_t kTameMinLevel = 10;         // the hunter pet quest is given at level 10
constexpr std::uint32_t kAmmoLow = 100;             // below this many pieces of its ammunition a hunter is given more
constexpr std::uint64_t kAmmoRetryMs = 5000;        // a bot is given ammunition at most this often (the snapshot needs a moment to show it)
constexpr std::uint32_t kAmmoGrantsMax = 40;        // per bot and run
constexpr std::uint32_t kAmmoStackMax = 200;        // a grant is at most one stack (the core refuses a count above the item's stack size: 200 for arrows and shot)
constexpr std::uint32_t kAmmoUselessMax = 3;        // grants in a row after which the count did not grow: the bot is left alone (the core refuses for another reason)
constexpr std::uint64_t kSupplyRetryMs = 5000;      // a bot is given more of a supply item at most this often (the snapshot needs a moment to show it)
constexpr std::uint32_t kSupplyGrantsMax = 40;       // per bot and item, per run (on top of the dressing's first grant)
constexpr std::uint32_t kSupplyUselessMax = 3;       // grants in a row after which neither total grew: the bot is left alone for that item
constexpr std::uint64_t kGroupingTimeoutMs = 120000;
constexpr std::uint64_t kMembersDelayMs = 3000;
constexpr std::uint64_t kEnterRetryMs = 10000;
constexpr std::uint64_t kEnterTimeoutMs = 90000;
// Before the party enters it rests at its staging place until every living bot has this much health (and mana, for mana users), or this long has passed: the gear
// the dressing phase puts on raises the maximum health to two or three times the current health, so a party that entered at once started at 35-45 %.
constexpr std::uint32_t kRecoverPct = 90;
constexpr std::uint64_t kRecoverMaxMs = 240000;
// After a wipe the revived party stands outside at the graveyard: this long after the leader is up there, the living members are put back at the entry (a corpse run
// through the portal does the same on foot), and again every kEnterRetryMs while some are still outside.
constexpr std::uint64_t kReenterDelayMs = 20000;
constexpr std::uint64_t kUnderstrengthGraceMs = 180000; // fewer than MinAlive alive this long: the run is given up
constexpr std::uint64_t kBossGraceMs = 120000;          // RequireBosses: time allowed after the last waypoint for the remaining bosses to die
constexpr float kRoutePointReachYards = 15.0f; // the leader counts as having reached a waypoint this close to it
// ...and only this far above or below it (a different storey of the same tower is not "reached"). Shadowfang Keep's own route (2026-10-27) puts its
// courtyard-floor waypoints (points 4, 9, 10: around the Rethilgore gate) within kRoutePointReachYards in X/Y of its topmost waypoints (points 35, 36:
// Odo the Blindwatcher, a storey above, on the tower roof) with only 9.3 yards of Z between them: a bot standing at the gate, nowhere near the roof,
// was credited with "reached" the dungeon's last point this way (bots 0.36.2 and earlier: furthest=37/37, bosses=0/5). 8 yards keeps that pair apart
// while staying well above the under-one-yard Z gaps between a waypoint and the very next one that are meant to count as the same spot.
constexpr float kRouteReachMaxZYards = 8.0f;

// The part of a snapshot that holds the bots in `keep` (guids), with their nearby slots: the director of one party of several sees only its own bots. The nearby units are
// stored per bot position (max_nearby_units slots each), so they are copied along with the bots, else a director would read another party's creatures.
forever_module::WorldTickSnapshotView SubsetView(forever_module::WorldTickSnapshotView const& view, std::function<bool(std::uint64_t)> const& keep);

class ScenarioDirector
{
public:
    explicit ScenarioDirector(ScenarioPlan plan) : _plan(std::move(plan)) {}

    // Call once per snapshot tick. Appends the director's actions for this tick.
    void Decide(forever_module::WorldTickSnapshotView const& view, std::uint64_t nowMs, std::vector<forever_module::BotAction>& actions);

    ScenarioPhase Phase() const { return _phase; }
    bool Taming() const { return _phase == ScenarioPhase::Taming; }
    bool RouteActive() const { return _phase == ScenarioPhase::Running; }

    // The result, once, after the scenario has ended.
    std::optional<ScenarioResult> TakeResult();

    // Bosses seen dead since the last call, by name (for the log as they happen).
    std::vector<std::string> TakeNewBossKills();

    // The creature entries of the bosses seen dead so far.
    std::set<std::uint32_t> const& KilledBossEntries() const { return _bossKilledEntries; }

private:
    void Finish(std::string outcome, std::uint64_t nowMs);

    ScenarioPlan _plan;
    ScenarioPhase _phase = ScenarioPhase::Waiting;
    std::uint64_t _phaseSinceMs = 0;
    bool _phaseStarted = false;
    std::map<std::uint64_t, std::size_t> _nextGrant;   // bot -> index of the next gear item to grant
    std::map<std::uint64_t, std::size_t> _nextSupply;  // bot -> index of the next supply beyond food and drink (index 2+) to grant once
    std::map<std::uint64_t, std::size_t> _nextClassSupply; // bot -> index of the next class supply to grant
    std::map<std::uint64_t, std::size_t> _nextClassQuest;  // bot -> index of the next class quest to send
    std::map<std::uint64_t, std::uint64_t> _ammoAtMs;      // bot -> when it was last given ammunition
    std::map<std::uint64_t, std::uint32_t> _ammoGrants;    // bot -> how often
    std::map<std::uint64_t, std::uint32_t> _ammoSeen;      // bot -> its ammunition count when it was last given some
    std::map<std::uint64_t, std::uint32_t> _ammoPeak;      // bot -> the highest count it showed since then
    std::map<std::uint64_t, std::uint32_t> _ammoUseless;   // bot -> grants in a row that did not raise its count
    // Supplies 0 (food) and 1 (drink) of _plan.Supplies, kept topped up the same way as ammunition: these are the only two the snapshot
    // gives a bag count for (BotSnapshot::food_count, drink_count; a count per item entry does not exist). Keyed by (bot guid, index) so
    // that one of the two having bag trouble never stops the other being granted to the same bot.
    using SupplyKey = std::pair<std::uint64_t, std::size_t>;
    std::map<SupplyKey, std::uint64_t> _supplyAtMs;     // when the item was last granted
    std::map<SupplyKey, std::uint32_t> _supplyGrants;   // how often
    std::map<SupplyKey, std::uint32_t> _supplySeen;     // its total (food_count or drink_count) when it was last granted
    std::map<SupplyKey, std::uint32_t> _supplyPeak;     // the highest total shown since then
    std::map<SupplyKey, std::uint32_t> _supplyUseless;  // grants in a row that did not raise it
    struct Home { std::uint32_t Map = 0; float X = 0, Y = 0, Z = 0; };
    std::map<std::uint64_t, Home> _tameHome;               // hunter -> where it stood when the Taming phase began
    std::map<std::uint64_t, std::uint64_t> _tameSentAtMs;  // hunter -> when it was last sent to the spot or back
    std::uint64_t _tameStartMs = 0;
    std::uint64_t _tameBackMs = 0;                         // when the hunters were first sent back (0 = not yet)
    bool _equipSent = false;
    std::uint64_t _outsideSinceMs = 0;     // since when the living leader has been outside the dungeon map in the Running phase (0 = not outside)
    std::uint64_t _lastReenterMs = 0;
    bool _recovered = false;               // the party has rested up at its staging place (Entering waits for it once)
    std::uint64_t _lastEnterMs = 0;
    std::uint64_t _leaderArrivedMs = 0;
    std::uint64_t _runStartMs = 0;
    std::uint64_t _routeDoneMs = 0;        // when the leader first touched the last waypoint (RequireBosses grace)
    std::set<std::uint32_t> _bossKilledEntries;
    std::map<std::uint64_t, bool> _everDead;
    std::vector<std::uint64_t> _members;               // bot guids in the order first seen
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

// One line of text for the log: "scenario <name>: outcome=... furthest=3/7 deaths=2 alive=3/5 events=3 revived=1 wipes=0 ghosts=0 run=412s", then ' | roster=<race>:<class>,... classes=<sorted ids> combo=<sorted ids joined by -> ' when the party is known.
std::string FormatScenarioResult(ScenarioResult const& result);
