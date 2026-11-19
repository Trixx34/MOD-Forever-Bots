// SPDX-License-Identifier: GPL-2.0-or-later
// BotPolicy: the survival system's ghost and map parts (2026-11-19, doc/survival.md). SafeRevive (default off) keeps a solo ghost from reviving beside a live hostile
// SafeRevive.LevelMargin (3) or more levels above it, or at a place where bots of its level band keep dying (the shared danger map): it is moved as a ghost (the core
// never refuses a ghost's teleport, and nothing can follow it) to another graveyard of its own map, or waits. DangerMap (default off) notes every open-world death of
// every bot in the shared map (danger_map.h) and keeps grind spots, quest walks and explore walks out of the cells that are dangerous for the bot's band.
//
// Arm C (2026-11-19): the Raven Hill loop (bots 235 and 66, level 8-10, 481 deaths to a level 24 Skeletal Horror, entry 202, standing at the Duskwood spirit healer)
// and the Skyborne killers (entries 251707, 254626, 251245, 256092: 1,692 deaths on map 2991). Every move stays on the bot's map (the Skyborne rule).
#include "bot_policy_internal.h"

#include <cstdio>
#include <string_view>

namespace
{
std::string Pos(float x, float y)
{
    return std::to_string(int(x)) + "," + std::to_string(int(y));
}

std::string Rate(float rate)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f", rate);
    return text;
}
}

