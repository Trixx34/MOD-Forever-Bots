// SPDX-License-Identifier: GPL-2.0-or-later
#include "scenario.h"

#include <algorithm>
#include <cmath>
#include <sstream>

using forever_module::ActionType;
using forever_module::BotAction;
using forever_module::BotSnapshot;

namespace
{

// Tuning values only this file uses (moved from scenario.h; the ones the tests also name stay in the header).
constexpr char const* kTimeStateNames[] = { "fight", "walk", "dead", "rest", "far", "idle" };
constexpr std::uint32_t kOnyxiaBossEntry = 10184; // Onyxia, the Onyxia raid's last boss (bot_policy_internal.h): her health is the result's onyxia_hp=
constexpr std::uint32_t kTimeRestHealthPct = 90;
constexpr std::uint32_t kTimeRestPowerPct = 95;
constexpr float kTimeFarYards = 30.0f;
constexpr float kTameArrivedYards = 150.0f;         // a hunter this close to the spot has arrived
constexpr float kTameHomeYards = 40.0f;             // and one this close to where it stood is back
constexpr std::uint32_t kAmmoLow = 100;             // below this many pieces of its ammunition a hunter is given more
constexpr std::uint32_t kAmmoGrantsMax = 40;        // per bot and run
constexpr std::uint32_t kAmmoStackMax = 200;        // a grant is at most one stack (the core refuses a count above the item's stack size: 200 for arrows and shot)
constexpr std::uint32_t kAmmoUselessMax = 3;        // grants in a row after which the count did not grow: the bot is left alone (the core refuses for another reason)
constexpr std::uint32_t kSupplyGrantsMax = 40;       // per bot and item, per run (on top of the dressing's first grant)
constexpr std::uint64_t kConfigProbeRetryMs = 6000;   // a config probe the core has not answered (or answered without applying) is sent again after this
constexpr std::uint32_t kConfigProbeRefusalsMax = 3;   // refusals in a row (a teleport refused in combat is not the config) that end the run as config_refused
constexpr std::uint64_t kGroupingTimeoutMs = 120000;
// The party waits at its staging place while any living member is in combat, and the whole party is sent in together once none is. This is the longest that
// wait may last; a fight that outlasts it ends the run as before.
constexpr std::uint64_t kEnterCombatMaxMs = 600000;

constexpr std::uint32_t kServiceEquip = 4; // kVendor's equip bit
constexpr std::uint32_t kWarriorClass = 1;
constexpr std::uint32_t kHunterClass = 3;

float Distance2d(BotSnapshot const& bot, ScenarioPoint const& point)
{
    float const dx = bot.pos_x - point.X;
    float const dy = bot.pos_y - point.Y;
    return std::sqrt(dx * dx + dy * dy);
}

// "Reached" a waypoint: close on the ground AND not on another floor (kRouteReachMaxZYards in scenario.h says why).
bool Reached(BotSnapshot const& bot, ScenarioPoint const& point)
{
    return Distance2d(bot, point) <= kRoutePointReachYards && std::fabs(bot.pos_z - point.Z) <= kRouteReachMaxZYards;
}

BotSnapshot const* LeaderOf(std::vector<BotSnapshot> const& bots)
{
    for (BotSnapshot const& bot : bots)
        if (bot.group_role == 2)
            return &bot;
    return nullptr;
}

} // namespace

void ScenarioDirector::MarkBossKilled(std::uint32_t entry, std::string const& name, std::uint64_t nowMs)
{
    _bossKilledEntries.insert(entry);
    if (std::find(_bossKills.begin(), _bossKills.end(), name) == _bossKills.end())
    {
        _bossKills.push_back(name);
        _bossKillMs.emplace_back(name, _runStartMs != 0 && nowMs >= _runStartMs ? nowMs - _runStartMs : 0);
        if (_runStartMs != 0)
        {
            NoteProgress(nowMs, true, name, 0);
            if (_attemptOpen)
            {
                run_telemetry::BossAttempt end;
                end.TS = (nowMs - _runStartMs) / 1000;
                end.Boss = _attemptBoss;
                end.End = true;
                end.DurS = nowMs >= _attemptStartMs ? (nowMs - _attemptStartMs) / 1000 : 0;
                end.Deaths = _deathEvents - _attemptDeaths0;
                end.Outcome = _attemptEntry == entry ? "killed" : "aborted";
                _telemetry.push_back(run_telemetry::FormatBossAttempt({ _plan.Name, "" }, end));
                _attemptOpen = false;
            }
        }
    }
}

// Progress of the run for the stall rule: a boss kill or a route point advanced. Starts a new counting window.
void ScenarioDirector::NoteProgress(std::uint64_t nowMs, bool boss, std::string const& name, std::uint32_t point)
{
    _stall.NoteProgress(nowMs);
    _lastProgress.IsBoss = boss;
    _lastProgress.Name = name;
    _lastProgress.Point = point;
    _lastProgress.AtS = _runStartMs != 0 && nowMs >= _runStartMs ? (nowMs - _runStartMs) / 1000 : 0;
    _window = {};
    _windowSinceMs = nowMs;
}

std::vector<std::string> ScenarioDirector::TakeTelemetry()
{
    std::vector<std::string> lines;
    lines.swap(_telemetry);
    return lines;
}

