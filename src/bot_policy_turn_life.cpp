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
    PartialChainTick(state, bot, nowMs);
    // A level-dungeon or LFG teleport was just ordered: the snapshot still shows the old place, so no other order until it has taken effect.
    if (nowMs < state.LevelHoldUntilMs)
        return TurnFlow::kSkip;
    if (state.TaxiFlight.OrderedMs != 0)
    {
        taxi::Progress const progress = taxi::Track(state.TaxiFlight, bot.in_flight != 0, nowMs, Opt().Taxi.WaitMs);
        if (progress != taxi::Progress::None)
        {
            if (Opt().OnEvent)
                Opt().OnEvent(progress == taxi::Progress::Landed ? taxi::LandedLine(bot.guid, bot.map_id, bot.pos_x, bot.pos_y, state.TaxiFlight, nowMs)
                                                                    : taxi::TimeoutLine(bot.guid, bot.map_id, progress, state.TaxiFlight, nowMs));
            state.TaxiFlight = taxi::Flight{};
        }
    }
    if (state.LosRepositionTarget != 0 && bot.life_state == 0 && ClassFor(Opt(), bot.guid) == 5 && state.ForcedHeal == 0)
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
                    std::uint32_t const threshold = member.guid == recoveryParty ? Opt().DungeonTankHealBelowPct : Opt().DungeonHealBelowPct;
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
                        float const moveYards = float(Opt().HealerLosMoveYards);
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
                        state.LastMove = state.Last;
                        state.LastMoveWhy = "los_reposition";
                        if (Opt().OnEvent)
                            Opt().OnEvent("healer_dps_los: healer=" + std::to_string(bot.guid) +
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
        if (Opt().RandomPct > 0)
            for (std::size_t skill = 0; skill < kCraftLineCount; ++skill)
                state.CraftRetryAtMs[skill] = nowMs + bot_random::Below(bot_random::Draw(Opt().RandomSeed, bot.guid, bot_random::kSaltIdle, 0x300000000ull + skill), 60000);
        if (Opt().RandomPct > 0)
        {
            state.RestPct = std::clamp<std::uint32_t>(std::uint32_t(bot_random::Vary(kRestBelowPct, std::max(1u, Opt().RandomPct / 3),
                                                                                     bot_random::Draw(Opt().RandomSeed, bot.guid, bot_random::kSaltRest, 0))), 70, 98);
            state.LeaderYards = float(bot_random::Vary(std::uint64_t(kFollowLeaderYards * 10.0f), Opt().RandomPct,
                                                       bot_random::Draw(Opt().RandomSeed, bot.guid, bot_random::kSaltRest, 1))) / 10.0f;
        }
        // Personality (off by default): the caution trait moves the rest threshold a few points, within 70..98.
        if (Opt().Personality.Enabled)
            state.RestPct = personality::RestPct(Opt().Personality, personality::TraitsFor(Opt().RandomSeed, bot.guid), state.RestPct);
    }
    if (state.OrderAction)
    {
        actions.push_back(*state.OrderAction);
        state.OrderAction.reset();
    }
    state.SeenHealthPct = HealthPct(bot, 100);
    state.SeenPowerPct = PowerPct(bot, 100);
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
    if (Opt().AvoidGroundEffects && bot.life_state == 0 && bot.in_flight == 0)
    {
        std::vector<ground_avoid::Circle> const circles = HazardCircles(Opt(), bot);
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
                if (Opt().OnEvent && nowMs - state.AvoidLoggedMs >= 10000)
                {
                    state.AvoidLoggedMs = nowMs;
                    Opt().OnEvent("avoid_effect: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) + " pos=" + std::to_string(int(bot.pos_x)) + "," +
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
                state.LastMove = out;
                state.LastMoveWhy = "ground_avoid";
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
    // The per-unit escalation counters were never dropped: one entry for every creature a bot ever avoided (the unit reference is unique per spawn). Past the cap,
    // the counters of units that are not avoided now go (their next avoid starts at the base time again).
    PruneUnitCounters(state.AvoidCount, state.AvoidUntilMs);
    PruneUnitCounters(state.StallCount, state.AvoidUntilMs);

    turn.nearby = view.NearbyOf(i);
    auto& nearby = turn.nearby;
    auto const ledBy = leaderOf.find(bot.guid);
    turn.deadParty = ledBy == leaderOf.end() ? 0 : ledBy->second;
    auto const& deadParty = turn.deadParty;
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
        if (Opt().OnEvent)
            Opt().OnEvent("revived: bot=" + std::to_string(bot.guid) + " how=" + (state.AcceptedFrom != 0 ? "resurrect" : state.Reclaimed ? "reclaim" : "unknown") +
                             " down_ms=" + std::to_string(nowMs - state.DeathMs) + " health_pct=" +
                             std::to_string(HealthPct(bot, 0)));
        state.Life = 0;
        state.RepairAfterDeath = true; // a death wears the gear: worn gear is repaired before the next fight (repair_policy.h)
        state.ReleaseNow = false;
        state.Last.reset();
        state.Sitting = false;
        state.NextCastMs = 0;

        // A death loop: the bot stands up among what killed it and dies again (a party member following a leader into a zone far above its level, a
        // grind spot with something strong on it; 470 of 942 deaths in 55 minutes on the sim, 17 bots dying every few seconds). The third death within
        // kDeathLoopMs, or the second one to the same creature entry within it, puts the bot at the nearest grind spot for its level that is not near its corpse.
        // DeathLoopBreaker: a death the breaker took is moved by DeathLoopBreakEscape (below), not here.
        bool const loopBroken = Opt().DeathLoopBreaker && state.LoopBreak;
        if (loopBroken)
        {
            state.LoopBreak = false;
            state.LoopBreakEscapeUntilMs = nowMs + kLoopBreakEscapeMs;
            state.LoopBreakEscapeRetryMs = 0;
            state.LoopBreakLoggedMs = 0;
        }
        if (!loopBroken && !stagingHold && Opt().Grind && !Opt().Grind->Spots.empty() && bot.level != 0 && nowMs >= state.EscapeUntilMs &&
            ((state.DeathTimesMs[0] != 0 && nowMs - state.DeathTimesMs[0] <= kDeathLoopMs) ||
             (state.KillerRepeat && state.DeathTimesMs[1] != 0 && nowMs - state.DeathTimesMs[1] <= kDeathLoopMs)))
        {
            std::vector<std::size_t> const spots = GrindSpotsForBot(state, bot, nowMs);
            for (std::size_t const index : spots)
            {
                GrindSpot const& spot = Opt().Grind->Spots[index];
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
                state.EscapeUntilMs = nowMs + kDeathLoopCooldownMs;
                state.Wandering = false;
                state.GrindVisited.push_back(index);
                if (state.GrindVisited.size() > kGrindMemory)
                    state.GrindVisited.erase(state.GrindVisited.begin());
                if (Opt().OnEvent)
                    Opt().OnEvent("death_loop_escape: bot=" + std::to_string(bot.guid) + " level=" + std::to_string(bot.level) + " map=" + std::to_string(bot.map_id) +
                                     " from=" + std::to_string(int(bot.pos_x)) + "," + std::to_string(int(bot.pos_y)) + " spot=" + std::to_string(index));
                break;
            }
        }
    }

    turn.lowHealth = bot.max_health > 0 && BelowPct(bot.health, bot.max_health, kDisengagePct);
    auto const& lowHealth = turn.lowHealth;

    // ==== Party role, dungeon routing and follow logic (alive bots) ====
    // The bot's class: its own (from the class table) or the configured one. Ranged classes approach to range.
    turn.classId = ClassFor(Opt(), bot.guid);
    auto const& classId = turn.classId;
    // A spec in the roster sets the role of a bot that has none: a tank spec tanks, a healer spec heals, a damage spec does not heal on its own (a shadow priest).
    if (!state.SpecRoleApplied && state.Role_ == BotState::Role::Auto && Opt().SpecOf)
    {
        state.SpecRoleApplied = true;
        if (std::string const rosterSpec = Opt().SpecOf(bot.guid); !rosterSpec.empty())
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
    // The off-tank: the party's first warrior member after its leader (in guid order, the roster's second warrior; the raid roster's first entries are its
    // warriors). With an Onyxian Warder it does not kite: it picks up a Warder that hits another member, and holds one while the leader is dead.
    turn.offTank = false;
    if (turn.partyMember && ClassFor(Opt(), bot.guid) == kWarriorClass)
        if (auto const members = tick.membersOf.find(partyLeader); members != tick.membersOf.end())
            for (std::uint64_t member : members->second)
                if (ClassFor(Opt(), member) == kWarriorClass)
                {
                    turn.offTank = member == bot.guid;
                    break;
                }
    auto const& partyMember = turn.partyMember;
    turn.routeOn = IsRouteActive(bot.guid) || (partyLeader != 0 && tankLed.count(partyLeader) != 0);   // the dungeon party rules apply to this bot
    auto const& routeOn = turn.routeOn;
    DeathLoopBreakEscape(tick, turn); // DeathLoopBreaker (bot_policy_death_loop.cpp), before the death area escape
    DeathAreaEscape(tick, turn);
    LevelUnsafeEscape(tick, turn); // LevelSafeEscape (bot_policy_level_safe.cpp)
    // A solo bot out of combat that is hurt (a reclaim brings it up at 50% health) rests before it fights or walks to a quest giver, up to kRecoverHoldMs: it is
    // treated as low on health (no target, no quest, away from a creature that is near, then sits). Prod 2026-11-12: revived at 49%, a bot pulled at once and died
    // to the same creature again, 812 deaths in 90 minutes.
    if (partyLeader == 0 && !routeOn && bot.in_combat == 0 && bot.threat_target_guid == 0 && bot.max_health > 0 && BelowPct(bot.health, bot.max_health, pull_ready::SoloRestMarkPct(Opt().SoloPullHpPct, kRecoverPullPct, state.RecoverSinceMs != 0)))
    {
        if (state.RecoverSinceMs == 0)
            state.RecoverSinceMs = nowMs;
        turn.hurtRest = nowMs - state.RecoverSinceMs < kRecoverHoldMs;
    }
    else
    {
        state.RecoverSinceMs = 0;
        turn.hurtRest = false;
    }
    if (turn.hurtRest)
    {
        turn.lowHealth = true;
        if (Opt().OnEvent && pull_ready::LogDue(state.SoloHpGate, nowMs))
            Opt().OnEvent("pull_wait: bot=" + std::to_string(bot.guid) + " reason=hp level=" + std::to_string(bot.level) + " hp_pct=" + std::to_string(HealthPct(bot, 100)) +
                             " mana_pct=" + std::to_string(bot.max_power > 0 ? bot.power * 100 / bot.max_power : 100) + " wait_ms=" + std::to_string(nowMs - state.RecoverSinceMs));
    }
    TurnSoloReady(tick, turn);
    if (turn.hurtRest && Opt().SoloPullHpPct != 0)
        turn.pullWait = true;   // SoloPullHpPct: no self-pull while it rests (the fixed 75 mark only keeps the quest walk and the target away)
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
    // The leader alive on another map: the member cannot follow it (below), and a party member does not grind or wander, so it stood for the whole run.
    bool const leaderAway = partyMember && !routeOn && leaderSnap && leaderSnap->map_id != bot.map_id && leaderSnap->life_state == 0 && bot.life_state == 0 &&
                            state.ChatFollow == 0 && !state.ChatStay;
    if (!leaderAway)
        state.LeaderAwaySinceMs = 0;
    else if (state.LeaderAwaySinceMs == 0)
        state.LeaderAwaySinceMs = nowMs;
    turn.leaderAwaySolo = leaderAway && nowMs - state.LeaderAwaySinceMs >= kLeaderAwaySoloMs;

    // Who an idle bot follows: a member its leader (and stays put once close). A bot nobody has invited into a
    // group does not follow a nearby player just because it is close (that made every uncontrolled bot latch
    // onto whichever real player walked past it); it only follows when told to (ChatFollow, below) or led.
    turn.followRef = 0;
    auto& followRef = turn.followRef;
    // Only a leader on the bot's own map: the core's follow walks towards the leader's coordinates whatever map they are on. In the Stockade (2026-11-07 22:20 and
    // 22:26, stockade#3) the leader's ghost was released to Stormwind (map 0, x about -8700), and its four members walked west out of the prison at 7 yards a second,
    // 100 and 400 yards off the mesh; the leader then rejoined next to one of them at -9,-16, where the route had no path (route_blocked at Targorr's point 0).
    if (partyMember && leaderSnap && leaderSnap->map_id == bot.map_id)
        followRef = atLeader ? 0 : leaderRef;
    // A leader revived at the instance entrance rests there before it goes back to its party (leader_recover_wait, below): the members deep in the dungeon stay where they are,
    // they do not walk back through the packs to it.
    if (followRef != 0 && routeOn && Opt().DungeonTeleport && leaderSnap && leaderSnap->life_state == 0)
        if (auto const dungeon = Opt().DungeonsByMap.find(bot.map_id);
            dungeon != Opt().DungeonsByMap.end() && dungeon->second.HasEntry && DistanceTo(*leaderSnap, dungeon->second.Entry) <= kEntryYards &&
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
        if (Opt().OnEvent)
            Opt().OnEvent(std::string("party_wait: leader=") + std::to_string(bot.guid) + " for=" +
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
    if (bot.level >= 10 && bot.in_combat == 0 && nowMs >= state.TalentRetryMs && !(Opt().IsAlt && Opt().IsAlt(bot.guid)))
    {
        std::uint32_t const role = state.Role_ == BotState::Role::Tank ? 1u : state.Role_ == BotState::Role::Healer ? 2u
                                   : state.Role_ == BotState::Role::Dps ? 3u
                                   : (classId == kWarriorClass && Opt().PartySize != 0) ? 1u : HealSpellFor(classId, bot.level) != 0 ? 2u : 3u;
        // The spec (roster field race:class:spec, else the default of the class and role) picks the (class, spec) profile first in the core.
        std::uint32_t const spec = static_cast<std::uint32_t>(forever_module::TalentSpecFromKey(classId, SpecFor(bot.guid, classId)));
        RespecInputs const respec{ Opt().TalentRespec, bot.level, bot.life_state == 0, bot.in_combat != 0, state.TalentLevel != 0,
                                   state.TalentRole, state.TalentSpec, role, spec, nowMs, state.RespecNotBeforeMs };
        if (RespecDue(respec))
        {
            // The core takes the spent points back and spends them by the new role or spec (kRespecTalents, sent with the actions): no talent
            // order for this change. A refused respec (no gold) leaves the points as they are; the next level-up's order spends the new points.
            state.TalentLevel = bot.level;
            state.TalentRole = role;
            state.TalentSpec = spec;
            state.RespecNotBeforeMs = nowMs + kRespecIntervalMs;
            if (_respecOrders.size() < 256)
                _respecOrders.push_back({ bot.guid, role, spec });
        }
        else if (state.TalentLevel != bot.level || state.TalentRole != role || state.TalentSpec != spec)
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
        bool const alt = Opt().IsAlt && Opt().IsAlt(bot.guid);
        bool const due = !alt && Opt().AutoGearing && (state.GearedAtLevel == 0 || bot.level >= state.GearedAtLevel + Opt().AutoGearEveryLevels);
        if (due || (state.GearRequested && !alt))
        {
            BotAction gear;
            gear.bot_guid = bot.guid;
            gear.type = ActionType::kAutoGear;
            // bit 0: a tank takes a one-hand weapon and a shield over a two-hander (any tank role, not just a warrior --
            // the old `classId == kWarriorClass` check never set this for a protection paladin or a feral tank druid).
            // bits 2-4: the gear stat profile (ModuleBotActions.cpp's PreferredStats), from the bot's resolved spec.
            bool const tankRole = state.Role_ == BotState::Role::Tank ||
                                  (state.Role_ == BotState::Role::Auto && classId == kWarriorClass && Opt().PartySize != 0);
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

// TransportRide: ride with the leader (doc/transports.md). A bot whose leader stands on a ship, zeppelin or elevator is ordered onto it (kTransport spell 3), a bot that
// rides while the leader does not is ordered off (spell 2). A passenger takes no world-coordinate move (turn.transportHold, see DecideBot and TurnSendMove).
void BotPolicy::TurnTransport(DecideTick& tick, BotTurn& turn)
{
    if (!Opt().TransportRide.Enabled)
        return;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    std::uint64_t const nowMs = tick.nowMs;

    transport_ride::Facts facts;
    facts.Alive = bot.life_state == 0;
    facts.InCombat = bot.in_combat != 0;
    facts.InFlight = bot.in_flight != 0;
    facts.Swimming = (bot.movement_flags & transport_ride::kFlagSwimming) != 0;
    facts.InGroup = bot.group_role != 0;
    facts.IsLeader = bot.group_role == 2;
    facts.BotOnTransport = transport_ride::OnTransport(bot.movement_flags, bot.transport_guid);
    facts.BotTransportEntry = std::uint32_t(bot.transport_guid);
    if (facts.InGroup && !facts.IsLeader)
    {
        forever_module::PartyView const* const party = tick.view.PartyViewOf(turn.i);
        if (party)
        {
            for (forever_module::PartyMember const& member : tick.view.MembersOf(*party))
            {
                if (member.guid != party->leader_ref && (member.member_flags & forever_module::kPartyMemberLeader) == 0)
                    continue;
                // The leader's transport counts only while it is on the bot's own map or the bot is to be brought over: the core decides the map.
                facts.LeaderKnown = member.online != 0;
                facts.LeaderTransportEntry = member.transport_entry;
                break;
            }
        }
    }

    // Events when the bot gets on or off a transport.
    bool const riding = facts.BotOnTransport;
    if (riding != state.TransportRiding)
    {
        state.TransportRiding = riding;
        if (Opt().OnEvent)
            Opt().OnEvent(std::string(riding ? "transport_board" : "transport_leave") + ": bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) +
                             " transport=" + std::to_string(bot.transport_guid));
    }

    transport_ride::Order const order = transport_ride::Decide(Opt().TransportRide, facts);
    if (riding)
        turn.transportHold = true;
    if (order == transport_ride::Order::kNone || !transport_ride::Due(nowMs, state.TransportNextMs))
        return;

    BotAction action;
    action.bot_guid = bot.guid;
    action.type = ActionType::kTransport;
    action.spell_id = order == transport_ride::Order::kJoin ? transport_ride::kJoinSpell : transport_ride::kLeaveSpell;
    tick.actions.push_back(action);
    state.TransportNextMs = transport_ride::NextAfter(Opt().TransportRide, nowMs, false);
    if (order == transport_ride::Order::kJoin)
        turn.transportHold = true; // the snapshot still shows the old place: no walk this turn that the boarding would undo
}

// Off-map recovery (off_map.h): a living bot far outside the known places of its map for OffMap.OffForMs is teleported to the nearest one of its faction.
// Not while the policy holds it (a scenario's staging, a dungeon route, tank lead, taming): those move it themselves. A refused teleport is logged by
// ReadResult (teleport_refused) and tried again after OffMap.CooldownMs.
BotPolicy::TurnFlow BotPolicy::TurnOffMap(DecideTick& tick, BotTurn& turn)
{
    if (!Opt().OffMap.Enabled)
        return TurnFlow::kNext;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    std::uint64_t const nowMs = tick.nowMs;
    off_map::Facts facts;
    facts.Map = bot.map_id;
    facts.X = bot.pos_x;
    facts.Y = bot.pos_y;
    facts.Alive = bot.life_state == 0;
    facts.InCombat = bot.in_combat != 0;
    facts.Travelling = bot.in_flight != 0 || bot.transport_guid != 0 || (bot.movement_flags & 8u) != 0;
    facts.Held = turn.stagingHold || IsRouteActive(bot.guid) || state.TankLead || tick.tankLed.count(bot.guid) != 0 || (_tamingFn && _tamingFn(bot.guid));
    off_map::Verdict const verdict = off_map::Check(Opt().OffMap, _knownWorld, state.OffMap, facts, nowMs);
    if (verdict == off_map::Verdict::kWaiting && !state.OffMap.Logged)
    {
        state.OffMap.Logged = true;
        LogEvent("off_map_seen: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) + " pos=" + std::to_string(int(bot.pos_x)) + "," +
                 std::to_string(int(bot.pos_y)) + "," + std::to_string(int(bot.pos_z)));
    }
    if (verdict != off_map::Verdict::kSend)
        return TurnFlow::kNext;
    std::uint8_t const side = Opt().PartySideOf ? Opt().PartySideOf(bot.guid) : (Opt().SideOf ? Opt().SideOf(bot.guid) : 0);
    off_map::Place const* home = _knownWorld.Home(bot.map_id, bot.pos_x, bot.pos_y, side);
    if (!home)
        return TurnFlow::kNext;
    BotAction action;
    action.bot_guid = bot.guid;
    action.type = ActionType::kTeleport;
    action.spell_id = home->Map;
    action.dest_x = home->X;
    action.dest_y = home->Y;
    action.dest_z = home->Z;
    tick.actions.push_back(action);
    state.Last.reset();
    state.GotoActive = false;
    state.LevelHoldUntilMs = nowMs + kOffMapHoldMs;
    LogEvent("off_map_home: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) + " pos=" + std::to_string(int(bot.pos_x)) + "," +
             std::to_string(int(bot.pos_y)) + "," + std::to_string(int(bot.pos_z)) + " to=" + std::to_string(int(home->X)) + "," + std::to_string(int(home->Y)) + "," +
             std::to_string(int(home->Z)) + " off_sec=" + std::to_string((nowMs - state.OffMap.OffSinceMs) / 1000) + " try=" + std::to_string(state.OffMap.Tries));
    return TurnFlow::kSkip;
}

// The death area escape (DeathAreaCount): a solo bot, alive and out of combat, inside the area it avoids is put at a grind spot for its level at least kDeathLoopAwayYards
// from it, and walks there as well (a goto) when the teleport is refused (Bot.Actions.Teleport off, a refusal in combat). The death loop escape above only ran at the
// moment of a revive and only by teleport: prod 2026-11-14 bot 641, level 5, died 590 times in 18 hours at one spot of a level 50 zone.
void BotPolicy::DeathAreaEscape(DecideTick& tick, BotTurn& turn)
{
    std::uint64_t const nowMs = tick.nowMs;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    if (Opt().DeathAreaCount == 0 || nowMs >= state.AvoidAreaUntilMs || state.AvoidAreaMap != bot.map_id || bot.life_state != 0 || bot.in_combat != 0 ||
        turn.partyLeader != 0 || turn.routeOn || turn.stagingHold || nowMs < state.AreaEscapeRetryMs)
        return;
    if (std::hypot(bot.pos_x - state.AvoidAreaX, bot.pos_y - state.AvoidAreaY) > float(Opt().DeathAreaYards))
        return;
    state.AreaEscapeRetryMs = nowMs + kDeathAreaEscapeRetryMs;
    if (!Opt().Grind || Opt().Grind->Spots.empty() || bot.level == 0)
    {
        LogEvent("death_area_escape: bot=" + std::to_string(bot.guid) + " level=" + std::to_string(bot.level) + " map=" + std::to_string(bot.map_id) + " result=no_grind_data");
        return;
    }
    for (std::size_t const index : GrindSpotsForBot(state, bot, nowMs))
    {
        GrindSpot const& spot = Opt().Grind->Spots[index];
        if (std::hypot(spot.X - state.AvoidAreaX, spot.Y - state.AvoidAreaY) < kDeathLoopAwayYards)
            continue;
        BotAction away;
        away.bot_guid = bot.guid;
        away.type = ActionType::kTeleport;
        away.spell_id = bot.map_id;
        away.dest_x = spot.X;
        away.dest_y = spot.Y;
        away.dest_z = spot.Z;
        tick.actions.push_back(away);
        // The walk there, in case the teleport is refused; a teleport that lands ends it at once (the goto is done within 3 yards).
        state.GotoActive = true;
        state.GotoPoint = Point{ spot.X, spot.Y, spot.Z };
        state.GotoUntilMs = nowMs + kDeathAreaWalkMs;
        state.TrainAfterGoto = false;
        state.Wandering = false;
        state.EscapeUntilMs = nowMs + kDeathLoopCooldownMs;
        state.GrindVisited.push_back(index);
        if (state.GrindVisited.size() > kGrindMemory)
            state.GrindVisited.erase(state.GrindVisited.begin());
        LogEvent("death_area_escape: bot=" + std::to_string(bot.guid) + " level=" + std::to_string(bot.level) + " map=" + std::to_string(bot.map_id) + " from=" +
                 std::to_string(int(bot.pos_x)) + "," + std::to_string(int(bot.pos_y)) + " spot=" + std::to_string(index));
        return;
    }
    LogEvent("death_area_escape: bot=" + std::to_string(bot.guid) + " level=" + std::to_string(bot.level) + " map=" + std::to_string(bot.map_id) + " result=no_spot_away");
}
