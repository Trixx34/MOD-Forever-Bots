// SPDX-License-Identifier: GPL-2.0-or-later
// BotPolicy: the death loop breaker (DeathLoopBreaker, default off; doc/level-safe.md). The 2026-11-19 sim (arm C): bots 235 and 66, level 8 to 10 Human hunters, died 260
// and 221 times at the Raven Hill graveyard in Duskwood to one Skeletal Horror (entry 202, level ~24) that stands at the spirit healer. Every death went: release, the
// ghost appears at Raven Hill (the nearest graveyard), the ghost fallback (spirit_healer_far) puts it back there and the spirit healer revives it at 50% health beside the
// Horror, death_loop_escape sends a teleport on the revive tick, but the Horror has the bot in combat by then and the core refuses a teleport in combat: dead again 5 s
// later. LevelSafeGraveyard did not swap that graveyard: its place minimum is 18 (a level 18-22 spot 240 yards off is within the 250 yard radius) and 18 is not above
// level 8 + the wide margin 10.
//
// The breaker acts while the bot is a ghost, which the killer cannot follow and the core never refuses a teleport for: after kLoopBreakDeaths deaths to one creature entry
// within kLoopBreakYards and kLoopBreakWindowMs, the corpse is given up (no corpse run, no reclaim), the loop's graveyard is marked unsafe for the bot's level for
// kLoopBreakUnsafeMs (the level-safe choice of every bot skips it), and the ghost is taken to another graveyard of its map and side (one that fits its level, else the
// nearest one away from the loop) and asks the spirit healer there; not while a live hostile kLoopBreakHighLevels above it stands beside the ghost (that graveyard is
// marked too and the next is tried). No graveyard known: the ghost waits up to kLoopBreakGhostWaitMs, then revives, and the revived bot is put at a grind spot for its
// level away from the loop (teleport plus walk, out of combat only, the death area escape's way). Every move stays on the bot's map (the Skyborne rule: never out of
// map 2991). Event death_loop_break with stage=detect, yard_unsafe, yard, own_yard, wait, wait_over, yard_held, escape, revived_away, in_combat, no_spot.
#include "bot_policy_internal.h"

namespace
{
std::string Pos(float x, float y)
{
    return std::to_string(int(x)) + "," + std::to_string(int(y));
}
}

bool BotPolicy::YardUnsafe(std::uint32_t map, float x, float y, std::uint32_t level, std::uint64_t nowMs) const
{
    for (UnsafeYard const& yard : _unsafeYards)
        if (yard.Map == map && nowMs < yard.UntilMs && level <= yard.Level && std::hypot(yard.X - x, yard.Y - y) <= level_safe::Graveyards::kSameYards)
            return true;
    return false;
}

// `forMs` 0: kLoopBreakUnsafeMs (the loop breaker); SafeRevive marks a graveyard held by a high hostile for a shorter while. A mark never shrinks.
void BotPolicy::MarkYardUnsafe(std::uint32_t map, float x, float y, std::uint32_t level, std::uint64_t nowMs, std::uint64_t forMs)
{
    std::uint64_t const until = nowMs + (forMs != 0 ? forMs : kLoopBreakUnsafeMs);
    std::erase_if(_unsafeYards, [nowMs](UnsafeYard const& yard) { return nowMs >= yard.UntilMs; });
    for (UnsafeYard& yard : _unsafeYards)
        if (yard.Map == map && std::hypot(yard.X - x, yard.Y - y) <= level_safe::Graveyards::kSameYards)
        {
            yard.Level = std::max(yard.Level, level);
            yard.UntilMs = std::max(yard.UntilMs, until);
            return;
        }
    if (_unsafeYards.size() >= kLoopBreakUnsafeMax)
        _unsafeYards.erase(_unsafeYards.begin());
    _unsafeYards.push_back(UnsafeYard{ map, x, y, level, until });
}