// Called once per Running tick, after the progress and the boss kills of the tick are known. Bounded output: one party_tl: per 30 s, a party_stalled: per 300 s
// without progress, two boss_attempt: lines per attempt. No per-bot lines.
void ScenarioDirector::TrackRun(forever_module::WorldTickSnapshotView const& view, BotSnapshot const* leader, char const* step, std::uint64_t nowMs)
{
    if (_runStartMs == 0 || nowMs < _runStartMs)
        return;
    constexpr std::size_t kTelemetryQueueMax = 256;
    std::vector<BotSnapshot> const& bots = view.bots;
    std::uint64_t const tS = (nowMs - _runStartMs) / 1000;
    run_telemetry::PartyId const id{ _plan.Name, "" };
    _step = step;

    std::uint32_t alive = 0, inFight = 0;
    for (BotSnapshot const& bot : bots)
    {
        alive += bot.life_state == 0;
        bool const fighting = bot.life_state == 0 && bot.in_combat != 0;
        inFight += fighting;
        bool& was = _inCombat[bot.guid];
        if (fighting && !was)
            ++_window.Pulls;
        was = fighting;
    }
    if (leader)
    {
        auto const [seen, fresh] = _moveSeq.try_emplace(leader->guid, leader->last_action_sequence);
        if (!fresh && seen->second != leader->last_action_sequence)
        {
            seen->second = leader->last_action_sequence;
            if (leader->last_action_type == static_cast<std::uint8_t>(ActionType::kMoveTo) &&
                (leader->last_action_status == forever_module::BotActionStatus::kNoRoute || leader->last_action_status == forever_module::BotActionStatus::kRefused))
                ++_window.RouteRefusals;
        }
    }

    // Boss attempts: the start is a member in a fight with a boss of the plan in view and alive; the end is its death (MarkBossKilled) or the fight being over.
    if (!_plan.Bosses.empty())
    {
        if (!_attemptOpen && inFight != 0)
        {
            for (std::size_t b = 0; b < bots.size() && !_attemptOpen; ++b)
                for (forever_module::NearbyUnit const& unit : view.NearbyOf(b))
                {
                    if (unit.health == 0 || forever_module::UnitRefType(unit.guid) != 8)
                        continue;
                    std::uint32_t const entry = forever_module::UnitRefEntry(unit.guid);
                    if (_bossKilledEntries.count(entry))
                        continue;
                    auto const boss = std::find_if(_plan.Bosses.begin(), _plan.Bosses.end(), [entry](auto const& pair) { return pair.first == entry; });
                    if (boss == _plan.Bosses.end())
                        continue;
                    _attemptOpen = true;
                    _attemptEntry = entry;
                    _attemptBoss = boss->second;
                    _attemptStartMs = nowMs;
                    _attemptLastFightMs = nowMs;
                    _attemptDeaths0 = _deathEvents;
                    run_telemetry::BossAttempt start;
                    start.TS = tS;
                    start.Boss = _attemptBoss;
                    start.Outcome = "open";
                    _telemetry.push_back(run_telemetry::FormatBossAttempt(id, start));
                    break;
                }
        }
        else if (_attemptOpen)
        {
            if (inFight != 0)
                _attemptLastFightMs = nowMs;
            if (inFight == 0 && (alive == 0 || nowMs - _attemptLastFightMs >= run_telemetry::kAttemptEndIdleMs))
            {
                run_telemetry::BossAttempt end;
                end.TS = tS;
                end.Boss = _attemptBoss;
                end.End = true;
                end.DurS = (_attemptLastFightMs - _attemptStartMs) / 1000;
                end.Deaths = _deathEvents - _attemptDeaths0;
                end.Outcome = "aborted";
                _telemetry.push_back(run_telemetry::FormatBossAttempt(id, end));
                _attemptOpen = false;
            }
        }
    }

    // The stall verdict.
    if (_stall.Check(nowMs))
    {
        run_telemetry::Stall stall;
        stall.TS = tS;
        _window.WindowMs = nowMs - _windowSinceMs;
        stall.Counters = _window;
        stall.Cause = run_telemetry::StallCause(_window);
        stall.NoProgressS = (nowMs - _stall.LastProgressMs()) / 1000;
        stall.Count = _stall.Count();
        stall.Point = _furthest;
        stall.Points = std::uint32_t(_plan.Route.size());
        stall.Last = _lastProgress;
        _telemetry.push_back(run_telemetry::FormatStall(id, stall));
        _lastStallCause = stall.Cause;
        _lastStallS = tS;
        _window = {};
        _windowSinceMs = nowMs;
    }

    // The timeline.
    std::uint64_t const slot = nowMs - _runStartMs >= run_telemetry::kTimelineEveryMs ? (nowMs - _runStartMs) / run_telemetry::kTimelineEveryMs : 0;
    if (slot > _timelineAtS && leader)
    {
        _timelineAtS = slot;
        run_telemetry::Timeline line;
        line.TS = tS;
        line.Leader = leader->guid;
        line.Map = leader->map_id;
        line.X = leader->pos_x;
        line.Y = leader->pos_y;
        line.Z = leader->pos_z;
        line.Step = _step;
        line.Point = _furthest;
        line.Points = std::uint32_t(_plan.Route.size());
        line.Target = forever_module::UnitRefEntry(leader->threat_target_guid);
        line.Alive = alive;
        line.Total = std::uint32_t(bots.size());
        line.InFight = inFight;
        line.Last = _lastProgress;
        _telemetry.push_back(run_telemetry::FormatTimeline(id, line));
    }
    if (_telemetry.size() > kTelemetryQueueMax)
        _telemetry.erase(_telemetry.begin(), _telemetry.begin() + std::ptrdiff_t(_telemetry.size() - kTelemetryQueueMax));
}

bool ScenarioDirector::NoteBossKill(std::uint32_t entry, std::uint64_t nowMs)
{
    for (auto const& [bossEntry, name] : _plan.Bosses)
        if (bossEntry == entry)
        {
            MarkBossKilled(entry, name, nowMs);
            return true;
        }
    return false;
}

bool BossKillLog::Record(std::uint32_t map, std::uint32_t instance, std::uint32_t entry)
{
    if (_seen.size() >= kMaxKept)
        _seen.clear(); // a long-running world: forget the oldest rather than grow without bound (a repeat of a very old kill is then logged again)
    return _seen.insert({ map, instance, entry }).second;
}

std::size_t BossKillLog::CountIn(std::uint32_t map, std::uint32_t instance) const
{
    std::size_t count = 0;
    for (auto const& [m, i, e] : _seen)
        if (m == map && i == instance)
            ++count;
    return count;
}

void ScenarioDirector::Finish(std::string outcome, std::uint64_t nowMs)
{
    ScenarioResult result;
    result.Name = _plan.Name;
    result.Outcome = std::move(outcome);
    result.RunMs = _runStartMs != 0 && nowMs >= _runStartMs ? nowMs - _runStartMs : 0;
    result.Furthest = _furthest;
    result.RouteSize = std::uint32_t(_plan.Route.size());
    result.RosterSize = _plan.RosterSize;
    result.PartyFormed = _partyFormed;
    result.Raid = _plan.Raid;
    result.Groups = _plan.Raid ? _plan.GroupSubgroups : 0;
    result.BossesTotal = std::uint32_t(_plan.Bosses.size());
    result.BossesKilled = _bossKills;
    result.BossKillMs = _bossKillMs;
    result.OnyxiaHealthPct = _onyxiaHealthPct;
    result.StateMs = _stateMs;
    result.Stalled = _stall.Count();
    result.LastStallCause = _lastStallCause;
    result.LastStallS = _lastStallS;
    for (auto const& [guid, dead] : _everDead)
        if (dead)
            ++result.Deaths;
    result.DeathEvents = _deathEvents;
    result.Revived = _revived;
    result.Wipes = _wipes;
    for (std::uint64_t const guid : _members)
        result.Party.emplace_back(_plan.RaceOf ? _plan.RaceOf(guid) : 0, _plan.ClassOf ? _plan.ClassOf(guid) : 0);
    _result = result;
    _phase = ScenarioPhase::Done;
}

