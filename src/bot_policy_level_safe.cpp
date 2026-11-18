// SPDX-License-Identifier: GPL-2.0-or-later
// BotPolicy: level-safe places and below-map recovery (2026-11-18, doc/level-safe.md; level_safe.h, below_map.h). The escape of a solo bot standing in a place far
// above its level, the check of a quest walk, the choice of a level-fitting graveyard for a ghost, and the teleport of a bot that fell below the map.
#include "bot_policy_internal.h"

namespace
{
constexpr std::uint64_t kLevelEscapeCheckMs = 5000; // LevelSafeEscape: a bot in a fitting place is looked at again after this
}

// LevelSafeEscape (level_safe.h): a solo bot, alive and out of combat, standing in a place far above its level (the lowest creature level of the grind spots
// around it above its level plus LevelSafe.EscapeMarginLevels, the wide margin) is put at the nearest grind spot for its level, at most once per LevelSafe.EscapeCooldownMs. The
// 2026-11-18 sim: bot 13, level 6, revived at the Raven Hill graveyard (Duskwood, level 20-26) 941 times; bots of level 5 and 6 in Feralas and the Wetlands.
// The core refuses a teleport in combat: a bot fighting there is not moved (a line says so once a minute); if it dies, its ghost is revived at a graveyard
// that fits its level (LevelSafeGraveyard), which is the escape for a bot that never leaves combat. Not for a party member, a dungeon route, a scenario, a
// chat order, a flight or transport: those are where they are on purpose.
void BotPolicy::LevelUnsafeEscape(DecideTick& tick, BotTurn& turn)
{
    std::uint64_t const nowMs = tick.nowMs;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    if (!_options.LevelSafe.Escape || _levelMap.Empty() || bot.level == 0 || bot.life_state != 0 || turn.partyLeader != 0 || tick.membersOf.count(bot.guid) != 0 || turn.routeOn ||
        turn.stagingHold || state.TankLead || state.ChatStay || state.ChatFollow != 0 || bot.in_flight != 0 || bot.transport_guid != 0 ||
        nowMs < state.LevelEscapeRetryMs || nowMs < state.EscapeUntilMs || !_options.Grind || _options.Grind->Spots.empty())
        return;
    std::uint32_t const margin = std::max(_options.LevelSafe.EscapeMarginLevels, _options.LevelSafe.MarginLevels);
    std::uint32_t const placeMin = _levelMap.PlaceMin(bot.map_id, bot.pos_x, bot.pos_y);
    if (placeMin == 0 || placeMin <= bot.level + margin)
    {
        state.LevelEscapeRetryMs = nowMs + kLevelEscapeCheckMs; // the place is looked up every few seconds, not every tick
        return;
    }
    std::string const where = " level=" + std::to_string(bot.level) + " map=" + std::to_string(bot.map_id) + " from=" + std::to_string(int(bot.pos_x)) + "," +
                              std::to_string(int(bot.pos_y)) + " place_min=" + std::to_string(placeMin);
    if (bot.in_combat != 0)
    {
        if (state.LevelEscapeLoggedMs == 0 || nowMs - state.LevelEscapeLoggedMs >= 60000)
        {
            state.LevelEscapeLoggedMs = nowMs;
            LogEvent("level_unsafe_escape: bot=" + std::to_string(bot.guid) + where + " result=in_combat");
        }
        return;
    }
    state.LevelEscapeRetryMs = nowMs + _options.LevelSafe.EscapeCooldownMs;
    for (std::size_t const index : GrindSpotsForBot(state, bot, nowMs))
    {
        GrindSpot const& spot = _options.Grind->Spots[index];
        // The spot's own range fits the bot; the place around it must not be too high either (a low spot inside a high zone), by the walk margin.
        if (_levelMap.TooHigh(spot.Map, spot.X, spot.Y, bot.level, _options.LevelSafe.MarginLevels))
            continue;
        BotAction away;
        away.bot_guid = bot.guid;
        away.type = ActionType::kTeleport;
        away.spell_id = bot.map_id;
        away.dest_x = spot.X;
        away.dest_y = spot.Y;
        away.dest_z = spot.Z;
        tick.actions.push_back(away);
        state.Last.reset();
        state.GotoActive = false;
        state.Wandering = false;
        state.LevelHoldUntilMs = nowMs + kOffMapHoldMs;
        state.EscapeUntilMs = nowMs + kDeathLoopCooldownMs;
        state.GrindVisited.push_back(index);
        if (state.GrindVisited.size() > kGrindMemory)
            state.GrindVisited.erase(state.GrindVisited.begin());
        LogEvent("level_unsafe_escape: bot=" + std::to_string(bot.guid) + where + " spot=" + std::to_string(index) + " to=" + std::to_string(int(spot.X)) + "," +
                 std::to_string(int(spot.Y)) + " spot_levels=" + std::to_string(spot.MinLevel) + "-" + std::to_string(spot.MaxLevel));
        return;
    }
    LogEvent("level_unsafe_escape: bot=" + std::to_string(bot.guid) + where + " result=no_spot");
}