// Just died (DeadRecord): the death and its killer are kept; the kLoopBreakDeaths-th death to the same entry within kLoopBreakYards of this one, in the window, is a loop.
// Solo bots only (not in a group, not on a dungeon route): a party's ghosts have their own rules. A bot the party plan holds but that is in no group in the game
// (turn.deadParty set, group_leader_guid 0) is solo.
void BotPolicy::DeathLoopBreakNote(DecideTick& tick, BotTurn& turn, std::uint32_t killerEntry)
{
    std::uint64_t const nowMs = tick.nowMs;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    state.LoopBreak = false;
    state.LoopBreakJudged = false;
    state.LoopBreakWait = false;
    if (!Opt().DeathLoopBreaker || killerEntry == 0 || bot.group_leader_guid != 0 || IsRouteActive(bot.guid))
        return;
    std::erase_if(state.LoopDeaths, [nowMs](BotLoopDeath const& death) { return nowMs - death.AtMs > kLoopBreakWindowMs; });
    state.LoopDeaths.push_back(BotLoopDeath{ bot.map_id, bot.pos_x, bot.pos_y, killerEntry, nowMs });
    if (state.LoopDeaths.size() > kLoopBreakMemory)
        state.LoopDeaths.erase(state.LoopDeaths.begin());
    std::uint32_t here = 0;
    for (BotLoopDeath const& death : state.LoopDeaths)
        if (death.Entry == killerEntry && death.Map == bot.map_id && std::hypot(death.X - bot.pos_x, death.Y - bot.pos_y) <= kLoopBreakYards)
            ++here;
    if (here < kLoopBreakDeaths)
        return;
    state.LoopBreak = true;
    state.LoopBreakEntry = killerEntry;
    state.LoopBreakMap = bot.map_id;
    state.LoopBreakX = bot.pos_x;
    state.LoopBreakY = bot.pos_y;
    LogEvent("death_loop_break: bot=" + std::to_string(bot.guid) + " level=" + std::to_string(bot.level) + " map=" + std::to_string(bot.map_id) + " pos=" +
             Pos(bot.pos_x, bot.pos_y) + " entry=" + std::to_string(killerEntry) + " deaths=" + std::to_string(here) + " stage=detect");
}