void ScenarioDirector::CheckConfig(forever_module::WorldTickSnapshotView const& view, std::uint64_t nowMs, std::vector<BotAction>& actions)
{
    std::vector<BotSnapshot> const& bots = view.bots;
    constexpr auto kGrantType = static_cast<std::uint8_t>(ActionType::kGrant);
    constexpr auto kTeleportType = static_cast<std::uint8_t>(ActionType::kTeleport);

    // The dressing's grants. A bot is baselined the first time it is seen (its results so far are not this run's), so only a later sequence is judged.
    for (BotSnapshot const& bot : bots)
    {
        auto const [seen, fresh] = _grantSeqSeen.try_emplace(bot.guid, bot.last_action_sequence);
        if (fresh || bot.last_action_type != kGrantType || seen->second == bot.last_action_sequence)
            continue;
        seen->second = bot.last_action_sequence;
        if (bot.last_action_status == forever_module::BotActionStatus::kApplied)
            _grantOk = true;
        else if (bot.last_action_status == forever_module::BotActionStatus::kRefused)
            _grantRefusedBy.insert(bot.guid);
    }
    // Two bots refused and none applied: the core is not granting (its grant key is zero or it refuses the whole class of action).
    if (!_grantOk && _grantRefusedBy.size() >= 2)
    {
        Finish("config_refused", nowMs);
        return;
    }
    if (_teleportOk)
        return;

    // The probe: a teleport of a living bot to its own spot. Its answer is a new sequence whose last action is the teleport.
    if (_probeSent)
    {
        for (BotSnapshot const& bot : bots)
        {
            if (bot.guid != _probeGuid || bot.last_action_sequence == _probeBaseline || bot.last_action_type != kTeleportType)
                continue;
            if (bot.last_action_status == forever_module::BotActionStatus::kApplied)
            {
                _teleportOk = true;
                _probeRefusals = 0;
                return;
            }
            if (bot.last_action_status == forever_module::BotActionStatus::kRefused)
            {
                if (++_probeRefusals >= kConfigProbeRefusalsMax)
                {
                    Finish("config_refused", nowMs);
                    return;
                }
            }
            _probeSent = false; // answered without applying: sent again below
        }
    }
    if (_probeSent && nowMs - _probeSentMs < kConfigProbeRetryMs)
        return;
    if (_probeFirstMs != 0 && nowMs - _probeFirstMs >= kConfigProbeTimeoutMs)
    {
        Finish("config_unknown", nowMs);
        return;
    }
    for (BotSnapshot const& bot : bots)
    {
        if (bot.life_state != 0)
            continue;
        BotAction probe;
        probe.bot_guid = bot.guid;
        probe.type = ActionType::kTeleport;
        probe.spell_id = bot.map_id;
        probe.dest_x = bot.pos_x;
        probe.dest_y = bot.pos_y;
        probe.dest_z = bot.pos_z;
        actions.push_back(probe);
        _probeSent = true;
        _probeGuid = bot.guid;
        _probeBaseline = bot.last_action_sequence;
        _probeSentMs = nowMs;
        if (_probeFirstMs == 0)
            _probeFirstMs = nowMs;
        return;
    }
}

std::vector<std::string> ScenarioDirector::TakeNewBossKills()
{
    std::vector<std::string> fresh(_bossKills.begin() + std::ptrdiff_t(_bossKillsReported), _bossKills.end());
    _bossKillsReported = _bossKills.size();
    return fresh;
}

std::optional<ScenarioResult> ScenarioDirector::TakeResult()
{
    if (!_result || _resultTaken)
        return std::nullopt;
    _resultTaken = true;
    return _result;
}