// LevelSafeWalks (level_safe.h): a quest walk (to a giver, an ender or a hunting area) whose end, or a point of its straight line past the first
// LevelSafe.SkipNearYards, lies in a place far above the bot is dropped: the planner skips the hunting area or blocks the quest (NoteLevelUnsafe) and gives
// another step on the next tick. The verdict for one walk is kept, so a walk is judged once, not every tick. A party member follows its leader (no quest
// step); a dungeon route has none either.
void BotPolicy::LevelSafeQuestWalk(DecideTick& tick, BotTurn& turn)
{
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    QuestStep& step = turn.questStep;
    if (!_options.LevelSafe.Walks || _levelMap.Empty() || bot.level == 0 || (step.Kind != QuestStepKind::WalkTo && step.Kind != QuestStepKind::Hunt))
        return;
    Point const dest{ step.Dest.X, step.Dest.Y, step.Dest.Z };
    bool const same = state.LevelSafeQuest == step.Quest && state.LevelSafeLevel == bot.level &&
                      std::hypot(state.LevelSafeDest.X - dest.X, state.LevelSafeDest.Y - dest.Y) < 1.0f;
    if (same && !state.LevelSafeBlocked)
        return; // judged clear already
    state.LevelSafeQuest = step.Quest;
    state.LevelSafeDest = dest;
    state.LevelSafeLevel = bot.level;
    level_safe::Options const& o = _options.LevelSafe;
    // The part of the walk still inside the bot's own place is not judged (a bot in a high place may walk out): at least the radius plus 50 yards.
    float const skipNear = std::max(o.SkipNearYards, o.RadiusYards + 50.0f);
    // A hand-in walk (to the ender of a quest in the log) takes the wide margin, as the escape does: the bot has done the quest, and its ender often stands in a
    // town among guards. A walk to something new (a giver, a hunting area) takes the narrow one.
    bool const handIn = step.Kind == QuestStepKind::WalkTo && !step.StartGiver;
    std::uint32_t const margin = handIn ? std::max(o.EscapeMarginLevels, o.MarginLevels) : o.MarginLevels;
    std::optional<level_safe::LevelMap::Hit> const hit =
        _levelMap.FirstTooHigh(bot.map_id, bot.pos_x, bot.pos_y, dest.X, dest.Y, bot.level, margin, o.StepYards, skipNear);
    state.LevelSafeBlocked = hit.has_value();
    if (!hit)
        return;
    QuestsFor(bot).NoteLevelUnsafe(state.Quest, step.Quest, step.Kind, tick.nowMs);
    LogEvent("quest_walk_blocked: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) + " quest=" + std::to_string(step.Quest) +
             " reason=level_unsafe kind=" + (step.Kind == QuestStepKind::Hunt ? "hunt" : step.StartGiver ? "giver" : "hand_in") + " level=" +
             std::to_string(bot.level) + " margin=" + std::to_string(margin) + " place_min=" + std::to_string(hit->PlaceMin) + " at=" + std::to_string(int(hit->X)) + "," + std::to_string(int(hit->Y)) +
             " dest=" + std::to_string(int(dest.X)) + "," + std::to_string(int(dest.Y)) + " dist=" +
             std::to_string(int(std::hypot(dest.X - bot.pos_x, dest.Y - bot.pos_y))));
    step = {};
}

