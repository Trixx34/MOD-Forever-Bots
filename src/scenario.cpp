#include "scenario.h"

#include <algorithm>
#include <cmath>
#include <sstream>

using forever_module::ActionType;
using forever_module::BotAction;
using forever_module::BotSnapshot;

namespace
{

constexpr std::uint32_t kServiceEquip = 4; // kVendor's equip bit
constexpr std::uint32_t kWarriorClass = 1;

float Distance2d(BotSnapshot const& bot, ScenarioPoint const& point)
{
    float const dx = bot.pos_x - point.X;
    float const dy = bot.pos_y - point.Y;
    return std::sqrt(dx * dx + dy * dy);
}

BotSnapshot const* LeaderOf(std::vector<BotSnapshot> const& bots)
{
    for (BotSnapshot const& bot : bots)
        if (bot.group_role == 2)
            return &bot;
    return nullptr;
}

} // namespace

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
    result.BossesTotal = std::uint32_t(_plan.Bosses.size());
    result.BossesKilled = _bossKills;
    for (auto const& [guid, dead] : _everDead)
        if (dead)
            ++result.Deaths;
    result.DeathEvents = _deathEvents;
    result.Revived = _revived;
    result.Wipes = _wipes;
    _result = result;
    _phase = ScenarioPhase::Done;
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
    // A bot is up only with life_state 0: a released ghost has health 1, so health says nothing about it.
    for (BotSnapshot const& bot : bots)
    {
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

    switch (_phase)
    {
        case ScenarioPhase::Waiting:
        {
            if (bots.size() >= _plan.RosterSize && aliveCount() >= _plan.RosterSize)
                enter(ScenarioPhase::Dressing);
            break;
        }

        case ScenarioPhase::Dressing:
        {
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
            // Supplies: food and drink, one kind per bot per tick, after its gear, so the party can eat and drink between pulls.
            for (BotSnapshot const& bot : bots)
            {
                std::size_t& next = _nextSupply[bot.guid];
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
            else if (_equipSent && nowMs - _phaseSinceMs >= kEquipPauseMs)
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

            if (leader->map_id != _plan.EntryMap)
            {
                if (_lastEnterMs == 0 || nowMs - _lastEnterMs >= kEnterRetryMs)
                {
                    teleport(*leader);
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
                for (BotSnapshot const& bot : bots)
                    allThere = allThere && bot.map_id == _plan.EntryMap;
                if (allThere && bots.size() >= _plan.RosterSize)
                {
                    _runStartMs = nowMs;
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
            if (nowMs - _phaseSinceMs >= kEnterTimeoutMs)
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
            for (BotSnapshot const& bot : bots)
            {
                if (bot.life_state != 0 || (leader && leader->life_state == 0 && &bot != leader))
                    continue;
                for (std::size_t i = 0; i < _plan.Route.size(); ++i)
                    if (Distance2d(bot, _plan.Route[i]) <= kRoutePointReachYards)
                        _furthest = std::max<std::uint32_t>(_furthest, std::uint32_t(i + 1));
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
                                _bossKilledEntries.insert(entry);
                                if (std::find(_bossKills.begin(), _bossKills.end(), name) == _bossKills.end())
                                    _bossKills.push_back(name);
                            }
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
    if (result.BossesTotal != 0)
    {
        out << " bosses=" << result.BossesKilled.size() << "/" << result.BossesTotal << " killed=";
        for (std::size_t i = 0; i < result.BossesKilled.size(); ++i)
            out << (i ? "," : "") << result.BossesKilled[i];
        if (result.BossesKilled.empty())
            out << "none";
    }
    return out.str();
}
