// SPDX-License-Identifier: GPL-2.0-or-later
// BotPolicy::Decide, a bot's turn, first steps: the last action's result, ground effects, death and recovery, roles, talents and gear (the steps DecideBot runs in order; bot_policy.cpp).
#include "bot_policy_internal.h"

BotPolicy::TurnFlow BotPolicy::TurnBegin(DecideTick& tick, BotTurn& turn)
{
    WorldTickSnapshotView const& view = tick.view;
    std::uint64_t nowMs = tick.nowMs;
    std::vector<BotAction>& actions = tick.actions;
    auto& leaderOf = tick.leaderOf;
    std::size_t const i = turn.i;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    // A scenario bot before its party is inside (the director's Waiting to Entering phases): it stays at the staging place. No quest, no grind spot, no
    // death-loop escape, no pull of its own; it fights only what attacks it (or its party). The 2026-10-29 Onyxia runs: the first tank walked to quest
    // 6582 at Emberstrife's den (10321, level 61 elite) and the others grinded the Wyrmbog's Firemanes, 204 pulls and 90 deaths in 15 minutes.
    turn.stagingHold = _stagingHoldFn && _stagingHoldFn(bot.guid);
    auto const& stagingHold = turn.stagingHold;
    ReadResult(state, bot, nowMs);
    if (state.TaxiFlight.OrderedMs != 0)
    {
        taxi::Progress const progress = taxi::Track(state.TaxiFlight, bot.in_flight != 0, nowMs, _options.Taxi.WaitMs);
        if (progress != taxi::Progress::None)
        {
            if (_options.OnEvent)
                _options.OnEvent(progress == taxi::Progress::Landed ? taxi::LandedLine(bot.guid, bot.map_id, bot.pos_x, bot.pos_y, state.TaxiFlight, nowMs)
                                                                    : taxi::TimeoutLine(bot.guid, bot.map_id, progress, state.TaxiFlight, nowMs));
            state.TaxiFlight = taxi::Flight{};
        }
    }
    if (state.LosRepositionTarget != 0 && bot.life_state == 0 && ClassFor(_options, bot.guid) == 5 && state.ForcedHeal == 0)
    {
        // LoS recovery must run before ordinary policy branches can continue early. Do not
        // reposition instead of an active heal: leave recovery pending if a party member is
        // still below the healing threshold.
        std::uint64_t const recoveryParty = leaderOf.count(bot.guid) ? leaderOf.at(bot.guid) : 0;
        bool needsHeal = false;
        if (recoveryParty != 0)
            for (BotSnapshot const& member : view.bots)
                if (leaderOf.count(member.guid) && leaderOf.at(member.guid) == recoveryParty &&
                    member.life_state == 0 && member.max_health != 0)
                {
                    std::uint32_t const threshold = member.guid == recoveryParty ? _options.DungeonTankHealBelowPct : _options.DungeonHealBelowPct;
                    if (std::uint64_t(member.health) * 100 / member.max_health < threshold)
                        needsHeal = true;
                }
        if (!needsHeal)
        {
            NearbyUnit const* losTarget = nullptr;
            for (NearbyUnit const& unit : view.NearbyOf(i))
                if (unit.guid == state.LosRepositionTarget && unit.health != 0)
                {
                    losTarget = &unit;
                    break;
                }
            if (!losTarget)
            {
                state.LosRepositionTarget = 0;
                state.CloserUntilMs = 0;
                state.Last.reset();
            }
            else
            {
                bool const walking = state.Last && state.Last->Type == ActionType::kMoveTo &&
                                     state.Last->Target == state.LosRepositionTarget;
                if (walking && DistanceTo(bot, state.Last->Dest) <= 3.0f)
                {
                    state.LosRepositionTarget = 0;
                    state.CloserUntilMs = 0;
                    state.Last.reset();
                }
                else
                {
                    if (!walking)
                    {
                        float dx = bot.pos_x - losTarget->pos_x;
                        float dy = bot.pos_y - losTarget->pos_y;
                        float const distance = std::hypot(dx, dy);
                        if (distance < 0.1f)
                        {
                            dx = std::cos(bot.orientation);
                            dy = std::sin(bot.orientation);
                        }
                        else
                        {
                            dx /= distance;
                            dy /= distance;
                        }
                        float const moveYards = float(_options.HealerLosMoveYards);
                        Point const dest{ losTarget->pos_x + dx * moveYards,
                                          losTarget->pos_y + dy * moveYards,
                                          losTarget->pos_z };
                        BotAction move;
                        move.bot_guid = bot.guid;
                        move.type = ActionType::kMoveTo;
                        move.dest_x = dest.X;
                        move.dest_y = dest.Y;
                        move.dest_z = dest.Z;
                        actions.push_back(move);
                        state.Last = Decision{ ActionType::kMoveTo, state.LosRepositionTarget, dest };
                        if (_options.OnEvent)
                            _options.OnEvent("healer_dps_los: healer=" + std::to_string(bot.guid) +
                                             " moving_to=" + std::to_string(int(dest.X)) + "," +
                                             std::to_string(int(dest.Y)) + " target=" +
                                             std::to_string(state.LosRepositionTarget));
                    }
                    state.NextCastMs = nowMs + 1000;
                    return TurnFlow::kSkip;
                }
            }
        }
    }
    if (state.RestPct == 0)
    {
        // Traits, drawn once per bot from the seed: the health below which it rests, and how close to its leader it stays put. Without randomness
        // (RandomPct 0) every bot has the shared values.
        state.RestPct = kRestBelowPct;
        state.LeaderYards = kFollowLeaderYards;
        // The first craft of each skill comes some seconds after the bot appears, not for all the bots at once.
        if (_options.RandomPct > 0)
            for (std::size_t skill = 0; skill < kCraftLineCount; ++skill)
                state.CraftRetryAtMs[skill] = nowMs + bot_random::Below(bot_random::Draw(_options.RandomSeed, bot.guid, bot_random::kSaltIdle, 0x300000000ull + skill), 60000);
        if (_options.RandomPct > 0)
        {
            state.RestPct = std::clamp<std::uint32_t>(std::uint32_t(bot_random::Vary(kRestBelowPct, std::max(1u, _options.RandomPct / 3),
                                                                                     bot_random::Draw(_options.RandomSeed, bot.guid, bot_random::kSaltRest, 0))), 70, 98);
            state.LeaderYards = float(bot_random::Vary(std::uint64_t(kFollowLeaderYards * 10.0f), _options.RandomPct,
                                                       bot_random::Draw(_options.RandomSeed, bot.guid, bot_random::kSaltRest, 1))) / 10.0f;
        }
    }
    if (state.OrderAction)
    {
        actions.push_back(*state.OrderAction);
        state.OrderAction.reset();
    }
    state.SeenHealthPct = bot.max_health ? std::uint32_t(std::uint64_t(bot.health) * 100 / bot.max_health) : 100;
    state.SeenPowerPct = bot.max_power ? std::uint32_t(std::uint64_t(bot.power) * 100 / bot.max_power) : 100;
    state.SeenLife = bot.life_state;
    state.SeenInCombat = bot.in_combat != 0;
    return TurnFlow::kNext;
}

