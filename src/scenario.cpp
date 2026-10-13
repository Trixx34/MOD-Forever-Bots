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
    for (auto const& [guid, dead] : _everDead)
        if (dead)
            ++result.Deaths;
    _result = result;
    _phase = ScenarioPhase::Done;
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
    for (BotSnapshot const& bot : bots)
        if (bot.health == 0)
            _everDead[bot.guid] = true;
    auto const aliveCount = [&bots]
    {
        std::uint32_t alive = 0;
        for (BotSnapshot const& bot : bots)
            alive += bot.health != 0;
        return alive;
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
                        if (bot.guid != leader->guid && bot.map_id != _plan.EntryMap && bot.health != 0)
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
            if (leader && leader->health != 0)
            {
                for (std::size_t i = 0; i < _plan.Route.size(); ++i)
                    if (Distance2d(*leader, _plan.Route[i]) <= kRoutePointReachYards)
                        _furthest = std::max<std::uint32_t>(_furthest, std::uint32_t(i + 1));
            }
            if (!_plan.Route.empty() && _furthest >= _plan.Route.size())
                Finish("complete", nowMs);
            else if (aliveCount() == 0)
                Finish("wipe", nowMs);
            else if (nowMs - _runStartMs >= _plan.DurationMs)
                Finish("timeout", nowMs);
            break;
        }

        case ScenarioPhase::Done:
            break;
    }

    if (_result)
        _result->Alive = aliveCount();
}

std::string FormatScenarioResult(ScenarioResult const& result)
{
    std::ostringstream out;
    out << "scenario " << result.Name << ": outcome=" << result.Outcome << " furthest=" << result.Furthest << "/" << result.RouteSize
        << " deaths=" << result.Deaths << " alive=" << result.Alive << "/" << result.RosterSize << " run=" << result.RunMs / 1000 << "s"
        << " party=" << (result.PartyFormed ? "yes" : "no");
    return out.str();
}
