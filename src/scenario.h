// SPDX-License-Identifier: GPL-2.0-or-later
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

#include <array>
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
    // 40-man raid support (doc 108): Raid on allows RosterSize up to 40 instead of 5 (see bot_settings.cpp's ScenarioRaid key).
    // A raid roster is formed into one raid group of GroupSubgroups subgroups of 5, not a flat party: the roster is expected to
    // be GroupSubgroups repeats of the same tank/healer/dps class composition a 5-man scenario's roster would use, one repeat per
    // subgroup (bot_policy.cpp assigns subgroup = roster position / 5, so the ordering in ScenarioRoster matters). The result line
    // carries raid=1 groups=<GroupSubgroups> so dashboards can tell a raid result from a dungeon one.
    bool Raid = false;
    std::uint32_t GroupSubgroups = 8; // MAX_RAID_SUBGROUPS in TrinityCore's Group.h; informational only when Raid is off
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
    // The config guard (ScenarioConfigGuard, off by default): the dressing does not end until the core has applied a teleport that the director sends a living bot
    // (to its own spot). A refused teleport, or grants refused for two bots with none applied, ends the run as "config_refused"; no applied teleport within
    // kConfigProbeTimeoutMs ends it as "config_unknown". Without the guard a core with its teleport or grant switched off ran the whole scenario in the wrong place.
    bool VerifyConfig = false;
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
    std::string Outcome;            // complete, wipe, timeout, enter_failed, understrength, abandoned_dead_leader, bosses_missing, config_refused, config_unknown
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
    // 40-man raid support (doc 108): Raid and Groups mirror ScenarioPlan::Raid/GroupSubgroups, so FormatScenarioResult can print
    // "raid=1 groups=8" for a raid scenario and nothing extra for a dungeon one.
    bool Raid = false;
    std::uint32_t Groups = 0;
    std::uint32_t BossesTotal = 0;  // bosses the plan lists
    std::vector<std::string> BossesKilled; // names of the ones seen dead, in the order they were seen
    // The same bosses with the time into the run they were seen dead (ms), in that order: boss_ms=Name:ms in the result line, so a clear's pace is in the log.
    std::vector<std::pair<std::string, std::uint64_t>> BossKillMs;
    // Onyxia's health in percent at the end of the run (onyxia_hp=): the last sighting by any bot, 0 for her corpse. Set only in the Onyxia raid
    // (her entry among the plan's bosses), and only when some bot saw her; empty otherwise, and the result line then leaves it out.
    std::optional<std::uint32_t> OnyxiaHealthPct;
    // Where the run's time went (ms per state, in kTimeStateNames order): time_s= in the result line. See TimeState.
    std::array<std::uint64_t, 6> StateMs = {};
    std::vector<std::pair<std::uint32_t, std::uint32_t>> Party; // race and class id of each member, in the order first seen
};

// Where a dungeon run's time goes, from the snapshots alone (one state per tick, first match wins; the time between two ticks goes to the state
// of the later one, a gap over kTimeStateMaxGapMs is not counted):
//   fight  a living member is in combat
//   walk   the leader is moving
//   dead   a member is down (corpse or ghost): the party waits for the release, the rez or the corpse run
//   rest   a living member is under kTimeRestHealthPct health or (a mana user) kTimeRestPowerPct mana: the party waits for it to regenerate
//   far    a living member is more than kTimeFarYards from the leader: the party waits for it to catch up
//   idle   none of those: an event (lever, door), a boss hold, a path being found, casting
// The thresholds are the policy's (kPartyReadyHealthPct, kPartyReadyPowerPct, kRouteWaitYards) so "rest" and "far" are the holds the leader logs as route_hold.
enum class TimeState : std::size_t { Fight, Walk, Dead, Rest, Far, Idle, Count };
constexpr std::uint64_t kTimeStateMaxGapMs = 5000;