BotPolicy::TurnFlow BotPolicy::TurnGroundEffects(DecideTick& tick, BotTurn& turn)
{
    std::uint64_t nowMs = tick.nowMs;
    std::vector<BotAction>& actions = tick.actions;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;

    // ==== Ground-effect avoidance ====
    // Ground effects (SDK 0.19): a bot inside a harmful area walks out of it before it does anything else (a healer in mid-heal included: standing in the fire costs
    // more than the heal). The walk's end point is kept for kAvoidKeepMs so that it does not shift every tick (a move would be sent anew each time). Not for a bot on a
    // flight, dead, or with no effect in view. A spell is harmful when its caster is hostile to the bot (the snapshot's flag), or when it is in the avoid table or the
    // dungeon's list (AvoidSpells of the route).
    if (_options.AvoidGroundEffects && bot.life_state == 0 && bot.in_flight == 0)
    {
        std::vector<ground_avoid::Circle> const circles = HazardCircles(_options, bot);
        bool walking = false;
        if (!circles.empty())
        {
            bool const keep = state.AvoidSetMs != 0 && nowMs < state.AvoidSetMs + kAvoidKeepMs && !ground_avoid::InsideAny(state.AvoidDest.X, state.AvoidDest.Y, circles) &&
                              DistanceTo(bot, Point{ state.AvoidDest.X, state.AvoidDest.Y, bot.pos_z }) > 1.0f;
            if (keep)
                walking = true;
            else if (auto const escape = ground_avoid::EscapePoint(bot.pos_x, bot.pos_y, circles))
            {
                state.AvoidDest = *escape;
                state.AvoidSetMs = nowMs;
                walking = true;
                if (_options.OnEvent && nowMs - state.AvoidLoggedMs >= 10000)
                {
                    state.AvoidLoggedMs = nowMs;
                    _options.OnEvent("avoid_effect: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) + " pos=" + std::to_string(int(bot.pos_x)) + "," +
                                     std::to_string(int(bot.pos_y)) + " effects=" + std::to_string(circles.size()) + " to=" + std::to_string(int(escape->X)) + "," +
                                     std::to_string(int(escape->Y)));
                }
            }
        }
        if (walking)
        {
            Decision const out{ ActionType::kMoveTo, 0, Point{ state.AvoidDest.X, state.AvoidDest.Y, bot.pos_z } };
            if (!state.Last || state.Last->Type != ActionType::kMoveTo || state.Last->Dest.X != out.Dest.X || state.Last->Dest.Y != out.Dest.Y)
            {
                BotAction move;
                move.bot_guid = bot.guid;
                move.type = ActionType::kMoveTo;
                move.dest_x = out.Dest.X;
                move.dest_y = out.Dest.Y;
                move.dest_z = out.Dest.Z;
                actions.push_back(move);
                state.Last = out;
            }
            return TurnFlow::kSkip;
        }
        state.AvoidSetMs = 0;
    }
    return TurnFlow::kNext;
}

void BotPolicy::TurnTargetMemory(DecideTick& tick, BotTurn& turn)
{
    WorldTickSnapshotView const& view = tick.view;
    std::uint64_t nowMs = tick.nowMs;
    auto& leaderOf = tick.leaderOf;
    std::size_t const i = turn.i;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;


    // Layout 13: the core says why the bot lost its last target (0 none, 1 killed, 2 leashed, 3 despawned, 4 another player).
    // A creature that leashed or despawned is not worth pulling again at once, so it is avoided like a refused one;
    // a kill is not, which keeps the bot working the corpses it just made.
    if (state.SeenThreatTarget != bot.threat_target_guid)
    {
        if (state.SeenThreatTarget != 0 && bot.threat_target_guid == 0 && bot.last_target_lost_reason != 0 &&
            bot.last_target_lost_reason != state.LostTargetReason)
        {
            state.LostTargetReason = bot.last_target_lost_reason;
            if (bot.last_target_lost_reason >= 2)
                state.AvoidUntilMs[state.SeenThreatTarget] = nowMs + kAvoidMs;
        }
        state.SeenThreatTarget = bot.threat_target_guid;
    }
    for (auto it = state.AvoidUntilMs.begin(); it != state.AvoidUntilMs.end();)
        it = nowMs >= it->second ? state.AvoidUntilMs.erase(it) : std::next(it);

    turn.nearby = view.NearbyOf(i);
    auto& nearby = turn.nearby;
    auto const ledBy = leaderOf.find(bot.guid);
    turn.deadParty = ledBy == leaderOf.end() ? 0 : ledBy->second;
    auto const& deadParty = turn.deadParty;
}