// LevelSafeGraveyard (level_safe.h): once per death, when the ghost's corpse or the graveyard it appeared at lies far above its level, the nearest graveyard
// seen so far (where other ghosts appeared) whose place fits the bot's level, on its map and for its side, becomes the ghost fallback's graveyard. A corpse
// too high sends the ghost there at once (DeadGhostFallback reason=level_unsafe: the spirit healer, no corpse run); a graveyard too high only changes where a
// later fallback goes. No fitting graveyard known: today's behaviour, and a line says so.
void BotPolicy::LevelSafeGhost(DecideTick& tick, BotTurn& turn)
{
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    if (!_options.LevelSafe.Graveyard || state.LevelSafeJudged || _levelMap.Empty() || bot.level == 0 || bot.life_state != 2 || state.GhostSinceMs == 0)
        return;
    // The corpse may show a tick after the ghost: judged when it is there, or after a few seconds without it.
    if (bot.corpse_valid == 0 && tick.nowMs - state.GhostSinceMs < 5000)
        return;
    state.LevelSafeJudged = true;
    std::uint32_t const margin = _options.LevelSafe.MarginLevels;                                        // a graveyard fits by the narrow margin
    std::uint32_t const wide = std::max(_options.LevelSafe.EscapeMarginLevels, margin);                  // the corpse and the graveyard are too high by the wide one
    bool const corpseHere = bot.corpse_valid != 0 && bot.corpse_map == state.GhostSpawnMap;
    std::uint32_t const yardMin = _levelMap.PlaceMin(state.GhostSpawnMap, state.GhostSpawn.X, state.GhostSpawn.Y);
    std::uint32_t const corpseMin = corpseHere ? _levelMap.PlaceMin(bot.corpse_map, bot.corpse_x, bot.corpse_y) : 0;
    bool const yardHigh = yardMin != 0 && yardMin > bot.level + wide;
    bool const corpseHigh = corpseMin != 0 && corpseMin > bot.level + wide;
    if (!yardHigh && !corpseHigh)
        return;
    float const nearX = corpseHere ? bot.corpse_x : state.GhostSpawn.X;
    float const nearY = corpseHere ? bot.corpse_y : state.GhostSpawn.Y;
    std::uint8_t const side = _options.SideOf ? _options.SideOf(bot.guid) : 0;
    std::string const line = "level_safe_yard: bot=" + std::to_string(bot.guid) + " level=" + std::to_string(bot.level) + " map=" + std::to_string(state.GhostSpawnMap) +
                             " graveyard=" + std::to_string(int(state.GhostSpawn.X)) + "," + std::to_string(int(state.GhostSpawn.Y)) + " graveyard_min=" +
                             std::to_string(yardMin) + " corpse_min=" + std::to_string(corpseMin) + " reason=" + (corpseHigh ? "corpse_high" : "graveyard_high");
    level_safe::Graveyards::Yard const* yard = _graveyards.Fitting(_levelMap, state.GhostSpawnMap, nearX, nearY, bot.level, margin, side);
    if (!yard)
    {
        LogEvent(line + " result=none_known known=" + std::to_string(_graveyards.Size()));
        return;
    }
    state.LevelSafeYardSet = true;
    state.LevelSafeCorpseHigh = corpseHigh;
    state.LevelSafeYardMap = yard->Map;
    state.LevelSafeYard = Point{ yard->X, yard->Y, yard->Z };
    LogEvent(line + " to=" + std::to_string(int(yard->X)) + "," + std::to_string(int(yard->Y)) + " to_min=" +
             std::to_string(_levelMap.PlaceMin(yard->Map, yard->X, yard->Y)) + " dist=" + std::to_string(int(std::hypot(yard->X - nearX, yard->Y - nearY))));
}

// Below-map recovery (below_map.h): a living bot far below the nearest known place around it, or with a storm of no-route answers for short walks, is
// teleported to the nearest known place (the off-map recovery's kTeleport), at most once per BelowMap.CooldownMs.
BotPolicy::TurnFlow BotPolicy::TurnBelowMap(DecideTick& tick, BotTurn& turn)
{
    if (!_options.BelowMap.Enabled)
        return TurnFlow::kNext;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    std::uint64_t const nowMs = tick.nowMs;
    below_map::Facts facts;
    facts.Alive = bot.life_state == 0;
    facts.InCombat = bot.in_combat != 0;
    facts.Travelling = bot.in_flight != 0 || bot.transport_guid != 0 || (bot.movement_flags & 8u) != 0;
    facts.Held = turn.stagingHold || IsRouteActive(bot.guid) || state.TankLead || tick.tankLed.count(bot.guid) != 0 || (_tamingFn && _tamingFn(bot.guid));
    facts.Z = bot.pos_z;
    off_map::Place const* ground = _knownWorld.Home(bot.map_id, bot.pos_x, bot.pos_y, 0);
    if (ground && std::hypot(ground->X - bot.pos_x, ground->Y - bot.pos_y) <= _options.BelowMap.NearYards)
    {
        facts.HasGround = true;
        facts.GroundZ = ground->Z;
    }
    below_map::Verdict const verdict = below_map::Check(_options.BelowMap, state.BelowMap, facts, nowMs);
    if (verdict != below_map::Verdict::kSendBelow && verdict != below_map::Verdict::kSendStorm)
        return TurnFlow::kNext;
    // z_below: back up to that place. A storm: the nearest place of its side.
    std::uint8_t const side = _options.PartySideOf ? _options.PartySideOf(bot.guid) : (_options.SideOf ? _options.SideOf(bot.guid) : 0);
    off_map::Place const* to = verdict == below_map::Verdict::kSendBelow ? ground : _knownWorld.Home(bot.map_id, bot.pos_x, bot.pos_y, side);
    if (!to)
        return TurnFlow::kNext;
    BotAction action;
    action.bot_guid = bot.guid;
    action.type = ActionType::kTeleport;
    action.spell_id = to->Map;
    action.dest_x = to->X;
    action.dest_y = to->Y;
    action.dest_z = to->Z;
    tick.actions.push_back(action);
    state.Last.reset();
    state.GotoActive = false;
    state.LevelHoldUntilMs = nowMs + kOffMapHoldMs;
    LogEvent("below_map_recover: bot=" + std::to_string(bot.guid) + " reason=" + below_map::VerdictName(verdict) + " map=" + std::to_string(bot.map_id) + " pos=" +
             std::to_string(int(bot.pos_x)) + "," + std::to_string(int(bot.pos_y)) + "," + std::to_string(int(bot.pos_z)) + " ground_z=" +
             (facts.HasGround ? std::to_string(int(facts.GroundZ)) : std::string("none")) + " to=" + std::to_string(int(to->X)) + "," + std::to_string(int(to->Y)) + "," +
             std::to_string(int(to->Z)));
    return TurnFlow::kSkip;
}