constexpr std::uint64_t kDressPauseMs = 1500;
constexpr std::uint64_t kEquipPauseMs = 3000;
constexpr std::uint64_t kTameTimeoutMs = 300000;    // the Taming phase gives the hunters this long to get a pet
constexpr std::uint64_t kTameRetryMs = 6000;        // a teleport that was refused (in combat) is sent again after this
constexpr std::uint64_t kTameBackMs = 20000;        // the hunters are given this long to be back before the grouping starts anyway
constexpr std::uint32_t kTameMinLevel = 10;         // the hunter pet quest is given at level 10
constexpr std::uint64_t kAmmoRetryMs = 5000;        // a bot is given ammunition at most this often (the snapshot needs a moment to show it)
constexpr std::uint64_t kSupplyRetryMs = 5000;      // a bot is given more of a supply item at most this often (the snapshot needs a moment to show it)
constexpr std::uint32_t kSupplyUselessMax = 3;       // grants in a row after which neither total grew: the bot is left alone for that item
// The roster is complete (every bot in view) but one of them is already dead: the run starts anyway this long after the last one came into view. A bot that
// died before the 40th was spawned (2026-10-29 22:31: the first tank, killed by Emberstrife while the roster was still spawning) held Waiting for good.
constexpr std::uint64_t kRosterDeadGraceMs = 30000;
constexpr std::uint64_t kConfigProbeTimeoutMs = 60000; // no applied probe in this long: the run ends as config_unknown
constexpr std::uint64_t kMembersDelayMs = 3000;
constexpr std::uint64_t kEnterRetryMs = 10000;
constexpr std::uint64_t kEnterTimeoutMs = 90000;
// A fight at the staging place holds the whole party only this long: then the members out of combat are sent in (the leader too, when it is one of them), and the
// ones still fighting every kEnterRetryMs after. The 2026-10-29 21:42 run waited the full kEnterCombatMaxMs with 1-13 bots always in some fight and ended enter_failed.
constexpr std::uint64_t kEnterCombatGraceMs = 45000;
// Once the leader is inside, the run starts when every bot is there, or when every living one is and this long has passed (a corpse or a ghost outside does not hold
// it: the policy walks it back through the portal), or at the latest kEnterStragglersMaxMs after the leader arrived.
constexpr std::uint64_t kEnterStragglersMs = 15000;
constexpr std::uint64_t kEnterStragglersMaxMs = 60000;
// Before the party enters it rests at its staging place until every living bot has this much health (and mana, for mana users), or this long has passed: the gear
// the dressing phase puts on raises the maximum health to two or three times the current health, so a party that entered at once started at 35-45 %.
constexpr std::uint32_t kRecoverPct = 90;
constexpr std::uint64_t kRecoverMaxMs = 240000;
// After a wipe the revived party stands outside at the graveyard: this long after the leader is up there, the living members are put back at the entry (a corpse run
// through the portal does the same on foot), and again every kEnterRetryMs while some are still outside.
constexpr std::uint64_t kReenterDelayMs = 20000;
constexpr std::uint64_t kUnderstrengthGraceMs = 180000; // fewer than MinAlive alive this long: the run is given up
constexpr std::uint64_t kBossGraceMs = 360000;          // RequireBosses: time allowed after the last waypoint for the remaining bosses to die
constexpr float kRoutePointReachYards = 15.0f; // the leader counts as having reached a waypoint this close to it
// ...and only this far above or below it (a different storey of the same tower is not "reached"). Shadowfang Keep's own route (2026-10-27) puts its
// courtyard-floor waypoints (points 4, 9, 10: around the Rethilgore gate) within kRoutePointReachYards in X/Y of its topmost waypoints (points 35, 36:
// Odo the Blindwatcher, a storey above, on the tower roof) with only 9.3 yards of Z between them: a bot standing at the gate, nowhere near the roof,
// was credited with "reached" the dungeon's last point this way (bots 0.36.2 and earlier: furthest=37/37, bosses=0/5). 8 yards keeps that pair apart
// while staying well above the under-one-yard Z gaps between a waypoint and the very next one that are meant to count as the same spot.
constexpr float kRouteReachMaxZYards = 8.0f;
// Route progress counts the points in order: from the furthest one reached, only the next this many can be reached (a leader that skips a few points
// on the way, or a bridge teleport that lands it 1 to 5 points on, still counts; a later point that lies by an earlier one does not).
constexpr std::size_t kRouteProgressWindow = 8;

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
    // Before the party is let loose inside: the bots hold at the staging place (a hunter of the Taming phase still tames: that is not a pull).
    bool Staging() const { return _phase != ScenarioPhase::Running && _phase != ScenarioPhase::Done; }
    bool RouteActive() const { return _phase == ScenarioPhase::Running; }

    // The result, once, after the scenario has ended.
    std::optional<ScenarioResult> TakeResult();

    // Bosses seen dead since the last call, by name (for the log as they happen).
    std::vector<std::string> TakeNewBossKills();

    // The creature entries of the bosses seen dead so far.
    std::set<std::uint32_t> const& KilledBossEntries() const { return _bossKilledEntries; }