void ScenarioDirector::Decide(forever_module::WorldTickSnapshotView const& view, std::uint64_t nowMs, std::vector<BotAction>& actions)
{
    if (_phase == ScenarioPhase::Done || view.tick_number == _lastTick)
        return;
    _lastTick = view.tick_number;
    if (!_phaseStarted)
    {
        _phaseStarted = true;
        _phaseSinceMs = nowMs;
    }
    auto const enter = [&](ScenarioPhase next)
    {
        _phase = next;
        _phaseSinceMs = nowMs;
    };

    std::vector<BotSnapshot> const& bots = view.bots;
    // A hunter of the pet level with no pet (pet_state 0 none, 1 alive, 2 dead: a dead one is revived by the policy, so it is not sent to tame).
    auto const needsPet = [&](BotSnapshot const& bot)
    {
        return _plan.ClassOf && _plan.ClassOf(bot.guid) == kHunterClass && bot.level >= kTameMinLevel && bot.life_state == 0 && bot.pet_state == 0;
    };
    // A bot is up only with life_state 0: a released ghost has health 1, so health says nothing about it.
    for (BotSnapshot const& bot : bots)
    {
        if (std::find(_members.begin(), _members.end(), bot.guid) == _members.end())
            _members.push_back(bot.guid);
        bool const down = bot.life_state != 0;
        bool& wasDown = _wasDown[bot.guid];
        if (down)
            _everDead[bot.guid] = true;
        if (down && !wasDown)
            ++_deathEvents;
        else if (!down && wasDown)
            ++_revived;
        wasDown = down;
    }
    auto const aliveCount = [&bots]
    {
        std::uint32_t alive = 0;
        for (BotSnapshot const& bot : bots)
            alive += bot.life_state == 0;
        return alive;
    };
    auto const ghostCount = [&bots]
    {
        std::uint32_t ghosts = 0;
        for (BotSnapshot const& bot : bots)
            ghosts += bot.life_state == 2;
        return ghosts;
    };

    // Ammunition for a hunter that carries none (the core recommends the projectile its bow, crossbow or gun takes: ammo_entry): without it the policy leaves
    // every ranged row out and the hunter only swings. From the dressing on, so the grant is in the bags before the party enters; the retry gap lets the
    // snapshot show the grant before the next one.
    if (_plan.AmmoCount != 0 && _phase != ScenarioPhase::Waiting)
        for (BotSnapshot const& bot : bots)
        {
            if (bot.ammo_entry != 0)
                _ammoPeak[bot.guid] = std::max(_ammoPeak[bot.guid], bot.ammo_count);
            if (bot.life_state != 0 || bot.ammo_entry == 0 || bot.ammo_count >= kAmmoLow || _ammoGrants[bot.guid] >= kAmmoGrantsMax)
                continue;
            std::uint64_t& last = _ammoAtMs[bot.guid];
            if (last != 0 && nowMs - last < kAmmoRetryMs)
                continue;
            // A grant that never raised the count (the core refused it) is not sent again and again: after kAmmoUselessMax in a row the bot is left alone. The count is compared
            // with the highest one seen since the grant, since a hunter shoots what it was given.
            if (last != 0)
                _ammoUseless[bot.guid] = _ammoPeak[bot.guid] > _ammoSeen[bot.guid] ? 0 : _ammoUseless[bot.guid] + 1;
            if (_ammoUseless[bot.guid] >= kAmmoUselessMax)
            {
                _ammoGrants[bot.guid] = kAmmoGrantsMax;
                continue;
            }
            _ammoSeen[bot.guid] = bot.ammo_count;
            _ammoPeak[bot.guid] = bot.ammo_count;
            BotAction grant;
            grant.bot_guid = bot.guid;
            grant.type = ActionType::kGrant;
            grant.spell_id = bot.ammo_entry;
            grant.dest_x = float(std::min(_plan.AmmoCount, kAmmoStackMax));
            actions.push_back(grant);
            last = nowMs;
            ++_ammoGrants[bot.guid];
        }

    // Supplies 0 (food) and 1 (drink) of _plan.Supplies: kept topped up for the whole run the same way as ammunition, including the first grant (so
    // nothing here needs the dressing phase specifically). A bot below half of its index's configured count is given that many more of it. These are
    // the only two the snapshot gives a bag count for (BotSnapshot::food_count, drink_count; there is no count per item entry), so each is compared
    // with and tracked by its own total. A grant that never raises it (the core refused it: a full bag, or -- for drink -- a class without mana,
    // which the core always reports a drink total of 0 for) is not sent again and again: after kSupplyUselessMax in a row that one is left alone;
    // the other (food or drink) is unaffected, since every count here is kept per (bot, index).
    if (!_plan.Supplies.empty() && _phase != ScenarioPhase::Waiting)
        for (BotSnapshot const& bot : bots)
        {
            if (bot.life_state != 0)
                continue;
            for (std::size_t i = 0; i < _plan.Supplies.size() && i < 2; ++i)
            {
                SupplyKey const key{ bot.guid, i };
                std::uint32_t const total = i == 0 ? bot.food_count : bot.drink_count;
                _supplyPeak[key] = std::max(_supplyPeak[key], total);
                std::uint32_t const configured = _plan.Supplies[i].second;
                std::uint32_t const low = std::max<std::uint32_t>(1, configured / 2);
                if (total >= low || _supplyGrants[key] >= kSupplyGrantsMax)
                    continue;
                std::uint64_t& last = _supplyAtMs[key];
                if (last != 0 && nowMs - last < kSupplyRetryMs)
                    continue;
                if (last != 0)
                    _supplyUseless[key] = _supplyPeak[key] > _supplySeen[key] ? 0 : _supplyUseless[key] + 1;
                if (_supplyUseless[key] >= kSupplyUselessMax)
                {
                    _supplyGrants[key] = kSupplyGrantsMax;
                    continue;
                }
                _supplySeen[key] = total;
                _supplyPeak[key] = total;
                BotAction grant;
                grant.bot_guid = bot.guid;
                grant.type = ActionType::kGrant;
                grant.spell_id = _plan.Supplies[i].first;
                grant.dest_x = float(configured);
                actions.push_back(grant);
                last = nowMs;
                ++_supplyGrants[key];
            }
        }

    switch (_phase)
    {
        case ScenarioPhase::Waiting:
        {
            // The whole roster in view, all of it alive; or, with a member already dead, kRosterDeadGraceMs later (the dead are revived by the policy and skipped
            // by the entering, so they need not hold the start).
            if (bots.size() < _plan.RosterSize)
            {
                _rosterInViewMs = 0;
                break;
            }
            if (_rosterInViewMs == 0)
                _rosterInViewMs = nowMs;
            if (aliveCount() >= _plan.RosterSize || nowMs - _rosterInViewMs >= kRosterDeadGraceMs)
                enter(ScenarioPhase::Dressing);
            break;
        }

        case ScenarioPhase::Dressing:
        {
            if (_plan.VerifyConfig)
            {
                CheckConfig(view, nowMs, actions);
                if (_phase == ScenarioPhase::Done)
                    break;
            }
            // One grant per bot per tick, in the order of its class's list.
            bool pending = false;
            for (BotSnapshot const& bot : bots)
            {
                std::uint32_t const classId = _plan.ClassOf ? _plan.ClassOf(bot.guid) : 0;
                auto const gear = _plan.Gear.find(classId);
                std::size_t& next = _nextGrant[bot.guid];
                if (gear == _plan.Gear.end() || gear->second.empty())
                {
                    // No gear list for the class: the core dresses the bot for its level, once. A warrior takes the tank's weapons.
                    if (next == 0)
                    {
                        BotAction autoGear;
                        autoGear.bot_guid = bot.guid;
                        autoGear.type = ActionType::kAutoGear;
                        autoGear.spell_id = classId == kWarriorClass ? 1u : 0u;
                        actions.push_back(autoGear);
                        next = 1;
                        pending = true;
                    }
                    continue;
                }
                if (next >= gear->second.size())
                    continue;
                BotAction grant;
                grant.bot_guid = bot.guid;
                grant.type = ActionType::kGrant;
                grant.spell_id = gear->second[next++];
                grant.dest_x = 1.0f;
                actions.push_back(grant);
                pending = true;
            }
            // Supplies 0 (food) and 1 (drink) are granted by the block above, running from here on alongside every phase like ammunition, not
            // gating this one. A Supplies index beyond that has no bag count to keep it topped up by, so it is granted once here instead, like gear.
            for (BotSnapshot const& bot : bots)
            {
                std::size_t& next = _nextSupply[bot.guid];
                if (next < 2)
                    next = 2; // indices 0 and 1 belong to the block above
                if (next >= _plan.Supplies.size())
                    continue;
                BotAction grant;
                grant.bot_guid = bot.guid;
                grant.type = ActionType::kGrant;
                grant.spell_id = _plan.Supplies[next].first;
                grant.dest_x = float(_plan.Supplies[next].second);
                actions.push_back(grant);
                ++next;
                pending = true;
            }
            // Class supplies (a warlock's Soul Shards), then the class quests that teach a pet's spells.
            for (BotSnapshot const& bot : bots)
            {
                std::uint32_t const classId = _plan.ClassOf ? _plan.ClassOf(bot.guid) : 0;
                if (auto const supplies = _plan.ClassSupplies.find(classId); supplies != _plan.ClassSupplies.end())
                {
                    std::size_t& next = _nextClassSupply[bot.guid];
                    if (next < supplies->second.size())
                    {
                        BotAction grant;
                        grant.bot_guid = bot.guid;
                        grant.type = ActionType::kGrant;
                        grant.spell_id = supplies->second[next].first;
                        grant.dest_x = float(supplies->second[next].second);
                        actions.push_back(grant);
                        ++next;
                        pending = true;
                        continue;
                    }
                }
                if (auto const quests = _plan.ClassQuests.find(classId); quests != _plan.ClassQuests.end())
                {
                    std::size_t& next = _nextClassQuest[bot.guid];
                    if (next < quests->second.size())
                    {
                        BotAction quest;
                        quest.bot_guid = bot.guid;
                        quest.type = ActionType::kQuest;
                        quest.spell_id = (4u << 24) | (quests->second[next] & 0xFFFFFFu); // operation 4: complete with the reward
                        actions.push_back(quest);
                        ++next;
                        pending = true;
                    }
                }
            }
            if (pending)
            {
                _phaseSinceMs = nowMs; // the pause counts from the last grant
                break;
            }
            if (!_equipSent && nowMs - _phaseSinceMs >= kDressPauseMs)
            {
                for (BotSnapshot const& bot : bots)
                {
                    BotAction equip;
                    equip.bot_guid = bot.guid;
                    equip.type = ActionType::kVendor;
                    equip.spell_id = kServiceEquip;
                    actions.push_back(equip);
                }
                _equipSent = true;
                _phaseSinceMs = nowMs;
            }
            else if (_equipSent && nowMs - _phaseSinceMs >= kEquipPauseMs && (!_plan.VerifyConfig || _teleportOk))
            {
                // Hunters without a pet are sent to tame one first (when the plan has a place for it). With the config guard on, the dressing ends only once a
                // teleport has been applied (CheckConfig), so the party cannot reach the entry with a teleport the core refuses.
                bool tamers = false;
                if (_plan.TameEnabled)
                    for (BotSnapshot const& bot : bots)
                        if (needsPet(bot))
                        {
                            tamers = true;
                            _tameHome[bot.guid] = Home{ bot.map_id, bot.pos_x, bot.pos_y, bot.pos_z };
                        }
                if (tamers)
                {
                    _tameStartMs = nowMs;
                    enter(ScenarioPhase::Taming);
                }
                else
                    enter(ScenarioPhase::Grouping);
            }
            break;
        }

        case ScenarioPhase::Taming:
        {
            // Out to the spot, one teleport per hunter that has not arrived (again after kTameRetryMs, for a refusal in combat); then the policy tames. Back when all have a pet, or at the timeout.
            bool pending = false;
            for (BotSnapshot const& bot : bots)
            {
                auto const home = _tameHome.find(bot.guid);
                if (home == _tameHome.end() || bot.life_state != 0 || bot.pet_state == 1)
                    continue;
                pending = true;
                std::uint64_t& sent = _tameSentAtMs[bot.guid];
                bool const there = bot.map_id == _plan.TameMap && Distance2d(bot, _plan.TameSpot) <= kTameArrivedYards;
                if (!there && (sent == 0 || nowMs - sent >= kTameRetryMs))
                {
                    BotAction away;
                    away.bot_guid = bot.guid;
                    away.type = ActionType::kTeleport;
                    away.spell_id = _plan.TameMap;
                    away.dest_x = _plan.TameSpot.X;
                    away.dest_y = _plan.TameSpot.Y;
                    away.dest_z = _plan.TameSpot.Z;
                    actions.push_back(away);
                    sent = nowMs;
                }
            }
            if (pending && nowMs - _tameStartMs < kTameTimeoutMs)
                break;
            // Back to where each hunter stood. The grouping starts when they are there, or kTameBackMs after the first order.
            if (_tameBackMs == 0)
                _tameBackMs = nowMs;
            bool allHome = true;
            for (BotSnapshot const& bot : bots)
            {
                auto const home = _tameHome.find(bot.guid);
                if (home == _tameHome.end() || bot.life_state != 0)
                    continue;
                bool const there = bot.map_id == home->second.Map && std::hypot(bot.pos_x - home->second.X, bot.pos_y - home->second.Y) <= kTameHomeYards;
                if (there)
                    continue;
                allHome = false;
                std::uint64_t& sent = _tameSentAtMs[bot.guid];
                if (sent == 0 || nowMs - sent >= kTameRetryMs)
                {
                    BotAction back;
                    back.bot_guid = bot.guid;
                    back.type = ActionType::kTeleport;
                    back.spell_id = home->second.Map;
                    back.dest_x = home->second.X;
                    back.dest_y = home->second.Y;
                    back.dest_z = home->second.Z;
                    actions.push_back(back);
                    sent = nowMs;
                }
            }
            if (allHome || nowMs - _tameBackMs >= kTameBackMs)
                enter(ScenarioPhase::Grouping);
            break;
        }

        case ScenarioPhase::Grouping:
        {
            bool together = bots.size() >= _plan.RosterSize;
            for (BotSnapshot const& bot : bots)
                together = together && bot.group_role != 0 && bot.group_size >= _plan.RosterSize;
            if (together)
                enter(ScenarioPhase::Entering);
            else if (nowMs - _phaseSinceMs >= kGroupingTimeoutMs)
            {
                _partyFormed = false;
                enter(ScenarioPhase::Entering);
            }
            break;
        }

        case ScenarioPhase::Entering:
        {
            // Rest first (see kRecoverPct): nothing is sent, so the bots sit, eat and drink by themselves, and the healer tops the party up.
            if (!_recovered)
            {
                bool ready = !bots.empty();
                for (BotSnapshot const& bot : bots)
                {
                    if (bot.life_state != 0)
                        continue;
                    if (bot.max_health != 0 && std::uint64_t(bot.health) * 100 < std::uint64_t(bot.max_health) * kRecoverPct)
                        ready = false;
                    if (bot.power_type == 0 && bot.max_power != 0 && std::uint64_t(bot.power) * 100 < std::uint64_t(bot.max_power) * kRecoverPct)
                        ready = false;
                }
                if (!ready && nowMs - _phaseSinceMs < kRecoverMaxMs)
                    break;
                _recovered = true;
                _phaseSinceMs = nowMs; // the entering timeout counts from here
            }
            BotSnapshot const* leader = LeaderOf(bots);
            if (!leader && !bots.empty())
                leader = &bots.front(); // no party: the first bot leads
            if (!leader)
                break;
            auto const teleport = [&](BotSnapshot const& bot)
            {
                BotAction action;
                action.bot_guid = bot.guid;
                action.type = ActionType::kTeleport;
                action.spell_id = _plan.EntryMap;
                action.dest_x = _plan.Entry.X;
                action.dest_y = _plan.Entry.Y;
                action.dest_z = _plan.Entry.Z;
                actions.push_back(action);
            };

            // The party goes in together, once no living member is in combat: a member still fighting would be refused, and the rest would go in without it.
            // Waiting is capped by kEnterCombatMaxMs, after which the timeout below ends the run.
            bool fighting = false;
            for (BotSnapshot const& bot : bots)
                fighting = fighting || (bot.life_state == 0 && bot.in_combat);
            if (fighting)
                _enterFought = true;
            if (!fighting)
                _enterFightSinceMs = 0;
            else if (_enterFightSinceMs == 0)
                _enterFightSinceMs = nowMs;
            // A fight that has lasted kEnterCombatGraceMs no longer holds the others: the ones out of combat go in, the ones fighting follow when they can.
            bool const fightTooLong = fighting && nowMs - _enterFightSinceMs >= kEnterCombatGraceMs;
            if (leader->map_id != _plan.EntryMap)
            {
                // Nobody goes in before the whole roster is at the leader's staging place: the first ones inside would meet the entrance alone.
                // A dead bot does not hold the gate; a living one still on its way does, until the timeout below; one in combat does until kEnterCombatGraceMs.
                bool staged = bots.size() >= _plan.RosterSize;
                for (BotSnapshot const& bot : bots)
                    staged = staged && (bot.life_state != 0 || ((!bot.in_combat || fightTooLong) && bot.map_id == leader->map_id));
                if (fighting && !fightTooLong)
                    break;
                if (staged && (_lastEnterMs == 0 || nowMs - _lastEnterMs >= kEnterRetryMs))
                {
                    if (!leader->in_combat)
                        teleport(*leader);
                    for (BotSnapshot const& bot : bots)
                        if (bot.guid != leader->guid && bot.map_id != _plan.EntryMap && bot.life_state == 0 && !bot.in_combat)
                            teleport(bot);
                    _lastEnterMs = nowMs;
                }
            }
            else
            {
                if (_leaderArrivedMs == 0)
                {
                    _leaderArrivedMs = nowMs;
                    _lastEnterMs = 0;
                }
                bool allThere = true;
                bool livingThere = true;
                for (BotSnapshot const& bot : bots)
                {
                    allThere = allThere && bot.map_id == _plan.EntryMap;
                    livingThere = livingThere && (bot.life_state != 0 || bot.map_id == _plan.EntryMap);
                }
                std::uint64_t const insideMs = nowMs - _leaderArrivedMs;
                bool const startNow = allThere || (livingThere && insideMs >= kEnterStragglersMs) || insideMs >= kEnterStragglersMaxMs;
                if (startNow && bots.size() >= _plan.RosterSize)
                {
                    _runStartMs = nowMs;
                    _stall.Start(nowMs);
                    _windowSinceMs = nowMs;
                    enter(ScenarioPhase::Running);
                    break;
                }
                if (nowMs - _leaderArrivedMs >= kMembersDelayMs && (_lastEnterMs == 0 || nowMs - _lastEnterMs >= kEnterRetryMs))
                {
                    for (BotSnapshot const& bot : bots)
                        if (bot.guid != leader->guid && bot.map_id != _plan.EntryMap && bot.life_state == 0)
                            teleport(bot);
                    _lastEnterMs = nowMs;
                }
            }
            // While the party waits for a fight to end, the timeout is kEnterCombatMaxMs rather than kEnterTimeoutMs.
            std::uint64_t const limitMs = _enterFought ? kEnterCombatMaxMs : kEnterTimeoutMs;
            if (nowMs - _phaseSinceMs >= limitMs)
                Finish("enter_failed", nowMs);
            break;
        }

        case ScenarioPhase::Running:
        {
            BotSnapshot const* leader = LeaderOf(bots);
            if (!leader && !bots.empty())
                leader = &bots.front();
            // Route progress: the leader's while it is up; with the leader down, the furthest point any living member reaches, so the
            // progress neither freezes nor is lost when the leader comes back. It never decreases.
            // In route order (2026-11-07): only the next kRouteProgressWindow points count, so a point is reached on the way, not by standing near a later
            // point that the route passes again (Wailing Caverns' point 70 lies by its point 22 on the way back, the Stockade's point 17 is its point 5,
            // Shadowfang Keep's last point is over its courtyard gate): the sim's cycle of 2026-11-07 14:20 reported furthest=70/96 for every wc party whose
            // leader was at point 33, and ended sfk#3 as bosses_missing at 818 s with the leader at point 2 (furthest=37/37). And the leader only while it is
            // up: the old test let every living member count while the leader was up (and nobody while it was down), the reverse of the rule above, so a
            // member chasing a pack across the dungeon moved the progress.
            bool const leaderUp = leader && leader->life_state == 0;
            std::uint32_t const furthestBefore = _furthest;
            for (bool advanced = true; advanced;)
            {
                advanced = false;
                std::size_t const end = std::min<std::size_t>(_plan.Route.size(), std::size_t(_furthest) + kRouteProgressWindow);
                for (BotSnapshot const& bot : bots)
                {
                    if (bot.life_state != 0 || (leaderUp && &bot != leader))
                        continue;
                    for (std::size_t i = _furthest; i < end; ++i)
                        if (Reached(bot, _plan.Route[i]) && std::uint32_t(i + 1) > _furthest)
                        {
                            _furthest = std::uint32_t(i + 1);
                            advanced = true;
                        }
                }
            }
            if (_furthest > furthestBefore)
                NoteProgress(nowMs, false, "", _furthest);
            // Where the time goes: one state for this tick (see TimeState).
            if (leader)
            {
                if (_stateLastMs != 0 && nowMs > _stateLastMs && nowMs - _stateLastMs <= kTimeStateMaxGapMs)
                {
                    TimeState state = TimeState::Idle;
                    bool fight = false, down = false, rest = false, far = false;
                    for (BotSnapshot const& bot : bots)
                    {
                        if (bot.life_state != 0)
                        {
                            down = true;
                            continue;
                        }
                        fight = fight || bot.in_combat != 0;
                        rest = rest || (bot.max_health > 0 && std::uint64_t(bot.health) * 100 < std::uint64_t(bot.max_health) * kTimeRestHealthPct) ||
                               (bot.power_type == 0 && bot.max_power > 0 && std::uint64_t(bot.power) * 100 < std::uint64_t(bot.max_power) * kTimeRestPowerPct);
                        if (&bot != leader && leader->life_state == 0)
                        {
                            float const dx = bot.pos_x - leader->pos_x, dy = bot.pos_y - leader->pos_y;
                            far = far || dx * dx + dy * dy > kTimeFarYards * kTimeFarYards;
                        }
                    }
                    if (fight)
                        state = TimeState::Fight;
                    else if (leader->life_state == 0 && leader->moving != 0)
                        state = TimeState::Walk;
                    else if (down)
                        state = TimeState::Dead;
                    else if (rest)
                        state = TimeState::Rest;
                    else if (far)
                        state = TimeState::Far;
                    _stateMs[std::size_t(state)] += nowMs - _stateLastMs;
                    _step = kTimeStateNames[std::size_t(state)];
                    std::uint64_t const dt = nowMs - _stateLastMs;
                    if (state == TimeState::Rest)
                        _window.RestMs += dt;
                    else if (state == TimeState::Dead)
                        _window.DeadMs += dt;
                    else if (state == TimeState::Fight)
                        _window.FightMs += dt;
                }
                _stateLastMs = nowMs;
            }
            // A boss counts as killed once any bot sees its corpse (a creature of that entry with no health).
            if (!_plan.Bosses.empty())
            {
                for (std::size_t b = 0; b < bots.size(); ++b)
                    for (forever_module::NearbyUnit const& unit : view.NearbyOf(b))
                    {
                        if (unit.health != 0 || forever_module::UnitRefType(unit.guid) != 8)
                            continue;
                        for (auto const& [entry, name] : _plan.Bosses)
                            if (entry == forever_module::UnitRefEntry(unit.guid))
                            {
                                MarkBossKilled(entry, name, nowMs);
                            }
                    }
            }
            TrackRun(view, leader, _step, nowMs);
            // Onyxia's health (onyxia_hp=): the last sighting by any bot, and 0 for her corpse. Only in the Onyxia raid (her entry is one of the bosses).
            if (std::any_of(_plan.Bosses.begin(), _plan.Bosses.end(), [](auto const& boss) { return boss.first == kOnyxiaBossEntry; }))
            {
                for (std::size_t b = 0; b < bots.size(); ++b)
                    for (forever_module::NearbyUnit const& unit : view.NearbyOf(b))
                    {
                        if (forever_module::UnitRefType(unit.guid) != 8 || forever_module::UnitRefEntry(unit.guid) != kOnyxiaBossEntry)
                            continue;
                        if (unit.max_health == 0 && unit.health != 0)
                            continue;
                        _onyxiaHealthPct = unit.max_health != 0 ? std::uint32_t(std::uint64_t(unit.health) * 100 / unit.max_health) : 0u;
                    }
            }
            // A wipe: the party was revived at the graveyard outside the dungeon. The leader is up and the route is not walkable from here: put the living back at the entry.
            if (leader && leader->life_state == 0 && leader->map_id != _plan.EntryMap)
            {
                if (_outsideSinceMs == 0)
                    _outsideSinceMs = nowMs;
                if (nowMs - _outsideSinceMs >= kReenterDelayMs && (_lastReenterMs == 0 || nowMs - _lastReenterMs >= kEnterRetryMs))
                {
                    for (BotSnapshot const& bot : bots)
                        if (bot.life_state == 0 && bot.map_id != _plan.EntryMap)
                        {
                            BotAction action;
                            action.bot_guid = bot.guid;
                            action.type = ActionType::kTeleport;
                            action.spell_id = _plan.EntryMap;
                            action.dest_x = _plan.Entry.X;
                            action.dest_y = _plan.Entry.Y;
                            action.dest_z = _plan.Entry.Z;
                            actions.push_back(action);
                        }
                    _lastReenterMs = nowMs;
                }
            }
            else
                _outsideSinceMs = 0;
            // A living member outside while the leader is inside (2026-11-07 16:29, gnomer#1, furthest=3/82): four members died at the Ironforge staging place, so the
            // leader went in alone (a dead bot does not hold the gate) and the run started without them (a corpse does not hold it either). They were revived where they
            // fell, on map 0, and walked to the portal; a bot is not taken in by walking onto it, so they stood there for the rest of the run while the leader died 19 times
            // alone at the Irradiated Pillagers. The entering teleport puts living stragglers in; once the run has started nothing did, short of a wipe. Now the living
            // ones out of combat are put at the entry the same way as after a wipe: kReenterDelayMs after one is first seen outside, again every kEnterRetryMs.
            bool memberOutside = false;
            if (leader && leader->life_state == 0 && leader->map_id == _plan.EntryMap)
                for (BotSnapshot const& bot : bots)
                    memberOutside = memberOutside || (&bot != leader && bot.life_state == 0 && bot.map_id != _plan.EntryMap);
            if (!memberOutside)
                _memberOutsideSinceMs = 0;
            else
            {
                if (_memberOutsideSinceMs == 0)
                    _memberOutsideSinceMs = nowMs;
                if (nowMs - _memberOutsideSinceMs >= kReenterDelayMs && (_lastMemberReenterMs == 0 || nowMs - _lastMemberReenterMs >= kEnterRetryMs))
                {
                    for (BotSnapshot const& bot : bots)
                        if (&bot != leader && bot.life_state == 0 && bot.map_id != _plan.EntryMap && !bot.in_combat)
                        {
                            BotAction action;
                            action.bot_guid = bot.guid;
                            action.type = ActionType::kTeleport;
                            action.spell_id = _plan.EntryMap;
                            action.dest_x = _plan.Entry.X;
                            action.dest_y = _plan.Entry.Y;
                            action.dest_z = _plan.Entry.Z;
                            actions.push_back(action);
                        }
                    _lastMemberReenterMs = nowMs;
                }
            }
            std::uint32_t const alive = aliveCount();
            if (alive == 0)
            {
                if (!_allDown)
                {
                    _allDown = true;
                    _allDownSinceMs = nowMs;
                }
            }
            else if (_allDown)
            {
                _allDown = false; // somebody came back: the wipe is cancelled
                ++_wipes;
            }
            bool const leaderDown = leader && leader->life_state != 0;
            if (leaderDown && !_leaderDown)
                _leaderDownSinceMs = nowMs;
            _leaderDown = leaderDown;
            bool const few = _plan.MinAlive != 0 && alive != 0 && alive < _plan.MinAlive;
            if (few && !_fewAlive)
                _fewAliveSinceMs = nowMs;
            _fewAlive = few;

            bool const routeDone = !_plan.Route.empty() && _furthest >= _plan.Route.size();
            if (routeDone && _routeDoneMs == 0)
                _routeDoneMs = nowMs;
            if (_plan.RequireBosses && !_plan.Bosses.empty())
            {
                // A dungeon clear: every boss dead is the goal; the last waypoint only starts a grace period for the ones still standing.
                if (_bossKills.size() >= _plan.Bosses.size())
                    Finish("complete", nowMs);
                else if (routeDone && nowMs - _routeDoneMs >= kBossGraceMs)
                    Finish("bosses_missing", nowMs);
            }
            else if (routeDone)
                Finish("complete", nowMs);
            if (_result)
                break;
            if (_allDown && (!_plan.WipeRecovery || nowMs - _allDownSinceMs >= _plan.WipeRecoveryMs))
                Finish("wipe", nowMs);
            else if (_plan.LeaderDeadAbandonMs != 0 && _leaderDown && nowMs - _leaderDownSinceMs >= _plan.LeaderDeadAbandonMs)
                Finish("abandoned_dead_leader", nowMs);
            else if (_fewAlive && nowMs - _fewAliveSinceMs >= kUnderstrengthGraceMs)
                Finish("understrength", nowMs);
            else if (nowMs - _runStartMs >= _plan.DurationMs)
                Finish("timeout", nowMs);
            break;
        }

        case ScenarioPhase::Done:
            break;
    }

    if (_result)
    {
        _result->Alive = aliveCount();
        _result->Ghosts = ghostCount();
        _result->DeathEvents = _deathEvents;
        _result->Revived = _revived;
    }
}