// The ghost of a loop break (DeadGhostFallback, before anything else): which graveyard it revives at. True: it took the ghost's turn (it waits, or a graveyard was just
// found held by a high hostile). False: the ghost fallback goes on with reason death_loop (the teleport to LevelSafeYard when set, then the spirit healer).
bool BotPolicy::DeathLoopBreakGhost(DecideTick& tick, BotTurn& turn)
{
    std::uint64_t const nowMs = tick.nowMs;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    if (!Opt().DeathLoopBreaker || !state.LoopBreak || bot.life_state != 2 || state.GhostSinceMs == 0)
        return false;
    std::string const who = "death_loop_break: bot=" + std::to_string(bot.guid) + " level=" + std::to_string(bot.level) + " map=" + std::to_string(state.GhostSpawnMap);
    if (!state.LoopBreakJudged)
    {
        state.LoopBreakJudged = true;
        state.LevelSafeJudged = true; // the breaker chooses this death's graveyard
        state.LevelSafeYardSet = false;
        bool const spawnNear = state.GhostSpawnMap == state.LoopBreakMap &&
                               std::hypot(state.GhostSpawn.X - state.LoopBreakX, state.GhostSpawn.Y - state.LoopBreakY) <= kLoopBreakYards;
        if (spawnNear && !YardUnsafe(state.GhostSpawnMap, state.GhostSpawn.X, state.GhostSpawn.Y, bot.level, nowMs))
        {
            MarkYardUnsafe(state.GhostSpawnMap, state.GhostSpawn.X, state.GhostSpawn.Y, bot.level, nowMs);
            LogEvent(who + " stage=yard_unsafe graveyard=" + Pos(state.GhostSpawn.X, state.GhostSpawn.Y) + " entry=" + std::to_string(state.LoopBreakEntry) +
                     " unsafe_s=" + std::to_string(kLoopBreakUnsafeMs / 1000));
        }
        if (!spawnNear && !YardUnsafe(state.GhostSpawnMap, state.GhostSpawn.X, state.GhostSpawn.Y, bot.level, nowMs))
        {
            // The ghost appeared away from the killer: the corpse is given up and the spirit healer of that graveyard revives it.
            LogEvent(who + " stage=own_yard graveyard=" + Pos(state.GhostSpawn.X, state.GhostSpawn.Y));
            return false;
        }
        // Another graveyard of the same map (never another map: the Skyborne rule) and side, not unsafe for this level and not beside the loop.
        std::uint8_t const side = Opt().SideOf ? Opt().SideOf(bot.guid) : 0;
        std::uint32_t const level = bot.level;
        auto const skip = [&](level_safe::Graveyards::Yard const& yard)
        {
            return YardUnsafe(yard.Map, yard.X, yard.Y, level, nowMs) ||
                   (yard.Map == state.LoopBreakMap && std::hypot(yard.X - state.LoopBreakX, yard.Y - state.LoopBreakY) <= kLoopBreakYards);
        };
        level_safe::Graveyards::Yard const* yard = nullptr;
        if (!_levelMap.Empty() && level != 0)
            yard = _graveyards.Fitting(_levelMap, state.GhostSpawnMap, state.LoopBreakX, state.LoopBreakY, level, Opt().LevelSafe.MarginLevels, side, skip);
        if (!yard)
            yard = _graveyards.Nearest(state.GhostSpawnMap, state.LoopBreakX, state.LoopBreakY, side,
                                       [&](level_safe::Graveyards::Yard const& candidate)
                                       {
                                           return skip(candidate) || std::hypot(candidate.X - state.LoopBreakX, candidate.Y - state.LoopBreakY) < kDeathLoopAwayYards;
                                       });
        if (yard && yard->Map == state.GhostSpawnMap)
        {
            state.LevelSafeYardSet = true;
            state.LevelSafeCorpseHigh = true;
            state.LevelSafeYardMap = yard->Map;
            state.LevelSafeYard = Point{ yard->X, yard->Y, yard->Z };
            LogEvent(who + " stage=yard from=" + Pos(state.GhostSpawn.X, state.GhostSpawn.Y) + " to=" + Pos(yard->X, yard->Y) + " dist=" +
                     std::to_string(int(std::hypot(yard->X - state.LoopBreakX, yard->Y - state.LoopBreakY))) + " known=" + std::to_string(_graveyards.Size()));
            return false;
        }
        state.LoopBreakWait = true;
        LogEvent(who + " stage=wait result=no_safe_yard known=" + std::to_string(_graveyards.Size()) + " wait_s=" + std::to_string(kLoopBreakGhostWaitMs / 1000));
    }
    bool const waitOver = nowMs - state.GhostSinceMs >= kLoopBreakGhostWaitMs;
    if (state.LoopBreakWait)
    {
        if (!waitOver)
        {
            DeadEmit(tick, turn, Decision{ ActionType::kStop, 0 }); // a ghost that waits: no corpse run into the killer
            return true;
        }
        state.LoopBreakWait = false;
        LogEvent(who + " stage=wait_over ghost_s=" + std::to_string((nowMs - state.GhostSinceMs) / 1000));
    }
    // At the graveyard it revives at: not with a live hostile far above the bot beside it (the Horror at Raven Hill). That graveyard is marked and the next one judged.
    std::uint32_t const yardMap = state.LevelSafeYardSet ? state.LevelSafeYardMap : state.GhostSpawnMap;
    Point const yardAt = state.LevelSafeYardSet ? state.LevelSafeYard : state.GhostSpawn;
    if (waitOver || bot.level == 0 || bot.map_id != yardMap || DistanceTo(bot, yardAt) > kGhostFallbackHealerYards)
        return false;
    for (NearbyUnit const& unit : turn.nearby)
    {
        if (!IsLiveMob(unit) || unit.level < bot.level + kLoopBreakHighLevels || rotation::DistanceYards(bot, unit) > kLoopBreakHostileYards)
            continue;
        MarkYardUnsafe(yardMap, yardAt.X, yardAt.Y, bot.level, nowMs);
        state.LoopBreakJudged = false;
        state.GhostFallbackActive = false;
        DeadEmit(tick, turn, Decision{ ActionType::kStop, 0 });
        LogEvent(who + " stage=yard_held graveyard=" + Pos(yardAt.X, yardAt.Y) + " entry=" + std::to_string(forever_module::UnitRefEntry(unit.guid)) + " hostile_level=" +
                 std::to_string(unit.level));
        return true;
    }
    return false;
}