private:
    void Finish(std::string outcome, std::uint64_t nowMs);
    // The config guard (ScenarioPlan::VerifyConfig): reads the grant results of the dressing, and sends the teleport probe until the core applies one.
    void CheckConfig(forever_module::WorldTickSnapshotView const& view, std::uint64_t nowMs, std::vector<forever_module::BotAction>& actions);

    ScenarioPlan _plan;
    bool _teleportOk = false;                              // the core applied a probe teleport
    bool _grantOk = false;                                 // the core applied a dressing grant
    bool _probeSent = false;                               // a probe is out and not yet answered
    std::uint64_t _probeGuid = 0;                          // the living bot the last probe teleport was sent to
    std::uint64_t _probeFirstMs = 0;                       // when the first probe was sent (the timeout counts from it)
    std::uint64_t _probeSentMs = 0;                        // when the last probe was sent
    std::uint32_t _probeBaseline = 0;                      // that bot's last_action_sequence before the last probe
    std::uint32_t _probeRefusals = 0;                      // probe teleports the core refused in a row
    std::map<std::uint64_t, std::uint32_t> _grantSeqSeen; // bot -> last_action_sequence of the last grant result looked at
    std::set<std::uint64_t> _grantRefusedBy;               // bots whose dressing grant the core refused
    std::vector<std::pair<std::string, std::uint64_t>> _bossKillMs; // boss name and ms into the run, in the order seen dead
    std::array<std::uint64_t, 6> _stateMs = {};                     // ms per TimeState while Running
    std::uint64_t _stateLastMs = 0;                                 // the last tick that was counted
    ScenarioPhase _phase = ScenarioPhase::Waiting;
    std::uint64_t _phaseSinceMs = 0;
    bool _phaseStarted = false;
    std::uint64_t _rosterInViewMs = 0;     // when the whole roster was first in view in the Waiting phase (0 = not yet)
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
    std::uint64_t _memberOutsideSinceMs = 0; // since when a living member has been outside while the living leader is inside (Running; 0 = none is)
    std::uint64_t _lastMemberReenterMs = 0;
    bool _recovered = false;               // the party has rested up at its staging place (Entering waits for it once)
    std::uint64_t _lastEnterMs = 0;
    std::uint64_t _leaderArrivedMs = 0;
    std::uint64_t _enterFightSinceMs = 0;  // since when some living member has been in combat at the staging place (0 = nobody is)
    bool _enterFought = false;             // a member was in combat outside the entry map while entering: the entering timeout is kEnterCombatMaxMs from then on
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
    std::optional<std::uint32_t> _onyxiaHealthPct; // Onyxia's health in percent, the last time a bot saw her (ScenarioResult::OnyxiaHealthPct)
    std::size_t _bossKillsReported = 0;
    bool _partyFormed = true;
    std::optional<ScenarioResult> _result;
    bool _resultTaken = false;
    std::uint64_t _lastTick = ~0ull;
};

// One line of text for the log: "scenario <name>: outcome=... furthest=3/7 deaths=2 alive=3/5 events=3 revived=1 wipes=0 ghosts=0 run=412s",
// then " time_s=fight:40,walk:900,..." (seconds per TimeState, only for a run that reached Running), then " raid=1 groups=8" for a raid scenario (doc 108; nothing for a dungeon one), then ' | roster=<race>:<class>,... classes=<sorted ids>
// combo=<sorted ids joined by -> ' when the party is known.
std::string FormatScenarioResult(ScenarioResult const& result);