bool BotPolicy::SafeReviveHeld(DecideTick& tick, BotTurn& turn, std::uint32_t map, Point const& at, char const* what)
{
    if (!Opt().SafeRevive.Enabled)
        return false;
    std::uint64_t const nowMs = tick.nowMs;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    if (bot.life_state != 2 || bot.level == 0 || state.SafeReviveGaveUp)
        return false;
    // Solo ghosts in the open world only: a party's and a dungeon's ghosts have their own rules (the leader's revive, the entrance).
    bool const instanceParty = IsRouteActive(bot.guid) || (turn.deadParty != 0 && tick.tankLed.count(turn.deadParty) != 0);
    if (instanceParty || bot.group_leader_guid != 0 || Opt().DungeonsByMap.count(map) != 0)
        return false;
    NearbyUnit const* high = nullptr;
    for (NearbyUnit const& unit : turn.nearby)
        if (IsLiveMob(unit) && unit.level >= bot.level + Opt().SafeRevive.LevelMargin &&
            std::hypot(unit.pos_x - at.X, unit.pos_y - at.Y) <= kSafeReviveHostileYards && (!high || unit.level > high->level))
            high = &unit;
    float const rate = _dangerMap.Rate(map, at.X, at.Y, bot.level, nowMs);
    bool const danger = _dangerMap.Dangerous(map, at.X, at.Y, bot.level, nowMs);
    if (!high && !danger)
        return false;
    std::string const who = "safe_revive: bot=" + std::to_string(bot.guid) + " level=" + std::to_string(bot.level) + " map=" + std::to_string(map);
    if (state.SafeReviveSinceMs == 0)
        state.SafeReviveSinceMs = nowMs;
    if (nowMs - state.SafeReviveSinceMs >= kSafeReviveMaxWaitMs)
    {
        // Bounded: a ghost never stays a ghost for good (an elite that never moves off the only graveyard known).
        state.SafeReviveGaveUp = true;
        LogEvent(who + " stage=give_up what=" + what + " at=" + Pos(at.X, at.Y) + " waited_s=" + std::to_string((nowMs - state.SafeReviveSinceMs) / 1000));
        return false;
    }
    bool const logDue = state.SafeReviveLoggedMs == 0 || nowMs - state.SafeReviveLoggedMs >= kSafeReviveLogMs;
    if (logDue)
    {
        state.SafeReviveLoggedMs = nowMs;
        LogEvent(who + " stage=held what=" + what + " reason=" + (high ? "hostile" : "danger") + " at=" + Pos(at.X, at.Y) +
                 (high ? " entry=" + std::to_string(forever_module::UnitRefEntry(high->guid)) + " hostile_level=" + std::to_string(high->level) : std::string()) +
                 " deaths_h=" + Rate(rate));
        if (danger)
            _dangerMap.NoteSkip("revive");
    }
    // A graveyard held is marked unsafe for this level a while, so this bot (and the level-safe choice of others) passes it over.
    bool const atYard = std::string_view(what) == "spirit_healer" ||
                        (map == state.GhostSpawnMap && state.GhostSinceMs != 0 && std::hypot(at.X - state.GhostSpawn.X, at.Y - state.GhostSpawn.Y) <= kGhostFallbackHealerYards);
    if (atYard)
        MarkYardUnsafe(map, at.X, at.Y, bot.level, nowMs, high ? kSafeReviveHostileUnsafeMs : kSafeReviveDangerUnsafeMs);
    // Another graveyard of the same map and side (never another map: the Skyborne rule), not unsafe, not dangerous, not this one: the ghost fallback takes the ghost
    // there (DeadGhostFallback's teleport to LevelSafeYard) and asks the spirit healer there.
    std::uint8_t const side = Opt().SideOf ? Opt().SideOf(bot.guid) : 0;
    std::uint32_t const level = bot.level;
    auto const skip = [&](level_safe::Graveyards::Yard const& yard)
    {
        return YardUnsafe(yard.Map, yard.X, yard.Y, level, nowMs) || _dangerMap.Dangerous(yard.Map, yard.X, yard.Y, level, nowMs) ||
               std::hypot(yard.X - at.X, yard.Y - at.Y) <= level_safe::Graveyards::kSameYards;
    };
    level_safe::Graveyards::Yard const* yard = nullptr;
    if (!_levelMap.Empty())
        yard = _graveyards.Fitting(_levelMap, map, at.X, at.Y, level, Opt().LevelSafe.MarginLevels, side, skip);
    if (!yard)
        yard = _graveyards.Nearest(map, at.X, at.Y, side, skip);
    bool const already = yard && state.LevelSafeYardSet && state.LevelSafeYardMap == yard->Map &&
                         std::hypot(state.LevelSafeYard.X - yard->X, state.LevelSafeYard.Y - yard->Y) <= level_safe::Graveyards::kSameYards;
    if (yard && yard->Map == map && !already)
    {
        state.LevelSafeYardSet = true;
        state.LevelSafeCorpseHigh = true;   // no corpse run back into it
        state.LevelSafeJudged = true;
        state.LevelSafeYardMap = yard->Map;
        state.LevelSafeYard = Point{ yard->X, yard->Y, yard->Z };
        state.GhostFallbackTeleportMs = 0;
        LogEvent(who + " stage=yard from=" + Pos(at.X, at.Y) + " to=" + Pos(yard->X, yard->Y) + " dist=" + std::to_string(int(std::hypot(yard->X - at.X, yard->Y - at.Y))) +
                 " known=" + std::to_string(_graveyards.Size()));
    }
    else if (!yard && logDue)
        LogEvent(who + " stage=wait known=" + std::to_string(_graveyards.Size()) + " max_wait_s=" + std::to_string(kSafeReviveMaxWaitMs / 1000));
    if (!state.GhostFallbackActive)
    {
        // A held reclaim (or a spirit healer asked from the corpse): the ghost goes to the graveyard and its spirit healer instead.
        state.GhostFallbackActive = true;
        LogEvent("ghost_fallback: bot=" + std::to_string(bot.guid) + " reason=safe_revive map=" + std::to_string(bot.map_id) + " corpse_valid=" +
                 std::to_string(bot.corpse_valid) + " ghost_s=" + std::to_string(state.GhostSinceMs != 0 ? (nowMs - state.GhostSinceMs) / 1000 : 0));
    }
    DeadEmit(tick, turn, Decision{ ActionType::kStop, 0 });
    return true;
}