// Revived from a loop break (TurnRevivedAndRoles arms it): a bot still within kDeathLoopAwayYards of the loop (it revived there after the wait) is put at a grind spot for
// its level away from it, and walks there when the teleport is refused (Bot.Actions.Teleport off). Not in combat (the core refuses that): tried again until
// kLoopBreakEscapeMs after the revive. A bot that revived at another graveyard has nothing to do.
void BotPolicy::DeathLoopBreakEscape(DecideTick& tick, BotTurn& turn)
{
    std::uint64_t const nowMs = tick.nowMs;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    if (!Opt().DeathLoopBreaker || state.LoopBreakEscapeUntilMs == 0 || bot.life_state != 0 || nowMs < state.LoopBreakEscapeRetryMs)
        return;
    std::string const who = "death_loop_break: bot=" + std::to_string(bot.guid) + " level=" + std::to_string(bot.level) + " map=" + std::to_string(bot.map_id);
    if (nowMs >= state.LoopBreakEscapeUntilMs)
    {
        state.LoopBreakEscapeUntilMs = 0;
        return;
    }
    if (bot.map_id != state.LoopBreakMap || std::hypot(bot.pos_x - state.LoopBreakX, bot.pos_y - state.LoopBreakY) >= kDeathLoopAwayYards)
    {
        state.LoopBreakEscapeUntilMs = 0;
        LogEvent(who + " stage=revived_away pos=" + Pos(bot.pos_x, bot.pos_y) + " loop=" + Pos(state.LoopBreakX, state.LoopBreakY));
        return;
    }
    if (bot.in_combat != 0)
    {
        if (state.LoopBreakLoggedMs == 0 || nowMs - state.LoopBreakLoggedMs >= 60000)
        {
            state.LoopBreakLoggedMs = nowMs;
            LogEvent(who + " stage=in_combat pos=" + Pos(bot.pos_x, bot.pos_y));
        }
        return;
    }
    state.LoopBreakEscapeUntilMs = 0;
    if (Opt().Grind && !Opt().Grind->Spots.empty() && bot.level != 0)
        for (std::size_t const index : GrindSpotsForBot(state, bot, nowMs))
        {
            GrindSpot const& spot = Opt().Grind->Spots[index];
            if (spot.Map != bot.map_id || std::hypot(spot.X - state.LoopBreakX, spot.Y - state.LoopBreakY) < kDeathLoopAwayYards ||
                (!_levelMap.Empty() && _levelMap.TooHigh(spot.Map, spot.X, spot.Y, bot.level, Opt().LevelSafe.MarginLevels)))
                continue;
            BotAction away;
            away.bot_guid = bot.guid;
            away.type = ActionType::kTeleport;
            away.spell_id = bot.map_id;
            away.dest_x = spot.X;
            away.dest_y = spot.Y;
            away.dest_z = spot.Z;
            tick.actions.push_back(away);
            // The walk there, in case the teleport is refused; a teleport that lands ends it at once.
            state.Last.reset();
            state.GotoActive = true;
            state.GotoPoint = Point{ spot.X, spot.Y, spot.Z };
            state.GotoUntilMs = nowMs + kDeathAreaWalkMs;
            state.TrainAfterGoto = false;
            state.Wandering = false;
            state.EscapeUntilMs = nowMs + kDeathLoopCooldownMs;
            state.AreaEscapeRetryMs = nowMs + kDeathAreaEscapeRetryMs; // not a second move by the death area escape this tick
            state.GrindVisited.push_back(index);
            if (state.GrindVisited.size() > kGrindMemory)
                state.GrindVisited.erase(state.GrindVisited.begin());
            LogEvent(who + " stage=escape from=" + Pos(bot.pos_x, bot.pos_y) + " spot=" + std::to_string(index) + " to=" + Pos(spot.X, spot.Y));
            return;
        }
    LogEvent(who + " stage=no_spot pos=" + Pos(bot.pos_x, bot.pos_y));
}