std::string FormatScenarioResult(ScenarioResult const& result)
{
    std::ostringstream out;
    out << "scenario " << result.Name << ": outcome=" << result.Outcome << " furthest=" << result.Furthest << "/" << result.RouteSize
        << " deaths=" << result.Deaths << " alive=" << result.Alive << "/" << result.RosterSize << " events=" << result.DeathEvents
        << " revived=" << result.Revived << " wipes=" << result.Wipes << " ghosts=" << result.Ghosts << " run=" << result.RunMs / 1000 << "s"
        << " party=" << (result.PartyFormed ? "yes" : "no");
    {
        std::uint64_t total = 0;
        for (std::uint64_t const ms : result.StateMs)
            total += ms;
        if (total != 0)
        {
            out << " time_s=";
            for (std::size_t i = 0; i < result.StateMs.size(); ++i)
                out << (i ? "," : "") << kTimeStateNames[i] << ":" << result.StateMs[i] / 1000;
        }
    }
    if (std::string const stall = run_telemetry::FormatResultFields(result.Stalled, result.LastStallCause, result.LastStallS); !stall.empty())
        out << " " << stall;
    // 40-man raid support (doc 108): raid=1 groups=<n> for a raid scenario, nothing extra for a dungeon one (acceptance criteria).
    if (result.Raid)
        out << " raid=1 groups=" << result.Groups;
    // Onyxia's health at the end, in the Onyxia raid only (study item 10); omitted when no bot saw her.
    if (result.OnyxiaHealthPct)
        out << " onyxia_hp=" << *result.OnyxiaHealthPct;
    if (result.BossesTotal != 0)
    {
        out << " bosses=" << result.BossesKilled.size() << "/" << result.BossesTotal << " killed=";
        for (std::size_t i = 0; i < result.BossesKilled.size(); ++i)
            out << (i ? "," : "") << result.BossesKilled[i];
        if (result.BossesKilled.empty())
            out << "none";
        // The time into the run each boss was seen dead (ms), in the order of the killed= list: how long a clear took, boss by boss.
        if (!result.BossKillMs.empty())
        {
            out << " boss_ms=";
            for (std::size_t i = 0; i < result.BossKillMs.size(); ++i)
                out << (i ? "," : "") << result.BossKillMs[i].first << ":" << result.BossKillMs[i].second;
        }
    }
    if (!result.Party.empty())
    {
        // After everything the older fields said; the " | " keeps the boss names (which may hold spaces) apart from these.
        std::vector<std::uint32_t> classes;
        out << " | roster=";
        for (std::size_t i = 0; i < result.Party.size(); ++i)
        {
            out << (i ? "," : "") << result.Party[i].first << ":" << result.Party[i].second;
            classes.push_back(result.Party[i].second);
        }
        std::sort(classes.begin(), classes.end());
        out << " classes=";
        for (std::size_t i = 0; i < classes.size(); ++i)
            out << (i ? "," : "") << classes[i];
        out << " combo=";
        for (std::size_t i = 0; i < classes.size(); ++i)
            out << (i ? "-" : "") << classes[i];
    }
    return out.str();
}

forever_module::WorldTickSnapshotView SubsetView(forever_module::WorldTickSnapshotView const& view, std::function<bool(std::uint64_t)> const& keep)
{
    forever_module::WorldTickSnapshotView out = view;
    out.bots.clear();
    out.nearby.clear();
    for (std::size_t i = 0; i < view.bots.size(); ++i)
    {
        if (!keep(view.bots[i].guid))
            continue;
        out.bots.push_back(view.bots[i]);
        std::size_t const from = i * view.max_nearby_units;
        for (std::uint32_t k = 0; k < view.max_nearby_units; ++k)
            out.nearby.push_back(from + k < view.nearby.size() ? view.nearby[from + k] : forever_module::NearbyUnit{});
    }
    return out;
}