// Just died (DeadRecord): an open-world death goes into the shared map (with DangerMap or SafeRevive on in either arm). A cell that just turned dangerous for the dead bot's band
// is logged once an hour.
void BotPolicy::DangerNoteDeath(DecideTick& tick, BotTurn& turn, std::uint32_t killerEntry)
{
    if (!DangerMapOn())   // either arm (ArmSplit): the map holds the deaths of both arms, as it would with the keys on for every bot
        return;
    BotSnapshot const& bot = turn.bot;
    bool const instanceParty = IsRouteActive(bot.guid) || (turn.deadParty != 0 && tick.tankLed.count(turn.deadParty) != 0);
    if (instanceParty || Opt().DungeonsByMap.count(bot.map_id) != 0 || (Opt().RaidRules && Opt().RaidRules->Find(bot.map_id)))
        return;
    std::optional<danger_map::HotCell> const hot = _dangerMap.NoteDeath(bot.map_id, bot.pos_x, bot.pos_y, bot.level, killerEntry, tick.nowMs);
    if (hot)
        LogEvent("danger_cell: map=" + std::to_string(hot->Map) + " cell=" + Pos(hot->X, hot->Y) + " band=" + danger_map::BandName(hot->Band) + " deaths_h=" +
                 Rate(hot->Rate) + " deaths=" + std::to_string(hot->Deaths) + " top_entry=" + std::to_string(hot->TopEntry) + " top_share=" +
                 std::to_string(hot->TopShare) + " bot=" + std::to_string(bot.guid) + " level=" + std::to_string(bot.level));
}

// After LevelSafeQuestWalk: a quest walk (to a giver, an ender or a hunting area) whose end or straight way passes a dangerous cell for the bot's band is dropped, the way
// the level-safe check drops one (the planner moves on to the next area or blocks the quest a while). A Skyborne exit step (the only way off map 2991) is never dropped.
void BotPolicy::DangerQuestWalk(DecideTick& tick, BotTurn& turn)
{
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    QuestStep& step = turn.questStep;
    std::uint64_t const nowMs = tick.nowMs;
    if (!Opt().DangerMap.Enabled || _dangerMap.Cells() == 0 || bot.level == 0 || (step.Kind != QuestStepKind::WalkTo && step.Kind != QuestStepKind::Hunt) ||
        step.HopLeg)
        return;
    Point const dest{ step.Dest.X, step.Dest.Y, step.Dest.Z };
    bool const same = state.DangerWalkQuest == step.Quest && std::hypot(state.DangerWalkDest.X - dest.X, state.DangerWalkDest.Y - dest.Y) < 1.0f;
    if (same && nowMs - state.DangerWalkJudgedMs < kDangerWalkJudgeMs)
        return; // judged clear a moment ago
    std::optional<danger_map::Map::Hit> const hit =
        _dangerMap.FirstDangerous(bot.map_id, bot.pos_x, bot.pos_y, dest.X, dest.Y, bot.level, nowMs, kDangerSkipNearYards);
    if (!hit)
    {
        state.DangerWalkQuest = step.Quest;
        state.DangerWalkDest = dest;
        state.DangerWalkJudgedMs = nowMs;
        return;
    }
    state.DangerWalkJudgedMs = 0;
    QuestsFor(bot).NoteLevelUnsafe(state.Quest, step.Quest, step.Kind, nowMs);
    _dangerMap.NoteSkip("quest");
    if (_dangerLog.Due(bot.guid, step.Quest, "danger", nowMs, Opt().LevelSafe.LogThrottleMs))
        LogEvent("quest_walk_blocked: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) + " quest=" + std::to_string(step.Quest) +
                 " reason=danger kind=" + (step.Kind == QuestStepKind::Hunt ? "hunt" : step.StartGiver ? "giver" : "hand_in") + " level=" + std::to_string(bot.level) +
                 " deaths_h=" + Rate(hit->Rate) + " at=" + Pos(hit->X, hit->Y) + " dest=" + Pos(dest.X, dest.Y) + " top_entry=" +
                 std::to_string(_dangerMap.TopKiller(bot.map_id, hit->X, hit->Y)));
    step = {};
}

// GrindSpotsForBot: the spots in a dangerous cell for the bot's band are left out, unless that leaves none (the bot then grinds as before rather than standing).
std::vector<std::size_t> BotPolicy::DangerFilterSpots(BotSnapshot const& bot, std::vector<std::size_t> spots, std::uint64_t nowMs)
{
    if (_dangerMap.Cells() == 0 || bot.level == 0 || !Opt().Grind)
        return spots;
    std::vector<std::size_t> kept;
    kept.reserve(spots.size());
    for (std::size_t const index : spots)
    {
        GrindSpot const& spot = Opt().Grind->Spots[index];
        if (_dangerMap.Dangerous(spot.Map, spot.X, spot.Y, bot.level, nowMs))
            _dangerMap.NoteSkip("grind");
        else
            kept.push_back(index);
    }
    if (kept.empty())
    {
        _dangerMap.NoteSkip("grind_all_hot");
        return spots;
    }
    return kept;
}