BotPolicy::TurnFlow BotPolicy::TurnDead(DecideTick& tick, BotTurn& turn)
{
    WorldTickSnapshotView const& view = tick.view;
    std::uint64_t nowMs = tick.nowMs;
    std::vector<BotAction>& actions = tick.actions;
    auto const& indexOf = tick.indexOf;
    auto const& tankLed = tick.tankLed;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto& nearby = turn.nearby;
    auto const& deadParty = turn.deadParty;

    // ==== Dead-bot handling (corpse, resurrect, ghost run) ====
    // A dead bot (a corpse, or a ghost: it has health 1, so only life_state tells) gets its own orders and nothing else. State
    // changes are logged once.
    if (bot.life_state != 0)
    {
        std::uint8_t const was = state.Life;
        auto const event = [&](std::string const& text)
        {
            if (_options.OnEvent)
                _options.OnEvent(text);
        };
        auto const emit = [&](Decision const& wantedDead)
        {
            if (wantedDead.Type == ActionType::kMoveTo && nowMs < state.NoRouteUntilMs &&
                std::hypot(wantedDead.Dest.X - state.NoRouteDest.X, wantedDead.Dest.Y - state.NoRouteDest.Y) < kNoRouteSameYards)
                return; // the core found no route to that place a moment ago
            if (state.Last && state.Last->Type == wantedDead.Type && state.Last->Target == wantedDead.Target &&
                state.Last->Dest.X == wantedDead.Dest.X && state.Last->Dest.Y == wantedDead.Dest.Y && state.Last->Dest.Z == wantedDead.Dest.Z)
                return;
            BotAction action;
            action.bot_guid = bot.guid;
            action.type = wantedDead.Type;
            action.dest_x = wantedDead.Dest.X;
            action.dest_y = wantedDead.Dest.Y;
            action.dest_z = wantedDead.Dest.Z;
            actions.push_back(action);
            state.Last = wantedDead;
        };
        if (was == 0)
        {
            // Just died: the living state is dropped, and how it happened is logged (the killer is the threat target, else the
            // closest live hostile).
            state.DeathMs = nowMs;
            state.ReleaseNow = false;
            state.ReleaseSent = false;
            state.ReleaseHeldLogged = false;
            state.AcceptRetryMs = 0;
            state.AcceptedFrom = 0;
            state.ReclaimRetryMs = 0;
            state.Reclaimed = false;
            state.ReviveRetryMs = 0;
            state.ReviveAttempts = 0;
            state.EntrySinceMs = 0;
            state.RunStarted = false;
            state.Inside = false;
            state.RunFailed = false;
            state.RunTimeoutTeleported = false;
            state.DeathTimesMs[0] = state.DeathTimesMs[1];
            state.DeathTimesMs[1] = state.DeathTimesMs[2];
            state.DeathTimesMs[2] = nowMs;
            state.RunStartDist = 0.0f;
            state.RunProgress = 0;
            state.InFight = false;
            state.AutoShotTarget = 0;
            state.PetMissing = true; // the pet is dismissed with its dead owner: Call Pet after the resurrection
            state.PetDead = false;
            state.PetProbe = false;
            state.PetFought = false;
            state.PetCommanded = false;
            state.Sitting = false;
            state.Approaching = false;
            state.Traveling = false;
            state.Wandering = false;
            state.Idling = false;
            NearbyUnit const* killer = nullptr;
            for (NearbyUnit const& unit : nearby)
                if (unit.guid == bot.threat_target_guid)
                    killer = &unit;
            if (!killer)
                killer = Closest(bot, nearby, IsLiveMob);
            event("died: bot=" + std::to_string(bot.guid) + " life=" + std::to_string(bot.life_state) + " map=" + std::to_string(bot.map_id) +
                  " pos=" + std::to_string(int(bot.pos_x)) + "," + std::to_string(int(bot.pos_y)) +
                  " last_decision=" + (state.Last ? std::to_string(int(state.Last->Type)) : std::string("none")) +
                  " killer_entry=" + (killer ? std::to_string(forever_module::UnitRefEntry(killer->guid)) : std::string("none")) +
                  " in_combat=" + std::to_string(bot.in_combat));
        }
        if (bot.life_state == 2 && was != 2)
        {
            state.RunStarted = false;
            state.Inside = false;
            state.Last.reset();
            event("ghost: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) + " corpse_map=" + std::to_string(bot.corpse_map) +
                  " corpse_valid=" + std::to_string(bot.corpse_valid) + " reclaim_wait_ms=" + std::to_string(bot.reclaim_wait_ms));
        }
        state.Life = bot.life_state;

        // A resurrect offer is accepted at any time, whatever the bot was doing.
        if (bot.resurrect_from != 0 && !(nowMs < state.AcceptIgnoreUntilMs && bot.resurrect_from == state.AcceptIgnoreFrom))
        {
            if (nowMs >= state.AcceptRetryMs)
            {
                BotAction accept;
                accept.bot_guid = bot.guid;
                accept.type = ActionType::kResurrect;
                accept.target_guid = bot.resurrect_from;
                accept.spell_id = 0;
                actions.push_back(accept);
                state.AcceptRetryMs = nowMs + kAcceptRetryMs;
                if (state.AcceptedFrom != bot.resurrect_from)
                {
                    state.AcceptedFrom = bot.resurrect_from;
                    event("resurrect_accept: bot=" + std::to_string(bot.guid) + " from=" + std::to_string(bot.resurrect_from) +
                          " life=" + std::to_string(bot.life_state) + " reason=offer_pending");
                }
            }
            return TurnFlow::kSkip;
        }

        if (bot.life_state == 1)
        {
            if (state.ReleaseSent && nowMs < state.ReleaseSentMs + kReleaseRetryMs)
                return TurnFlow::kSkip;
            std::uint64_t const waited = nowMs - state.DeathMs;
            bool anyAlive = false;
            bool rezzer = false;
            // The core refuses a release only for a corpse that is itself in combat; a fight elsewhere in the party does not hold it (0.36.2: the dungeon
            // ghosts waited out whole fights, 44-87 s, with no rezzer up). A ghost released in a fight waits at the entrance, or goes to the leader once it is quiet.
            bool const fighting = bot.in_combat != 0;
            bool partyFights = fighting;
            if (deadParty != 0)
                tick.ForParty(deadParty, [&](BotSnapshot const& member)
                {
                    if (member.guid == bot.guid || member.life_state != 0)
                        return;
                    anyAlive = true;
                    rezzer = rezzer || IsRezzer(member);
                    partyFights = partyFights || member.in_combat != 0;
                });
            bool const dungeonParty = IsRouteActive(bot.guid) || (deadParty != 0 && tankLed.count(deadParty) != 0);
            // The wait for a rezzer is the same 20 s in a dungeon as outside (the owner's rule of 2026-10-27).
            std::uint64_t const releaseDelayMs = _options.ReleaseDelayMs;
            std::uint64_t const releaseCombatHoldMs = _options.ReleaseCombatHoldMs;
            // The Recover tactic (the core's revive order, as for a ghost): the leader is up and nothing is fighting, so the dead member is stood up at the leader's side after a few
            // seconds instead of waiting for the healer's 10 s cast, a release and a corpse run (the members that released were killed again outside by the wolves of Westfall).
            if (_options.TacticRecover && _options.DungeonReviveOutOfCombat && dungeonParty && deadParty != 0 && deadParty != bot.guid && !partyFights &&
                state.ReviveAttempts < kOutOfCombatReviveTries && waited >= kCorpseReviveDelayMs && bot.resurrect_from == 0)
            {
                auto const leaderAt = indexOf.find(deadParty);
                BotSnapshot const* const leader = leaderAt == indexOf.end() ? nullptr : &view.bots[leaderAt->second];
                if (leader && leader->life_state == 0 && leader->in_combat == 0)
                {
                    if (nowMs >= state.ReviveRetryMs)
                    {
                        BotAction revive;
                        revive.bot_guid = bot.guid;
                        revive.type = ActionType::kRevive;
                        revive.target_guid = PlayerRef(deadParty);
                        revive.dest_x = 0.0f;
                        actions.push_back(revive);
                        state.ReviveAttempts += 1;
                        state.ReviveRetryMs = nowMs + kOutOfCombatReviveRetryMs;
                        event("revive_corpse: bot=" + std::to_string(bot.guid) + " leader=" + std::to_string(leader->guid) + " attempt=" + std::to_string(state.ReviveAttempts) +
                              " waited_ms=" + std::to_string(waited));
                    }
                    return TurnFlow::kSkip;
                }
            }
            char const* reason = nullptr;
            if (!anyAlive)
                reason = "release_all_down";
            else if (!rezzer)
                reason = "release_no_rezzer";
            else if (waited >= releaseDelayMs)
                reason = "release_timeout";
            if (!reason && state.ReleaseNow)
                reason = "release_ordered";
            if (!reason)
                return TurnFlow::kSkip; // waiting for a resurrect
            if (fighting && waited < releaseCombatHoldMs)
            {
                // The core refuses a release in combat, and this corpse is still in combat: held back, capped.
                if (!state.ReleaseHeldLogged)
                {
                    state.ReleaseHeldLogged = true;
                    event("release_held: bot=" + std::to_string(bot.guid) + " reason=combat would_release=" + reason);
                }
                return TurnFlow::kSkip;
            }
            BotAction release;
            release.bot_guid = bot.guid;
            release.type = ActionType::kReleaseSpirit;
            actions.push_back(release);
            state.ReleaseSent = true;
            state.ReleaseSentMs = nowMs;
            event(std::string("release: bot=") + std::to_string(bot.guid) + " reason=" + reason + " waited_ms=" + std::to_string(waited) +
                  " rezzer_up=" + (rezzer ? "yes" : "no") + " alive_members=" + (anyAlive ? "yes" : "no"));
            return TurnFlow::kSkip;
        }

        // A live Onyxian Warder near the corpse, or near a living member of the ghost's party: the ghost stays a ghost. Bringing it back there (the teleport to the entrance or to
        // the leader, which the core answers with a resurrect at 50% health on entering the dungeon where the corpse is; the leader's revive; the corpse reclaim) put it among
        // Fire Novas again 20 s after it died (2026-10-29: 1935 died 20:46:02, back 20:46:22, dead 20:46:38). This is not DungeonReviveOutOfCombat: that option only governs the
        // leader's revive; the 20 s revival came from the entrance teleport of the ghost branch below, which no option stops.
        if (deadParty != 0 && bot.corpse_valid != 0 && (IsRouteActive(bot.guid) || tankLed.count(deadParty) != 0))
        {
            auto const warderNear = [&](BotSnapshot const& at, std::span<NearbyUnit const> units, float x, float y)
            {
                if (at.map_id != bot.corpse_map)
                    return false;
                for (NearbyUnit const& unit : units)
                    if (IsLiveMob(unit) && forever_module::UnitRefEntry(unit.guid) == kOnyxianWarderEntry &&
                        std::hypot(unit.pos_x - x, unit.pos_y - y) <= kReviveWarderYards)
                        return true;
                return false;
            };
            bool held = false;
            tick.ForParty(deadParty, [&](BotSnapshot const& member)
            {
                if (held || member.life_state != 0)
                    return;
                auto const at = indexOf.find(member.guid);
                if (at == indexOf.end())
                    return;
                std::span<NearbyUnit const> const units = view.NearbyOf(at->second);
                held = warderNear(member, units, bot.corpse_x, bot.corpse_y) || warderNear(member, units, member.pos_x, member.pos_y);
            });
            if (held)
            {
                if (_options.OnEvent && nowMs - state.ReviveHeldLoggedMs >= kReviveWarderLogMs)
                {
                    state.ReviveHeldLoggedMs = nowMs;
                    _options.OnEvent("revive_held: bot=" + std::to_string(bot.guid) + " reason=warder_alive yards=" + std::to_string(int(kReviveWarderYards)) +
                                     " waited_ms=" + std::to_string(nowMs - state.DeathMs));
                }
                return TurnFlow::kSkip;
            }
        }

        // Dungeon party, out of combat (DungeonReviveOutOfCombat): a ghost is revived by its leader instead of walking back to its corpse, once the
        // leader is alive and not fighting. The core's revive order puts the bot at the leader's side, or where it fell when the leader is on another
        // map. The ghost waits between tries and walks on only once they are used up (kOutOfCombatReviveTries), or when the leader is not up.
        // Only in the dungeon (the owner's rule of 2026-10-22): a ghost still on the outside map walks to the instance door and is revived on
        // entering it, whatever the leader is doing.
        if (_options.DungeonReviveOutOfCombat && deadParty != 0 && deadParty != bot.guid && bot.in_combat == 0 && state.ReviveAttempts < kOutOfCombatReviveTries &&
            (IsRouteActive(bot.guid) || tankLed.count(deadParty) != 0) && bot.corpse_valid != 0 && bot.map_id == bot.corpse_map)
        {
            auto const leaderAt = indexOf.find(deadParty);
            BotSnapshot const* const leader = leaderAt == indexOf.end() ? nullptr : &view.bots[leaderAt->second];
            if (leader && leader->life_state == 0 && leader->in_combat == 0)
            {
                if (nowMs >= state.ReviveRetryMs)
                {
                    BotAction revive;
                    revive.bot_guid = bot.guid;
                    revive.type = ActionType::kRevive;
                    revive.target_guid = PlayerRef(deadParty);
                    revive.dest_x = 0.0f;
                    actions.push_back(revive);
                    state.ReviveAttempts += 1;
                    state.ReviveRetryMs = nowMs + kOutOfCombatReviveRetryMs;
                    event("revive_out_of_combat: bot=" + std::to_string(bot.guid) + " leader=" + std::to_string(leader->guid) +
                          " attempt=" + std::to_string(state.ReviveAttempts) + " map=" + std::to_string(bot.map_id));
                }
                return TurnFlow::kSkip;
            }
        }

        // A ghost: back to the corpse. A corpse on another map (an instance, a ghost is released outside) is reached through the portal.
        if (state.RunFailed)
            return TurnFlow::kSkip;
        auto const fail = [&](char const* why)
        {
            state.RunFailed = true;
            emit(Decision{ ActionType::kStop, 0 });
            event(std::string("corpse_run_failed: bot=") + std::to_string(bot.guid) + " reason=" + why + " map=" + std::to_string(bot.map_id) +
                  " corpse_map=" + std::to_string(bot.corpse_map) + " run_ms=" + std::to_string(state.RunStarted ? nowMs - state.RunStartMs : 0));
        };
        if (!bot.corpse_valid)
        {
            fail("no_corpse");
            return TurnFlow::kSkip;
        }
        // A ghost of a dungeon party goes back to the instance entrance and is revived there (the owner's rule of 2026-10-25), not walked back to its corpse.
        // Outside the instance it is teleported in, and inside it is put at the entrance when it is further off. It waits there for the leader's revive
        // (the revive above). Only when no revive comes within kDungeonEntryWaitMs does the ghost fall back to its corpse run.
        auto const entryOf = _options.DungeonsByMap.find(bot.corpse_map);
        bool const instanceParty = IsRouteActive(bot.guid) || (deadParty != 0 && tankLed.count(deadParty) != 0);
        if (instanceParty && entryOf != _options.DungeonsByMap.end() && entryOf->second.HasEntry && state.EntrySinceMs == 0)
            state.EntrySinceMs = nowMs;
        bool const atEntry = bot.map_id == bot.corpse_map && entryOf != _options.DungeonsByMap.end() && entryOf->second.HasEntry &&
                             DistanceTo(bot, entryOf->second.Entry) <= kEntryYards;
        // Out of combat with the party's leader up: the ghost does not wait at the entrance or walk to its corpse. It is teleported to the leader and stays
        // by it for the revive (above). A fight in the party brings back the entrance wait below.
        auto const leaderAt = deadParty != 0 && deadParty != bot.guid ? indexOf.find(deadParty) : indexOf.end();
        if (instanceParty && leaderAt != indexOf.end() && view.bots[leaderAt->second].life_state == 0 && !PartyFighting(tick, deadParty))
        {
            BotSnapshot const& leader = view.bots[leaderAt->second];
            bool const withLeader = bot.map_id == leader.map_id && DistanceTo(bot, Point{ leader.pos_x, leader.pos_y, leader.pos_z }) <= kEntryYards;
            if (!withLeader && nowMs >= state.CorpseTeleportRetryMs)
            {
                state.CorpseTeleportRetryMs = nowMs + kEntryTeleportRetryMs;
                BotAction toLeader;
                toLeader.bot_guid = bot.guid;
                toLeader.type = ActionType::kTeleport;
                toLeader.spell_id = leader.map_id;
                toLeader.dest_x = leader.pos_x;
                toLeader.dest_y = leader.pos_y;
                toLeader.dest_z = leader.pos_z;
                actions.push_back(toLeader);
                event("party_teleport: bot=" + std::to_string(bot.guid) + " leader=" + std::to_string(leader.guid) + " map=" + std::to_string(bot.map_id) +
                      " leader_map=" + std::to_string(leader.map_id) + " waited_ms=" + std::to_string(nowMs - state.DeathMs));
            }
            return TurnFlow::kSkip;
        }
        if (instanceParty && entryOf != _options.DungeonsByMap.end() && entryOf->second.HasEntry && nowMs - state.EntrySinceMs < kDungeonEntryWaitMs)
        {
            if (!atEntry && nowMs >= state.CorpseTeleportRetryMs)
            {
                state.CorpseTeleportRetryMs = nowMs + kEntryTeleportRetryMs;
                BotAction entry;
                entry.bot_guid = bot.guid;
                entry.type = ActionType::kTeleport;
                entry.spell_id = bot.corpse_map;
                entry.dest_x = entryOf->second.Entry.X;
                entry.dest_y = entryOf->second.Entry.Y;
                entry.dest_z = entryOf->second.Entry.Z;
                actions.push_back(entry);
                event("dungeon_entry_teleport: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) + " corpse_map=" + std::to_string(bot.corpse_map) +
                      " waited_ms=" + std::to_string(nowMs - state.EntrySinceMs));
            }
            return TurnFlow::kSkip; // at the entrance it waits for the leader's revive (the revive above); the corpse run is not walked
        }
        if (instanceParty && entryOf != _options.DungeonsByMap.end() && entryOf->second.HasEntry && !state.RunStarted)
            event("dungeon_entry_wait_expired: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) + " waited_ms=" + std::to_string(nowMs - state.EntrySinceMs));
        if (!state.RunStarted)
        {
            state.RunStarted = true;
            state.RunStartMs = nowMs;
            event("corpse_run_start: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) + " corpse_map=" +
                  std::to_string(bot.corpse_map) + " dist=" + (bot.map_id == bot.corpse_map ? std::to_string(int(DistanceTo(bot, Point{ bot.corpse_x, bot.corpse_y, bot.corpse_z }))) : std::string("other_map")));
        }
        // A corpse run that takes longer than CorpseRunTeleportMs (the owner's rule of 2026-10-25: 60 s; the sim's wiped Deadmines party stood four minutes at the
        // Sentinel Hill graveyard, its walk to the door refused for "no route"): the ghost is put at its corpse now, the same teleport as the four-minute timeout
        // below, brought forward. A ghost already at its corpse (waiting out the core's reclaim delay, or a hostile beside it) is not on a run any more: left alone.
        bool const atCorpse = bot.map_id == bot.corpse_map && DistanceTo(bot, Point{ bot.corpse_x, bot.corpse_y, bot.corpse_z }) <= kReclaimYards;
        if (_options.CorpseRunTeleportMs != 0 && !state.RunTimeoutTeleported && !atCorpse && nowMs - state.RunStartMs > _options.CorpseRunTeleportMs)
        {
            state.RunTimeoutTeleported = true;
            state.RunStartMs = nowMs;
            BotAction back;
            back.bot_guid = bot.guid;
            back.type = ActionType::kTeleport;
            back.spell_id = bot.corpse_map;
            back.dest_x = bot.corpse_x;
            back.dest_y = bot.corpse_y;
            back.dest_z = bot.corpse_z;
            actions.push_back(back);
            event("corpse_run_teleport: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) + " corpse_map=" + std::to_string(bot.corpse_map) +
                  " reason=run_limit");
            return TurnFlow::kSkip;
        }
        if (nowMs - state.RunStartMs > _options.CorpseRunTimeoutMs)
        {
            // Four minutes of walking and the corpse is not reached: the ghost is put at the corpse once (the core does not refuse a teleport for a ghost),
            // and the run starts over. A ghost that gave up here stayed a ghost for good (34 bots in 55 minutes on the sim).
            if (!state.RunTimeoutTeleported)
            {
                state.RunTimeoutTeleported = true;
                state.RunStartMs = nowMs;
                BotAction back;
                back.bot_guid = bot.guid;
                back.type = ActionType::kTeleport;
                back.spell_id = bot.corpse_map;
                back.dest_x = bot.corpse_x;
                back.dest_y = bot.corpse_y;
                back.dest_z = bot.corpse_z;
                actions.push_back(back);
                event("corpse_run_teleport: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) + " corpse_map=" + std::to_string(bot.corpse_map) +
                      " reason=timeout");
                return TurnFlow::kSkip;
            }
            fail("timeout");
            return TurnFlow::kSkip;
        }
        Point const corpse{ bot.corpse_x, bot.corpse_y, bot.corpse_z };
        if (bot.map_id == bot.corpse_map)
        {
            float const dist = DistanceTo(bot, corpse);
            if (!state.Inside)
            {
                state.Inside = true;
                state.InsideSinceMs = nowMs;
                state.RunStartDist = std::max(dist, 1.0f);
                event("corpse_map_reached: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) + " dist=" + std::to_string(int(dist)));
            }
            std::uint32_t const done = dist >= state.RunStartDist ? 0u : std::uint32_t((1.0f - dist / state.RunStartDist) * 100.0f);
            for (std::uint32_t mark : { 25u, 50u, 75u })
                if (done >= mark && state.RunProgress < mark)
                {
                    state.RunProgress = mark;
                    event("corpse_run_progress: bot=" + std::to_string(bot.guid) + " pct=" + std::to_string(mark) + " dist=" + std::to_string(int(dist)));
                }
            if (dist <= kReclaimYards)
            {
                emit(Decision{ ActionType::kStop, 0 });
                bool danger = false;
                for (NearbyUnit const& unit : nearby)
                    danger = danger || (IsLiveMob(unit) && rotation::DistanceYards(bot, unit) <= kReclaimDangerYards);
                if (!danger)
                    state.ReclaimDangerSinceMs = 0;
                else if (state.ReclaimDangerSinceMs == 0)
                {
                    state.ReclaimDangerSinceMs = nowMs;
                    event("reclaim_held: bot=" + std::to_string(bot.guid) + " reason=hostile_near_corpse dist=" + std::to_string(int(dist)));
                }
                bool const holdForDanger = danger && nowMs - state.ReclaimDangerSinceMs < kReclaimDangerWaitMs;
                if (bot.reclaim_wait_ms == 0 && nowMs >= state.ReclaimRetryMs && !holdForDanger)
                {
                    BotAction reclaim;
                    reclaim.bot_guid = bot.guid;
                    reclaim.type = ActionType::kReclaimCorpse;
                    actions.push_back(reclaim);
                    state.ReclaimRetryMs = nowMs + kReclaimRetryMs;
                    event("reclaim: bot=" + std::to_string(bot.guid) + " dist=" + std::to_string(int(dist)) + " run_ms=" + std::to_string(nowMs - state.RunStartMs));
                }
            }
            else
                emit(Decision{ ActionType::kMoveTo, 0, corpse });
        }
        else if (PolicyOptions::ScenarioRoute const& own = ScenarioOf(bot.guid); own.HasPortal && bot.map_id == own.PortalMap && bot.corpse_map == own.PortalInsideMap)
            emit(Decision{ ActionType::kMoveTo, 0, own.PortalPoint });
        else if (nowMs >= state.CorpseTeleportRetryMs)
        {
            // No way on foot to the corpse's map (run 9: a member that died in the dungeon was left on the outside map and idled for 45 minutes
            // after the run failed). The core does not refuse a teleport for a ghost (only one in combat), so the ghost is put at its corpse.
            BotAction back;
            back.bot_guid = bot.guid;
            back.type = ActionType::kTeleport;
            back.spell_id = bot.corpse_map;
            back.dest_x = bot.corpse_x;
            back.dest_y = bot.corpse_y;
            back.dest_z = bot.corpse_z;
            actions.push_back(back);
            state.CorpseTeleportRetryMs = nowMs + kCorpseTeleportRetryMs;
            event("corpse_teleport: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) + " corpse_map=" + std::to_string(bot.corpse_map) +
                  " run_ms=" + std::to_string(nowMs - state.RunStartMs));
        }
        return TurnFlow::kSkip;
    }
    return TurnFlow::kNext;
}

void BotPolicy::TurnRevivedAndRoles(DecideTick& tick, BotTurn& turn)
{
    WorldTickSnapshotView const& view = tick.view;
    std::uint64_t nowMs = tick.nowMs;
    std::vector<BotAction>& actions = tick.actions;
    auto const& indexOf = tick.indexOf;
    auto& leaderOf = tick.leaderOf;
    auto const& tankLed = tick.tankLed;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto const& stagingHold = turn.stagingHold;
    auto& nearby = turn.nearby;

    // Alive again after being down: the movement orders are sent afresh, and how it happened is logged.
    if (state.Life != 0)
    {
        if (_options.OnEvent)
            _options.OnEvent("revived: bot=" + std::to_string(bot.guid) + " how=" + (state.AcceptedFrom != 0 ? "resurrect" : state.Reclaimed ? "reclaim" : "unknown") +
                             " down_ms=" + std::to_string(nowMs - state.DeathMs) + " health_pct=" +
                             std::to_string(bot.max_health ? std::uint64_t(bot.health) * 100 / bot.max_health : 0));
        state.Life = 0;
        state.ReleaseNow = false;
        state.Last.reset();
        state.Sitting = false;
        state.NextCastMs = 0;

        // A death loop: the bot stands up among what killed it and dies again (a party member following a leader into a zone far above its level, a
        // grind spot with something strong on it; 470 of 942 deaths in 55 minutes on the sim, 17 bots dying every few seconds). The third death within
        // kDeathLoopMs puts the bot at the nearest grind spot for its level that is not near its corpse.
        if (!stagingHold && _options.Grind && !_options.Grind->Spots.empty() && bot.level != 0 && state.DeathTimesMs[0] != 0 &&
            nowMs - state.DeathTimesMs[0] <= kDeathLoopMs && nowMs >= state.EscapeUntilMs)
        {
            std::uint8_t const side = _options.SideOf ? _options.SideOf(bot.guid) : std::uint8_t(0);
            std::vector<std::size_t> const spots = GrindSpotsFor(*_options.Grind, bot.map_id, bot.level, side, bot.pos_x, bot.pos_y, state.GrindVisited);
            for (std::size_t const index : spots)
            {
                GrindSpot const& spot = _options.Grind->Spots[index];
                if (std::hypot(spot.X - bot.pos_x, spot.Y - bot.pos_y) < kDeathLoopAwayYards)
                    continue;
                BotAction away;
                away.bot_guid = bot.guid;
                away.type = ActionType::kTeleport;
                away.spell_id = bot.map_id;
                away.dest_x = spot.X;
                away.dest_y = spot.Y;
                away.dest_z = spot.Z;
                actions.push_back(away);
                state.EscapeUntilMs = nowMs + kDeathLoopMs;
                state.Wandering = false;
                state.GrindVisited.push_back(index);
                if (state.GrindVisited.size() > kGrindMemory)
                    state.GrindVisited.erase(state.GrindVisited.begin());
                if (_options.OnEvent)
                    _options.OnEvent("death_loop_escape: bot=" + std::to_string(bot.guid) + " level=" + std::to_string(bot.level) + " map=" + std::to_string(bot.map_id) +
                                     " from=" + std::to_string(int(bot.pos_x)) + "," + std::to_string(int(bot.pos_y)) + " spot=" + std::to_string(index));
                break;
            }
        }
    }

    turn.lowHealth = bot.max_health > 0 && std::uint64_t(bot.health) * 100 < std::uint64_t(bot.max_health) * kDisengagePct;
    auto const& lowHealth = turn.lowHealth;

    // ==== Party role, dungeon routing and follow logic (alive bots) ====
    // The bot's class: its own (from the class table) or the configured one. Ranged classes approach to range.
    turn.classId = ClassFor(_options, bot.guid);
    auto const& classId = turn.classId;
    // A spec in the roster sets the role of a bot that has none: a tank spec tanks, a healer spec heals, a damage spec does not heal on its own (a shadow priest).
    if (!state.SpecRoleApplied && state.Role_ == BotState::Role::Auto && _options.SpecOf)
    {
        state.SpecRoleApplied = true;
        if (std::string const rosterSpec = _options.SpecOf(bot.guid); !rosterSpec.empty())
            if (std::uint32_t const role = rotation::SpecRole(classId, rosterSpec))
                state.Role_ = role == 1 ? BotState::Role::Tank : role == 2 ? BotState::Role::Healer : BotState::Role::Dps;
    }
    turn.spec = SpecFor(bot.guid, classId);
    auto const& spec = turn.spec;

    // The bot's party: a leader is the bot that invites, a member follows its leader. The leader's snapshot is the one
    // this tick's view holds, when the leader is online.
    auto const led = leaderOf.find(bot.guid);
    turn.partyLeader = led == leaderOf.end() ? 0 : led->second;
    auto const& partyLeader = turn.partyLeader;
    turn.partyLeaderBot = partyLeader != 0 && partyLeader == bot.guid;
    auto const& partyLeaderBot = turn.partyLeaderBot;
    turn.partyMember = partyLeader != 0 && !partyLeaderBot;
    auto const& partyMember = turn.partyMember;
    turn.routeOn = IsRouteActive(bot.guid) || (partyLeader != 0 && tankLed.count(partyLeader) != 0);   // the dungeon party rules apply to this bot
    auto const& routeOn = turn.routeOn;
    // A dungeon party's bot on the outside map (a wipe left it at the graveyard, a rescue teleport was refused in combat): it goes back
    // through the instance door and starts no fight on the way. The owner's rule of 2026-10-22: a bot of a dungeon party must not be
    // able to die outside the dungeon; it comes back as a ghost would and is revived in the dungeon it died in.
    // The route, events, kill priority and portal of the bot's own party's dungeon (one of several, or the scenario's only one).
    turn.scenario = &ScenarioOf(bot.guid);
    PolicyOptions::ScenarioRoute const& scenario = *turn.scenario;
    turn.returningToDungeon = routeOn && scenario.HasPortal && scenario.PortalInsideMap != 0 &&
                                   bot.map_id != scenario.PortalInsideMap && bot.in_combat == 0;
    auto const& returningToDungeon = turn.returningToDungeon;
    turn.leaderRef = partyLeader != 0 ? PlayerRef(partyLeader) : 0;
    auto const& leaderRef = turn.leaderRef;
    turn.leaderSnap = nullptr;
    auto& leaderSnap = turn.leaderSnap;
    if (partyMember)
    {
        if (auto const at = indexOf.find(partyLeader); at != indexOf.end())
            leaderSnap = &view.bots[at->second];
    }
    bool const atLeader = leaderSnap && Distance2d(bot, *leaderSnap) <= (state.FollowYards > 0.0f ? state.FollowYards : state.LeaderYards);

    // Who an idle bot follows: a member its leader (and stays put once close). A bot nobody has invited into a
    // group does not follow a nearby player just because it is close (that made every uncontrolled bot latch
    // onto whichever real player walked past it); it only follows when told to (ChatFollow, below) or led.
    turn.followRef = 0;
    auto& followRef = turn.followRef;
    if (partyMember && leaderSnap)
        followRef = atLeader ? 0 : leaderRef;
    // A leader revived at the instance entrance rests there before it goes back to its party (leader_recover_wait, below): the members deep in the dungeon stay where they are,
    // they do not walk back through the packs to it.
    if (followRef != 0 && routeOn && _options.DungeonTeleport && leaderSnap && leaderSnap->life_state == 0)
        if (auto const dungeon = _options.DungeonsByMap.find(bot.map_id);
            dungeon != _options.DungeonsByMap.end() && dungeon->second.HasEntry && DistanceTo(*leaderSnap, dungeon->second.Entry) <= kEntryYards &&
            DistanceTo(bot, dungeon->second.Entry) > kLostLeaderYards)
            followRef = 0;
    if (state.ChatFollow != 0)
    {
        followRef = state.ChatFollow; // told to follow the speaker
        if (state.FollowYards > 0.0f)
        {
            // A follow distance: walk when farther than it, stop within half of it (the speaker has to be in view to tell).
            for (NearbyUnit const& unit : nearby)
                if (unit.guid == state.ChatFollow)
                {
                    float const distanceToSpeaker = rotation::DistanceYards(bot, unit);
                    if (distanceToSpeaker <= state.FollowYards * 0.5f)
                        state.FollowWalking = false;
                    else if (distanceToSpeaker > state.FollowYards)
                        state.FollowWalking = true;
                    if (!state.FollowWalking)
                        followRef = 0;
                    break;
                }
        }
    }
    if (state.ChatStay)
        followRef = 0;                // told to hold the position
    if (state.TankLead)
        followRef = 0;                // leading the group: it walks the route, not after the speaker

    // The party leader waits for a dead member (a corpse that a rezzer can still resurrect, a ghost on its way back), capped.
    turn.deadWaitKind = partyLeader != 0 ? DeadWait(tick, partyLeader) : std::uint8_t(0);
    auto const& deadWaitKind = turn.deadWaitKind;
    if (partyLeaderBot && deadWaitKind != state.DeadWaitKind)
    {
        if (_options.OnEvent)
            _options.OnEvent(std::string("party_wait: leader=") + std::to_string(bot.guid) + " for=" +
                             (deadWaitKind == 1 ? "rez" : deadWaitKind == 2 ? "ghost" : "none") + " waited_ms=" +
                             std::to_string(state.DeadWaitKind != 0 ? nowMs - state.DeadWaitSinceMs : 0));
        if (state.DeadWaitKind == 0)
            state.DeadWaitSinceMs = nowMs;
        state.DeadWaitKind = deadWaitKind;
    }
}

void BotPolicy::TurnTalentsAndGear(DecideTick& tick, BotTurn& turn)
{
    std::uint64_t nowMs = tick.nowMs;
    std::vector<BotAction>& actions = tick.actions;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto const& classId = turn.classId;
    auto const& spec = turn.spec;

    // Talent order: the core spends the bot's points by the profile for its (class, role). Once at level 10 or more, again at each level and
    // when the role changes; out of combat. What Auto means is the same as in the rest of the policy: a warrior in a party tanks, a class
    // with a heal spell heals unless told dps, the rest deal damage. (No respec: a role change after points were spent only affects new points.)
    if (bot.level >= 10 && bot.in_combat == 0 && nowMs >= state.TalentRetryMs && !(_options.IsAlt && _options.IsAlt(bot.guid)))
    {
        std::uint32_t const role = state.Role_ == BotState::Role::Tank ? 1u : state.Role_ == BotState::Role::Healer ? 2u
                                   : state.Role_ == BotState::Role::Dps ? 3u
                                   : (classId == kWarriorClass && _options.PartySize != 0) ? 1u : HealSpellFor(classId, bot.level) != 0 ? 2u : 3u;
        // The spec (roster field race:class:spec, else the default of the class and role) picks the (class, spec) profile first in the core.
        std::uint32_t const spec = static_cast<std::uint32_t>(forever_module::TalentSpecFromKey(classId, SpecFor(bot.guid, classId)));
        if (state.TalentLevel != bot.level || state.TalentRole != role || state.TalentSpec != spec)
        {
            state.TalentLevel = bot.level;
            state.TalentRole = role;
            state.TalentSpec = spec;
            if (_talentOrders.size() < 256)
                _talentOrders.push_back({ bot.guid, role, spec });
        }
    }

    // Auto gear: out of combat, once when the bot first appears (if AutoGearing) and after it has gained some levels, or when asked.
    if (bot.in_combat == 0 && nowMs >= state.GearRetryMs)
    {
        bool const alt = _options.IsAlt && _options.IsAlt(bot.guid);
        bool const due = !alt && _options.AutoGearing && (state.GearedAtLevel == 0 || bot.level >= state.GearedAtLevel + _options.AutoGearEveryLevels);
        if (due || (state.GearRequested && !alt))
        {
            BotAction gear;
            gear.bot_guid = bot.guid;
            gear.type = ActionType::kAutoGear;
            // bit 0: a tank takes a one-hand weapon and a shield over a two-hander (any tank role, not just a warrior --
            // the old `classId == kWarriorClass` check never set this for a protection paladin or a feral tank druid).
            // bits 2-4: the gear stat profile (ModuleBotActions.cpp's PreferredStats), from the bot's resolved spec.
            bool const tankRole = state.Role_ == BotState::Role::Tank ||
                                  (state.Role_ == BotState::Role::Auto && classId == kWarriorClass && _options.PartySize != 0);
            gear.spell_id = (tankRole ? 1u : 0u) | (rotation::GearProfileFor(classId, spec) << 2);
            actions.push_back(gear);
            state.GearedAtLevel = bot.level != 0 ? bot.level : 1;
            state.GearRequested = false;
            state.GearRetryMs = nowMs + 10000;
        }
        else if (!alt && bot.life_state == 0 && (state.BagGearRequested || nowMs >= state.NextBagGearMs))
        {
            // Not tied to an auto gear pass, and it works with AutoGearing off: spell_id 2 is "only wear what is better in the bags".
            // The first look at a bot only sets its schedule.
            if (state.NextBagGearMs != 0 || state.BagGearRequested)
            {
                BotAction gear;
                gear.bot_guid = bot.guid;
                gear.type = ActionType::kAutoGear;
                gear.spell_id = 2u;
                actions.push_back(gear);
            }
            state.BagGearRequested = false;
            state.NextBagGearMs = nowMs + kBagGearEveryMs + (bot.guid % kBagGearSpreadMs / 1000) * 1000;
        }
    }
}
