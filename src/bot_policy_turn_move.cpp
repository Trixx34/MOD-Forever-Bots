// SPDX-License-Identifier: GPL-2.0-or-later
// BotPolicy::Decide, a bot's turn: where it moves (flee, kite, fight, follow, the dungeon route, quests, goto) and whether it is idle (the steps DecideBot runs in order; bot_policy.cpp).
#include "bot_policy_internal.h"

void BotPolicy::TurnMoveChoice(DecideTick& tick, BotTurn& turn)
{
    std::uint64_t nowMs = tick.nowMs;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto& nearby = turn.nearby;
    auto const& lowHealth = turn.lowHealth;
    auto const& partyLeader = turn.partyLeader;
    auto const& partyMember = turn.partyMember;
    auto& followRef = turn.followRef;
    auto& targetUnit = turn.targetUnit;
    auto& targetRef = turn.targetRef;
    auto const& distance = turn.distance;
    auto& wanted = turn.wanted;
    auto& moveWhy = turn.moveWhy;
    auto& kiteRetry = turn.kiteRetry;
    auto& aoeWarder = turn.aoeWarder;
    auto& healLosPoint = turn.healLosPoint;
    auto& kiteEntry = turn.kiteEntry;

    // A member of a party does not run from the fight on low health: its healer can only heal a tank that holds still, and the pack follows a fleeing bot onto the party.
    if (lowHealth && partyLeader == 0)
    {
        // Below kDisengagePct health: get clear of whatever is close enough to still kill it. In combat that is the threat
        // target; out of combat the closest live hostile is the one to run from, since nothing says yet that it is the target.
        // Standing still to rest used to be the whole reply, and a hostile five yards away killed the bot.
        NearbyUnit const* threat = targetUnit;
        if (!threat)
            threat = Closest(bot, nearby, IsLiveMob);
        float const away = threat ? (threat == targetUnit ? distance : rotation::DistanceYards(bot, *threat))
                                  : std::numeric_limits<float>::max();
        if (threat && away < kFleeYards)
        {
            float const dx = bot.pos_x - threat->pos_x;
            float const dy = bot.pos_y - threat->pos_y;
            float const dist = std::sqrt(dx * dx + dy * dy);
            if (dist > 0.0f)
            {
                Point flee{ threat->pos_x + dx / dist * kFleeYards,
                            threat->pos_y + dy / dist * kFleeYards,
                            bot.pos_z };
                wanted = Decision{ ActionType::kMoveTo, 0, flee };
                moveWhy = "flee";
            }
            else
                wanted = Decision{ ActionType::kStop, 0 };
        }
        else
        {
            wanted = followRef ? Decision{ ActionType::kFollow, followRef } : Decision{ ActionType::kStop, 0 };
        }
    }
    else if (partyMember && aoeWarder != nullptr &&
             std::hypot(bot.pos_x - aoeWarder->pos_x, bot.pos_y - aoeWarder->pos_y) < kAoeStandYards - 0.5f)
    {
        // An Onyxian Warder is near (kAoeStandYards, in combat or not): every party member other than the tank (the leader) gets out of its Fire Nova, whatever it is fighting or healing (the 2026-10-29 wipe:
        // only a bot whose own target was the Warder kited). Each bot has its own point round the circle; one the core has no route to is given up for the next one
        // round, and with none left the bot stands and works from where it is.
        // A kite point that a hazard covers (a ground effect the bot would walk into) is given up for the next one too.
        wanted = Decision{ ActionType::kStop, 0 };
        std::vector<ground_avoid::Circle> const hazards = HazardCircles(_options, bot);
        for (int retry = 0; retry < 6; ++retry)
        {
            Point const kite = AoeKitePoint(bot, *aoeWarder, retry, kiteEntry);
            bool blocked = nowMs < state.NoRouteUntilMs && std::hypot(kite.X - state.NoRouteDest.X, kite.Y - state.NoRouteDest.Y) < kNoRouteSameYards;
            for (auto const& place : state.RefusedPlaces)
                if (nowMs < place.second && std::hypot(kite.X - place.first.X, kite.Y - place.first.Y) < kNoRouteSameYards)
                    blocked = true;
            if (!blocked && !ground_avoid::InsideAny(kite.X, kite.Y, hazards))
            {
                wanted = Decision{ ActionType::kMoveTo, 0, kite };
                moveWhy = "kite";
                kiteRetry = retry;
                break;
            }
        }
    }
    else if (partyMember && bot.in_combat != 0 && nowMs < state.HealLosUntilMs && state.HealLosTarget != 0 && aoeWarder != nullptr && healLosPoint)
    {
        // A healer whose heal was refused for line of sight steps to where it may see the member, but never into the Fire Nova.
        wanted = Decision{ ActionType::kMoveTo, 0, *healLosPoint };
        moveWhy = "heal_los";
    }
    else if (targetRef != 0)
        MoveToTarget(tick, turn);
    else
        MoveWithoutTarget(tick, turn);
}

// TurnMoveChoice with a target: the fight starts, and the bot closes in, stands in its band, or kites.
void BotPolicy::MoveToTarget(DecideTick& tick, BotTurn& turn)
{
    std::uint64_t nowMs = tick.nowMs;
    std::vector<BotAction>& actions = tick.actions;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto const& classId = turn.classId;
    auto const& partyLeader = turn.partyLeader;
    auto const& partyMember = turn.partyMember;
    auto& targetUnit = turn.targetUnit;
    auto& targetRef = turn.targetRef;
    auto const& distance = turn.distance;
    auto& wanted = turn.wanted;
    auto& moveWhy = turn.moveWhy;
    auto& kiteRetry = turn.kiteRetry;
    auto& kiteEntry = turn.kiteEntry;
    if (!state.InFight)
    {
        state.InFight = true;
        state.FightStartMs = nowMs;
        // The review's instrumentation (2026-10-22): the party's state at the start of a fight, so a run says
        // whether the pulls happen with the party healthy and with mana, and where they are.
        if (_options.OnEvent)
            _options.OnEvent("pull: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) +
                  " pos=" + std::to_string(int(bot.pos_x)) + "," + std::to_string(int(bot.pos_y)) +
                  " hp=" + std::to_string(bot.health) + "/" + std::to_string(bot.max_health) +
                  " power=" + std::to_string(bot.power) + "/" + std::to_string(bot.max_power) +
                  " target=" + std::to_string(forever_module::UnitRefEntry(targetRef)));
        // Layout 13: the pet is in the snapshot, so it can be sent at this fight's target. Once per fight, so a
        // long one does not repeat the order. A core that reports no pet never gets this action.
        if (PetOf(bot) == PetKnown::kOut)
        {
            BotAction petGo;
            petGo.bot_guid = bot.guid;
            petGo.type = ActionType::kPetCommand;
            petGo.spell_id = kPetAttack;
            petGo.target_guid = targetRef;
            actions.push_back(petGo);
            state.PetCommanded = true;
        }
    }
    // A party healer supports from range: it does not close to melee, and it does not auto-attack (below). A heal has a long range,
    // so it stays where the party's casters stand even when its mana is spent: walking into the pack only puts it in the boss's
    // cleave and gives it the tank's aggro (the 2026-10-23 Deadmines run: healers at 0 power meleed for 1-4k and took 2-9k).
    bool const healerSupport = partyLeader != 0 && state.Role_ != BotState::Role::Dps &&
                               HealSpellFor(classId, bot.level) != 0;
    // A ranged bot that has run out of power closes to melee range: its spells are spent.
    bool const outOfPower = bot.max_power > 0 && std::uint64_t(bot.power) * 100 < std::uint64_t(bot.max_power) * kOutOfPowerPct;
    // A hunter with no ammunition cannot shoot (the core refuses every ranged attack): it closes to melee range as well.
    float approach = (outOfPower || NoAmmo(bot, classId)) && !healerSupport ? kMeleeApproachYards : rotation::ApproachYards(classId);
    if (nowMs < state.CloserUntilMs)
        approach = std::min(approach, kMeleeApproachYards + 1.0f);
    if (nowMs < state.RangeCloserUntilMs)
        approach = std::min(approach, approach > kMeleeApproachYards + 0.5f ? kCasterCloserYards : kMeleeCloserYards);
    // Around an area-attack target a party member other than the tank never closes in: it holds at kite distance.
    bool const aoeTarget = partyMember && targetUnit && forever_module::UnitRefEntry(targetUnit->guid) == kOnyxianWarderEntry;
    if (aoeTarget)
        approach = std::max(approach, kAoeStandYards);
    // Onyxia herself: the bot's role band (healer, ranged or melee) sets how close it comes. It closes to the band's far edge, and a healer or ranged bot
    // that is nearer than the band's near edge kites back to its middle. A melee bot's band is its close range (2-9), so it only closes.
    bool const bossTarget = partyMember && targetUnit && forever_module::UnitRefEntry(targetUnit->guid) == kOnyxiaBossEntry;
    StandBand const band = healerSupport ? kHealerBand : rotation::ApproachYards(classId) <= kMeleeApproachYards + 0.5f ? kMeleeBand : kRangedBand;
    bool const bossBandKite = bossTarget && band.Min > kMeleeBand.Max;
    if (bossTarget)
        approach = std::min(approach, band.Max);
    if (distance <= approach)
    {
        // A ranged bot that has walked into casting range stops there: the core's follow keeps running it into the target,
        // and a spell with a cast time fails while the caster moves.
        if (state.Approaching && approach > kMeleeApproachYards + 0.5f)
        {
            BotAction stop;
            stop.bot_guid = bot.guid;
            stop.type = ActionType::kStop;
            actions.push_back(stop);
        }
        state.Approaching = false;
        // Kite: a ranged bot backs off when a melee target gets inside kiting distance, then resumes attacking
        // once it is back at range.
        // Not in a party: a party's tank holds the target, and a caster that keeps backing off never gets a spell off (300-450 move orders a minute per caster in the
        // Deadmines runs, and each step re-pulled the next pack).
        // The exception is a target with a big area attack around it (the Onyxian Warder): only the tank, the party leader, stands in it,
        // and every other party member, melee class or not, backs off past its radius and casts from there.
        float const kiteYards = aoeTarget ? kAoeStandYards : kRangedKiteYards;
        if (bossBandKite && distance < band.Min)
        {
            // Back to the middle of the band, on a point no hazard covers; with none the bot holds where it is.
            std::optional<Point> const stand = BandStandPoint(bot, *targetUnit, band.Mid(), HazardCircles(_options, bot));
            if (stand)
            {
                wanted = Decision{ ActionType::kMoveTo, 0, *stand };
                moveWhy = "band_kite";
            }
            else
                wanted = Decision{ ActionType::kStop, 0 };
        }
        else if (((partyLeader == 0 && approach > kMeleeApproachYards + 0.5f) || aoeTarget) && targetUnit && distance < kiteYards)
        {
            float const dx = bot.pos_x - targetUnit->pos_x;
            float const dy = bot.pos_y - targetUnit->pos_y;
            float const dist = std::sqrt(dx * dx + dy * dy);
            if (dist > 0.0f)
            {
                Point kite{ targetUnit->pos_x + dx / dist * kiteYards,
                            targetUnit->pos_y + dy / dist * kiteYards,
                            bot.pos_z };
                bool hold = false;
                if (aoeTarget)
                {
                    // The bot's own bearing, as in the branch above; a point a hazard covers is passed over for the next retry, and with none clear the bot holds.
                    std::vector<ground_avoid::Circle> const hazards = HazardCircles(_options, bot);
                    bool found = false;
                    for (int retry = 0; retry < 6 && !found; ++retry)
                    {
                        kite = AoeKitePoint(bot, *targetUnit, retry, kiteEntry);
                        found = !ground_avoid::InsideAny(kite.X, kite.Y, hazards);
                        kiteRetry = retry;
                    }
                    hold = !found;
                }
                if (hold)
                    wanted = Decision{ ActionType::kStop, 0 };
                else
                {
                    wanted = Decision{ ActionType::kMoveTo, 0, kite };
                    moveWhy = "kite";
                }
            }
            else
                wanted = Decision{ ActionType::kAttackTarget, targetRef };
        }
        else if (healerSupport || aoeTarget)
        {
            // A healer does not auto-attack in a party: the core's attack order runs it into melee range, where it takes the
            // same hits as the tank and pulls aggro with its swings. It holds the ranged stand-off and works with spells.
            wanted = Decision{ ActionType::kStop, 0 };
        }
        else
            wanted = Decision{ ActionType::kAttackTarget, targetRef };
    }
    else
    {
        // Walking to a target that never gets in range (behind a wall, or unreachable) is given up after a while:
        // the target is avoided, and the bot picks another one next tick.
        if (partyLeader != 0 && bot.in_combat != 0 && distance > kPartyChaseYards)
        {
            state.Approaching = false;
            AvoidUnit(state.AvoidUntilMs, state.AvoidCount, targetRef, nowMs);
            wanted = Decision{ ActionType::kStop, 0 };
            if (_options.OnEvent && nowMs - state.MoveWhyLoggedMs >= 5000)
            {
                state.MoveWhyLoggedMs = nowMs;
                _options.OnEvent("chase_dropped: bot=" + std::to_string(bot.guid) + " distance=" + std::to_string(int(distance)) + " at=" + std::to_string(int(bot.pos_x)) + "," +
                                 std::to_string(int(bot.pos_y)) + "," + std::to_string(int(bot.pos_z)));
            }
        }
        else if (!state.Approaching || state.ApproachTarget != targetRef)
        {
            state.Approaching = true;
            state.ApproachTarget = targetRef;
            state.ApproachSinceMs = nowMs;
            wanted = Decision{ ActionType::kFollow, targetRef };
            moveWhy = "approach_target";
        }
        else if (nowMs - state.ApproachSinceMs > kApproachTimeoutMs)
        {
            state.Approaching = false;
            AvoidUnit(state.AvoidUntilMs, state.AvoidCount, targetRef, nowMs);
            wanted = Decision{ ActionType::kStop, 0 };
        }
        else
        {
            wanted = Decision{ ActionType::kFollow, targetRef };
        }
    }
}

// TurnMoveChoice with no target: the fight is over; the bot follows, is put back by its leader, walks the dungeon route, a quest step or a goto order.
void BotPolicy::MoveWithoutTarget(DecideTick& tick, BotTurn& turn)
{
    WorldTickSnapshotView const& view = tick.view;
    std::uint64_t nowMs = tick.nowMs;
    std::vector<BotAction>& actions = tick.actions;
    auto const& indexOf = tick.indexOf;
    auto& membersOf = tick.membersOf;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto const& partyLeaderBot = turn.partyLeaderBot;
    auto const& partyMember = turn.partyMember;
    auto const& routeOn = turn.routeOn;
    PolicyOptions::ScenarioRoute const& scenario = *turn.scenario;
    auto& leaderSnap = turn.leaderSnap;
    auto& followRef = turn.followRef;
    auto& wanted = turn.wanted;
    // Layout 13: the fight just ended, so the pet has nothing to hunt. Call it back once; a core that reports no pet
    // never sent it out, so there is nothing to recall.
    if (state.InFight && state.PetCommanded)
    {
        BotAction back;
        back.bot_guid = bot.guid;
        back.type = ActionType::kPetCommand;
        back.spell_id = kPetFollow; // not Stay: a pet left standing where the fight ended is left behind when the party walks on
        actions.push_back(back);
        state.PetCommanded = false;
    }
    state.InFight = false;
    wanted = followRef ? Decision{ ActionType::kFollow, followRef } : Decision{ ActionType::kStop, 0 };
    // A member that has lost its leader in a dungeon (far away, or on another level of the instance, so that it has no way to follow) is put next to it after a while
    // (DungeonTeleport; the core's Bot.Actions.Teleport). Not in a fight, and not again for 30 seconds.
    // A leader just revived at the instance entrance is not followed there: it comes back to the party (leader_recover, in the route below).
    bool leaderAtEntry = false;
    if (leaderSnap)
        if (auto const dungeon = _options.DungeonsByMap.find(bot.map_id); dungeon != _options.DungeonsByMap.end() && dungeon->second.HasEntry)
            leaderAtEntry = DistanceTo(*leaderSnap, dungeon->second.Entry) <= kEntryYards && DistanceTo(bot, dungeon->second.Entry) > kLostLeaderYards;
    if (_options.DungeonTeleport && routeOn && partyMember && leaderSnap && bot.life_state == 0 && bot.in_combat == 0 && leaderSnap->life_state == 0 && !leaderAtEntry)
    {
        bool const lost = Distance2d(bot, *leaderSnap) > kLostLeaderYards || std::fabs(bot.pos_z - leaderSnap->pos_z) > kLostLeaderHeight;
        if (!lost)
            state.LostSinceMs = 0;
        else if (state.LostSinceMs == 0)
            state.LostSinceMs = nowMs;
        else if (nowMs - state.LostSinceMs >= kLostLeaderMs && nowMs >= state.NextRecoverMs)
        {
            if (_options.OnEvent)
                _options.OnEvent("member_recover: bot=" + std::to_string(bot.guid) + " leader=" + std::to_string(leaderSnap->guid) + " from=" + std::to_string(int(bot.pos_x)) + "," +
                                 std::to_string(int(bot.pos_y)) + "," + std::to_string(int(bot.pos_z)) + " leader_at=" + std::to_string(int(leaderSnap->pos_x)) + "," +
                                 std::to_string(int(leaderSnap->pos_y)) + "," + std::to_string(int(leaderSnap->pos_z)));
            wanted = Decision{ ActionType::kTeleport, 0, Point{ leaderSnap->pos_x - 3.0f, leaderSnap->pos_y, leaderSnap->pos_z }, bot.map_id };
            state.NextRecoverMs = nowMs + 30000;
            state.LostSinceMs = 0;
            state.Last.reset();
        }
    }

    // Dungeon route (doc 19): the leader of a party walks the route while it is healthy and its members are close. Otherwise
    // it stands (and so rests), and the party catches up. The route is the scenario's, or for "tank lead" the one of the dungeon of the bot's map.
    std::vector<Point> const* routePoints = nullptr;
    std::map<std::size_t, std::uint32_t> const* routeBoss = nullptr;
    std::vector<dungeon_events::Event> const* routeEvents = nullptr;
    if (partyLeaderBot && !state.ChatStay)
    {
        if (IsRouteActive(bot.guid) && (scenario.RouteMap == 0 || bot.map_id == scenario.RouteMap))
        {
            routePoints = &scenario.Route;
            routeBoss = &scenario.RouteBossAt;
            routeEvents = &scenario.Events;
        }
        else if (state.TankLead)
        {
            if (auto const dungeon = _options.DungeonsByMap.find(bot.map_id); dungeon != _options.DungeonsByMap.end())
            {
                routePoints = &dungeon->second.Route;
                routeBoss = &dungeon->second.BossAt;
                routeEvents = &dungeon->second.Events;
            }
        }
    }
    std::set<std::uint32_t> const& killedNow = IsRouteActive(bot.guid) ? KilledBossesOf(bot.guid) : _tankKilled[bot.map_id];
    if (routePoints && !IsRouteActive(bot.guid) && routeBoss && !routeBoss->empty())
    {
        // The bosses of a "tank lead" run: one seen dead by the leader or any member counts (the scenario director does this for a scenario).
        std::set<std::uint32_t>& killedHere = _tankKilled[bot.map_id];
        auto const scan = [&](std::uint64_t guid)
        {
            auto const at = indexOf.find(guid);
            if (at == indexOf.end())
                return;
            for (NearbyUnit const& unit : view.NearbyOf(at->second))
                if (unit.health == 0 && forever_module::UnitRefType(unit.guid) == 8)
                    for (auto const& [point, entry] : *routeBoss)
                        if (entry == forever_module::UnitRefEntry(unit.guid))
                            killedHere.insert(entry);
        };
        scan(bot.guid);
        for (std::uint64_t member : membersOf[bot.guid])
            scan(member);
    }
    if (routePoints && !routePoints->empty() && state.RouteIndex < routePoints->size())
    {
        RouteLeg route{ routePoints, routeBoss, routeEvents, &killedNow };
        RouteReadiness(tick, turn, route);
        RouteLeaderRecover(tick, turn, route);
        RouteEvents(tick, turn, route);
        RouteWalkOn(tick, turn, route);
    }
    MoveForQuestOrGoto(tick, turn);
}

// The route leader's checks before it walks on: the party together, its own health, the party's mana; and a waypoint the core keeps finding no route to is
// recovered (the leader put back on the route), skipped, or reported as blocking at a boss.
void BotPolicy::RouteReadiness(DecideTick& tick, BotTurn& turn, RouteLeg& route)
{
    WorldTickSnapshotView const& view = tick.view;
    std::uint64_t nowMs = tick.nowMs;
    auto const& indexOf = tick.indexOf;
    auto& membersOf = tick.membersOf;
    std::size_t const i = turn.i;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto& nearby = turn.nearby;
    auto const& deadWaitKind = turn.deadWaitKind;
    auto& routeBusy = turn.routeBusy;
    auto& wanted = turn.wanted;
    std::vector<Point> const* const routePoints = route.routePoints;
    std::map<std::size_t, std::uint32_t> const* const routeBoss = route.routeBoss;
    std::set<std::uint32_t> const& killedNow = *route.killedNow;
    bool& together = route.together;
    together = true;
    for (std::uint64_t member : membersOf[bot.guid])
    {
        auto const at = indexOf.find(member);
        if (at == indexOf.end() || view.bots[at->second].life_state != 0)
            continue;
        together = together && (state.RegroupStuck.count(member) != 0 || Distance2d(bot, view.bots[at->second]) <= kRouteWaitYards);
    }
    if (state.TankLead && !IsRouteActive(bot.guid))
    {
        // The group's player: the tank does not run off without them. A player out of its view counts as far.
        bool playerNear = false;
        for (NearbyUnit const& unit : nearby)
            if (unit.guid == bot.group_leader_guid)
                playerNear = rotation::DistanceYards(bot, unit) <= kRouteWaitYards;
        together = together && (playerNear || state.LevelRun);   // a level-dungeon party has no human to wait for
    }
    together = together && deadWaitKind == 0;
    route.strong = bot.max_health == 0 ||
                        std::uint64_t(bot.health) * 100 >= std::uint64_t(bot.max_health) * kRouteMinHealthPct;
    bool const& strong = route.strong;
    // Mana: a party whose healer (or any caster) is dry walks into the next pack and loses it. The members sit and drink while the leader waits, at most kRouteManaMaxMs.
    bool manaLowInParty = false;
    {
        auto const dry = [](BotSnapshot const& member)
        {
            return member.life_state == 0 && member.power_type == 0 && member.max_power > 0 &&
                   std::uint64_t(member.power) * 100 < std::uint64_t(member.max_power) * kRouteMinManaPct;
        };
        std::uint64_t manaSum = 0;
        auto const add = [&manaSum](BotSnapshot const& member)
        {
            if (member.life_state == 0 && member.power_type == 0 && member.max_power > 0)
                manaSum += member.power;
        };
        manaLowInParty = dry(bot);
        add(bot);
        for (std::uint64_t member : membersOf[bot.guid])
            if (auto const at = indexOf.find(member); at != indexOf.end())
            {
                manaLowInParty = manaLowInParty || dry(view.bots[at->second]);
                add(view.bots[at->second]);
            }
        if (!manaLowInParty || bot.in_combat != 0)
        {
            state.RouteManaSinceMs = 0;
            state.RouteManaProgressMs = 0;
        }
        else if (state.RouteManaSinceMs == 0)
        {
            state.RouteManaSinceMs = nowMs;
            state.RouteManaProgressMs = nowMs;
            state.RouteManaLastSum = manaSum;
        }
        else
        {
            // 0.44.14: the 60 s cap counts from the last tick the party's mana rose (the members are drinking), so a priest drinking from 3% is not
            // dragged into the next pack after 60 s at 40%; a party where nobody gains mana (no drink, cannot sit) still goes on after 60 s.
            if (manaSum > state.RouteManaLastSum)
                state.RouteManaProgressMs = nowMs;
            state.RouteManaLastSum = manaSum;
        }
    }
    route.manaReady = !manaLowInParty || nowMs - state.RouteManaProgressMs >= kRouteManaMaxMs || nowMs - state.RouteManaSinceMs >= kRouteManaAbsMaxMs;
    // PartyMana: a member that said it is low on mana holds the walk too (never in combat, and never longer than PartyManaHoldSec).
    if (_options.PartyMana.Enabled && route.manaReady)
        if (auto const mana = _partyMana.find(bot.guid); mana != _partyMana.end() && mana->second.Holding(_options.PartyMana, bot.in_combat != 0, nowMs))
            route.manaReady = false;
    bool const& manaReady = route.manaReady;
    // A waypoint the core finds no route to (a spawn on a ledge or off the navmesh, a generated point inside a wall) would hold the whole run for
    // good: after kRouteNoRouteSkip refusals in a row the leader gives that point up and goes on to the next one.
    // A boss point is never skipped: the boss stands between the party and the rest of the dungeon (a locked door opens when it dies, or it guards
    // the way). A boss that cannot be reached stops the run at that point, which the result reports as the furthest point reached.
    bool& recovering = route.recovering;
    recovering = false;   // the leader was just put back on the route: no walking order this tick
    bool const bossHere = (*routeBoss).count(state.RouteIndex) != 0 && !killedNow.count((*routeBoss).at(state.RouteIndex));
    if (state.RouteNoRoute >= kRouteNoRouteSkip && bossHere)
    {
        if (state.RouteNoRoute == kRouteNoRouteSkip && _options.OnEvent)
            _options.OnEvent("route_blocked: bot=" + std::to_string(bot.guid) + " point=" + std::to_string(state.RouteIndex) + "/" + std::to_string(routePoints->size()) +
                             " boss=" + std::to_string((*routeBoss).at(state.RouteIndex)) + " reason=no_route map=" + std::to_string(bot.map_id) +
                             " from=" + std::to_string(int(bot.pos_x)) + "," + std::to_string(int(bot.pos_y)) + "," + std::to_string(int(bot.pos_z)));
        state.RouteNoRoute = kRouteNoRouteSkip + 1; // logged once
    }
    else if (state.RouteNoRoute >= kRouteNoRouteSkip && _options.DungeonTeleport && state.RouteRecoveredAt != state.RouteIndex)
    {
        // Off the mesh, on another level, or cut off: put the leader back on the route (the point before, or the first point) once; the same point failing again
        // after that is skipped (or blocks, for a boss).
        std::size_t back = state.RouteIndex > 0 ? state.RouteIndex - 1 : 0;
        bool const far = DistanceTo(bot, (*routePoints)[state.RouteIndex]) > kRouteFarYards;
        if (far)
        {
            // Too far for the core's pathfinder (a revived leader at the entrance): the point before is no nearer. Back to the point nearest the leader (at or before this one).
            float best = DistanceTo(bot, (*routePoints)[0]);
            back = 0;
            for (std::size_t i = 1; i <= state.RouteIndex; ++i)
                if (float const d = DistanceTo(bot, (*routePoints)[i]); d < best)
                {
                    best = d;
                    back = i;
                }
            if (_options.OnEvent)
                _options.OnEvent("route_rewind: bot=" + std::to_string(bot.guid) + " from=" + std::to_string(state.RouteIndex) + " to=" + std::to_string(back));
            state.RouteIndex = back;
        }
        state.RouteRecoveredAt = far ? std::size_t(-1) : state.RouteIndex;   // a far leader is rewound again if it needs it
        state.RouteNoRoute = 0;
        if (_options.OnEvent)
            _options.OnEvent("route_recover: bot=" + std::to_string(bot.guid) + " point=" + std::to_string(state.RouteIndex) + "/" + std::to_string(routePoints->size()) +
                             " from=" + std::to_string(int(bot.pos_x)) + "," + std::to_string(int(bot.pos_y)) + "," + std::to_string(int(bot.pos_z)) + " to_point=" +
                             std::to_string(back));
        wanted = Decision{ ActionType::kTeleport, 0, (*routePoints)[back], bot.map_id };
        routeBusy = true;
        recovering = true;
        state.Last.reset();
    }
    else if (state.RouteNoRoute >= kRouteNoRouteSkip)
    {
        if (_options.OnEvent)
        {
            Point const& given = (*routePoints)[state.RouteIndex];
            _options.OnEvent("route_skip: bot=" + std::to_string(bot.guid) + " point=" + std::to_string(state.RouteIndex) + "/" + std::to_string(routePoints->size()) +
                             " reason=no_route map=" + std::to_string(bot.map_id) + " from=" + std::to_string(int(bot.pos_x)) + "," + std::to_string(int(bot.pos_y)) + "," +
                             std::to_string(int(bot.pos_z)) + " to=" + std::to_string(int(given.X)) + "," + std::to_string(int(given.Y)) + "," + std::to_string(int(given.Z)) +
                             " mounted=" + std::to_string(state.Mounted));
        }
        ++state.RouteIndex;
        state.RouteNoRoute = 0;
    }
}

void BotPolicy::RouteLeaderRecover(DecideTick& tick, BotTurn& turn, RouteLeg& route)
{
    WorldTickSnapshotView const& view = tick.view;
    std::uint64_t nowMs = tick.nowMs;
    auto const& indexOf = tick.indexOf;
    auto& membersOf = tick.membersOf;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto& routeBusy = turn.routeBusy;
    auto& wanted = turn.wanted;
    bool const& strong = route.strong;
    bool& recovering = route.recovering;
    // The leader revived at the instance entrance (a ghost of a dungeon party is put there) while a living member stands further in: the leader is put
    // next to that member, and the party goes on from where it was. Before, the members were put next to the leader at the entrance and the party walked
    // the cleared way again (60 of the 141 member teleports of the 2026-10-26 Deadmines run).
    // Stockade, 2026-10-28 (0.39.0 sim, 220 deaths in 6 h): the leader comes back at about half health (a release brings it back at 49 %, and it then arrives next to the party
    // with 30 %): 31 of the 96 tank deaths came less than 120 s after the same tank's previous death, 24 of 60 leader_recover teleports were followed by that tank's death
    // within 120 s, and 38 of 72 tank pull lines of a 50-minute journal window began below 50 % health (5 to 13 % in the other dungeons). The tank stays at the entrance, which is quiet, and sits
    // there until it is fit to walk on (kRouteMinHealthPct, the same as for a walk along the route), at most kLeaderRecoverMaxWaitMs: a tank that cannot get there (no food,
    // a slow regeneration) goes in all the same.
    bool atEntry = false;
    if (!recovering && _options.DungeonTeleport && bot.life_state == 0 && bot.in_combat == 0)
        if (auto const dungeon = _options.DungeonsByMap.find(bot.map_id);
            dungeon != _options.DungeonsByMap.end() && dungeon->second.HasEntry && DistanceTo(bot, dungeon->second.Entry) <= kEntryYards)
            atEntry = true;
    if (!atEntry)
        state.LeaderRecoverWaitSinceMs = 0;
    // 0.44.8: the Deadmines run of 2026-11-05 (06:53-07:55) had 103 stretches of a leader at the entrance (median 60 s, 120 min over 18 parties), and in 28 % of the lines
    // every member further in was in a fight, which kept the leader out (a member in a fight was never a target). After kLeaderRecoverCombatMs at the entrance a member in a fight is a target too.
    if (!atEntry)
        state.AtEntrySinceMs = 0;
    else if (state.AtEntrySinceMs == 0)
        state.AtEntrySinceMs = nowMs;
    bool const fightingMemberOk = atEntry && state.AtEntrySinceMs != 0 && nowMs - state.AtEntrySinceMs >= kLeaderRecoverCombatMs;
    if (!recovering && atEntry && nowMs >= state.NextRecoverMs)
        if (auto const dungeon = _options.DungeonsByMap.find(bot.map_id); dungeon != _options.DungeonsByMap.end())
            for (std::uint64_t member : membersOf[bot.guid])
            {
                auto const at = indexOf.find(member);
                if (at == indexOf.end())
                    continue;
                BotSnapshot const& other = view.bots[at->second];
                if (other.life_state != 0 || (other.in_combat != 0 && !fightingMemberOk) || other.map_id != bot.map_id || DistanceTo(other, dungeon->second.Entry) <= kLostLeaderYards)
                    continue;
                if (!strong)
                {
                    if (state.LeaderRecoverWaitSinceMs == 0)
                    {
                        state.LeaderRecoverWaitSinceMs = nowMs;
                        if (_options.OnEvent)
                            _options.OnEvent("leader_recover_wait: bot=" + std::to_string(bot.guid) + " member=" + std::to_string(other.guid) + " hp_pct=" +
                                             std::to_string(bot.max_health ? std::uint64_t(bot.health) * 100 / bot.max_health : 100));
                    }
                    if (nowMs - state.LeaderRecoverWaitSinceMs < kLeaderRecoverMaxWaitMs)
                        break; // rests at the entrance first
                }
                state.LeaderRecoverWaitSinceMs = 0;
                if (_options.OnEvent)
                    _options.OnEvent("leader_recover: bot=" + std::to_string(bot.guid) + " member=" + std::to_string(other.guid) + " from=" + std::to_string(int(bot.pos_x)) +
                                     "," + std::to_string(int(bot.pos_y)) + "," + std::to_string(int(bot.pos_z)) + " member_at=" + std::to_string(int(other.pos_x)) + "," +
                                     std::to_string(int(other.pos_y)) + "," + std::to_string(int(other.pos_z)) + " point=" + std::to_string(state.RouteIndex));
                wanted = Decision{ ActionType::kTeleport, 0, Point{ other.pos_x - 3.0f, other.pos_y, other.pos_z }, bot.map_id };
                routeBusy = true;
                recovering = true;
                state.NextRecoverMs = nowMs + 30000;
                state.RouteNoRoute = 0;
                state.RouteStallSinceMs = 0;
                state.Last.reset();
                break;
            }
}

void BotPolicy::RouteEvents(DecideTick& tick, BotTurn& turn, RouteLeg& route)
{
    std::uint64_t nowMs = tick.nowMs;
    std::vector<BotAction>& actions = tick.actions;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto& nearby = turn.nearby;
    auto& routeBusy = turn.routeBusy;
    auto const& distance = turn.distance;
    auto& wanted = turn.wanted;
    auto& moveWhy = turn.moveWhy;
    std::vector<dungeon_events::Event> const* const routeEvents = route.routeEvents;
    std::set<std::uint32_t> const& killedNow = *route.killedNow;
    // Events (dungeon_events.h): a door to open, a charge to fire. While one runs the leader does only what it asks.
    bool& eventBusy = route.eventBusy;
    eventBusy = false;
    if (routeEvents && !routeEvents->empty())
    {
        if (!state.Events || state.EventsMap != bot.map_id)
        {
            state.Events = std::make_shared<dungeon_events::Runner>(dungeon_events::ForMap(*routeEvents, bot.map_id));
            state.EventsMap = bot.map_id;
        }
        dungeon_events::Facts facts;
        facts.X = bot.pos_x;
        facts.Y = bot.pos_y;
        facts.Z = bot.pos_z;
        facts.InCombat = bot.in_combat != 0;
        facts.Alive = bot.life_state == 0;
        facts.KilledBosses = &killedNow;
        dungeon_events::Command const command = state.Events->Next(facts, nowMs);
        for (std::string const& line : state.Events->TakeLog())
            if (_options.OnEvent)
                _options.OnEvent(line + " bot=" + std::to_string(bot.guid));
        switch (command.Do)
        {
            case dungeon_events::Command::What::Nothing:
                break;
            case dungeon_events::Command::What::Hold:
                eventBusy = true;
                break;
            case dungeon_events::Command::What::MoveTo:
                wanted = Decision{ ActionType::kMoveTo, 0, Point{ command.X, command.Y, command.Z } };
                moveWhy = "event_move";
                eventBusy = true;
                break;
            case dungeon_events::Command::What::Use:
            {
                BotAction use;
                use.bot_guid = bot.guid;
                use.type = ActionType::kUseObject;
                use.spell_id = command.Object;
                use.target_guid = command.Item;
                use.dest_x = command.X;
                use.dest_y = command.Y;
                use.dest_z = command.Z;
                actions.push_back(use);
                wanted = Decision{ ActionType::kStop, 0 };
                eventBusy = true;
                break;
            }
            case dungeon_events::Command::What::Talk:
            {
                // Talk to the creature and pick a gossip option (kGossip, SDK 0.24): the Disciple of Naralex starts his escort this way.
                BotAction talk;
                talk.bot_guid = bot.guid;
                talk.type = ActionType::kGossip;
                talk.spell_id = command.Creature;
                talk.dest_x = float(command.Option);
                talk.dest_y = command.Y;
                talk.dest_z = command.Z;
                state.GossipX = command.X;
                state.GossipY = command.Y;
                actions.push_back(talk);
                wanted = Decision{ ActionType::kStop, 0 };
                eventBusy = true;
                break;
            }
            case dungeon_events::Command::What::Follow:
            {
                // Follow an escort NPC: the creature of that entry nearest in view. Out of view the leader stands, and the party with it.
                NearbyUnit const* escort = nullptr;
                float escortDistance = std::numeric_limits<float>::max();
                for (NearbyUnit const& unit : nearby)
                    if (forever_module::UnitRefEntry(unit.guid) == command.Creature && unit.health > 0)
                    {
                        float const distance = rotation::DistanceYards(bot, unit);
                        if (distance < escortDistance)
                        {
                            escort = &unit;
                            escortDistance = distance;
                        }
                    }
                wanted = escort ? Decision{ ActionType::kFollow, escort->guid } : Decision{ ActionType::kStop, 0 };
                moveWhy = "event_escort";
                eventBusy = true;
                break;
            }
            case dungeon_events::Command::What::Teleport:
                if (_options.DungeonTeleport)
                    wanted = Decision{ ActionType::kTeleport, 0, Point{ command.X, command.Y, command.Z }, bot.map_id };
                else
                    wanted = Decision{ ActionType::kStop, 0 };
                eventBusy = true;
                break;
        }
        if (eventBusy)
            routeBusy = true;
    }
}

// The leader walks on: points the fights drew it past are dropped, a boss point holds it until the boss is dead, and the walk (or why it holds) is logged.
void BotPolicy::RouteWalkOn(DecideTick& tick, BotTurn& turn, RouteLeg& route)
{
    WorldTickSnapshotView const& view = tick.view;
    std::uint64_t nowMs = tick.nowMs;
    auto const& indexOf = tick.indexOf;
    auto& membersOf = tick.membersOf;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto const& deadWaitKind = turn.deadWaitKind;
    auto& routeBusy = turn.routeBusy;
    auto& wanted = turn.wanted;
    auto& moveWhy = turn.moveWhy;
    std::vector<Point> const* const routePoints = route.routePoints;
    std::map<std::size_t, std::uint32_t> const* const routeBoss = route.routeBoss;
    std::set<std::uint32_t> const& killedNow = *route.killedNow;
    bool& together = route.together;
    bool const& strong = route.strong;
    bool const& manaReady = route.manaReady;
    bool& recovering = route.recovering;
    bool& eventBusy = route.eventBusy;
    while (!eventBusy && !recovering && state.RouteIndex + 1 < routePoints->size() && (*routeBoss).count(state.RouteIndex) == 0 &&
           DistanceTo(bot, (*routePoints)[state.RouteIndex]) > kRoutePointReachYards &&
           DistanceTo(bot, (*routePoints)[state.RouteIndex + 1]) <= kRouteSkipAheadYards)
    {
        if (_options.OnEvent)
            _options.OnEvent("route_passed: bot=" + std::to_string(bot.guid) + " point=" + std::to_string(state.RouteIndex) + "/" + std::to_string(routePoints->size()) +
                             " from=" + std::to_string(int(bot.pos_x)) + "," + std::to_string(int(bot.pos_y)) + "," + std::to_string(int(bot.pos_z)));
        ++state.RouteIndex;
        state.RouteNoRoute = 0;
    }
    Point const* waypoint = eventBusy || recovering || state.RouteIndex >= routePoints->size() ? nullptr : &(*routePoints)[state.RouteIndex];
    if (waypoint)
    {
        float const dx = bot.pos_x - waypoint->X;
        float const dy = bot.pos_y - waypoint->Y;
        if (std::sqrt(dx * dx + dy * dy) <= kRoutePointReachYards)
        {
            // At a boss point the leader stays until that boss is dead, so the party fights it where it stands (the pull logic takes what is near). A boss that
            // is not there to be fought, after kBossHoldMs, is given up and the route goes on.
            auto const boss = (*routeBoss).find(state.RouteIndex);
            bool hold = boss != (*routeBoss).end() && !killedNow.count(boss->second);
            if (hold)
            {
                if (state.RouteBossSinceMs == 0)
                    state.RouteBossSinceMs = nowMs;
                if (nowMs - state.RouteBossSinceMs >= kBossHoldMs)
                {
                    if (_options.OnEvent)
                        _options.OnEvent("boss_missed: bot=" + std::to_string(bot.guid) + " point=" + std::to_string(state.RouteIndex) + " boss=" + std::to_string(boss->second));
                    hold = false;
                }
            }
            if (!hold)
            {
                ++state.RouteIndex; // arrived: on to the next point in the same tick
                state.RouteNoRoute = 0;
                state.RouteBossSinceMs = 0;
                waypoint = state.RouteIndex < routePoints->size() ? &(*routePoints)[state.RouteIndex] : nullptr;
            }
            else
            {
                waypoint = nullptr; // stand here and let the party deal with the boss
                routeBusy = true;   // and do not wander off while idle
            }
        }
    }
    if (waypoint && together && strong && manaReady)
    {
        wanted = Decision{ ActionType::kMoveTo, 0, *waypoint };
        moveWhy = "*waypoint";
        routeBusy = true;
        // A walk that does not move (kRouteStallMs): sent again, and counted as a refusal of this point.
        if (state.RouteStallSinceMs == 0 || bot.in_combat != 0 ||
            std::hypot(bot.pos_x - state.RouteStallAt.X, bot.pos_y - state.RouteStallAt.Y) > kRouteStallYards)
        {
            state.RouteStallSinceMs = nowMs;
            state.RouteStallAt = Point{ bot.pos_x, bot.pos_y, bot.pos_z };
        }
        else if (nowMs - state.RouteStallSinceMs >= kRouteStallMs)
        {
            ++state.RouteNoRoute;
            state.RouteStallSinceMs = nowMs;
            state.Last.reset();
            if (_options.OnEvent)
                _options.OnEvent("route_stall: bot=" + std::to_string(bot.guid) + " point=" + std::to_string(state.RouteIndex) + "/" + std::to_string(routePoints->size()) +
                                 " pos=" + std::to_string(int(bot.pos_x)) + "," + std::to_string(int(bot.pos_y)) + "," + std::to_string(int(bot.pos_z)) + " to=" +
                                 std::to_string(int(waypoint->X)) + "," + std::to_string(int(waypoint->Y)) + "," + std::to_string(int(waypoint->Z)) +
                                 " refusals=" + std::to_string(state.RouteNoRoute));
        }
        // The leader's walk, at most every 5 s: which point it goes to, from where, and how many of its members are within the waiting distance.
        if (_options.OnEvent && nowMs - state.RouteWalkLoggedMs >= 5000)
        {
            state.RouteWalkLoggedMs = nowMs;
            std::uint32_t near = 0, all = 0;
            for (std::uint64_t member : membersOf[bot.guid])
                if (auto const at = indexOf.find(member); at != indexOf.end() && view.bots[at->second].life_state == 0)
                {
                    ++all;
                    near += Distance2d(bot, view.bots[at->second]) <= kRouteWaitYards;
                }
            _options.OnEvent("route_walk: bot=" + std::to_string(bot.guid) + " point=" + std::to_string(state.RouteIndex) + "/" + std::to_string(routePoints->size()) +
                             " to=" + std::to_string(int(waypoint->X)) + "," + std::to_string(int(waypoint->Y)) + "," + std::to_string(int(waypoint->Z)) + " from=" +
                             std::to_string(int(bot.pos_x)) + "," + std::to_string(int(bot.pos_y)) + "," + std::to_string(int(bot.pos_z)) + " members_near=" +
                             std::to_string(near) + "/" + std::to_string(all) + " combat=" + std::to_string(bot.in_combat));
        }
    }
    else
    {
        state.RouteStallSinceMs = 0;
        // Why the leader does not walk on, out of combat, at most every kRouteHoldLogMs: its health, a dead member, or the member farthest behind.
        if (waypoint && bot.in_combat == 0 && _options.OnEvent && nowMs - state.RouteHoldLoggedMs >= kRouteHoldLogMs)
        {
            state.RouteHoldLoggedMs = nowMs;
            std::uint64_t farthest = 0;
            float farthestYards = 0.0f;
            for (std::uint64_t member : membersOf[bot.guid])
                if (auto const at = indexOf.find(member); at != indexOf.end() && view.bots[at->second].life_state == 0)
                    if (float const yards = Distance2d(bot, view.bots[at->second]); yards > farthestYards)
                    {
                        farthestYards = yards;
                        farthest = member;
                    }
            char const* why = !strong ? "health" : !manaReady ? "mana" : deadWaitKind != 0 ? "dead_member" : farthestYards > kRouteWaitYards ? "member_far" : "player_far";
            _options.OnEvent(std::string("route_hold: bot=") + std::to_string(bot.guid) + " point=" + std::to_string(state.RouteIndex) + "/" +
                             std::to_string(routePoints->size()) + " why=" + why + " hp_pct=" +
                             std::to_string(bot.max_health ? std::uint64_t(bot.health) * 100 / bot.max_health : 100) + " farthest=" + std::to_string(farthest) +
                             " farthest_yards=" + std::to_string(int(farthestYards)) + " pos=" + std::to_string(int(bot.pos_x)) + "," + std::to_string(int(bot.pos_y)) +
                             "," + std::to_string(int(bot.pos_z)));
        }
    }
}

// The bot's quest step (walk, accept, hand in, hunt, ...) and a goto order.
void BotPolicy::MoveForQuestOrGoto(DecideTick& tick, BotTurn& turn)
{
    std::uint64_t nowMs = tick.nowMs;
    std::vector<BotAction>& actions = tick.actions;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto& questStep = turn.questStep;
    auto& routeBusy = turn.routeBusy;
    auto& wanted = turn.wanted;
    auto& moveWhy = turn.moveWhy;
    if (questStep.Kind != QuestStepKind::WalkTo)
    {
        state.WalkStillSinceMs = 0; // the stall timer only runs during a quest walk
        state.WalkAnchored = false;
        state.QuestTeleportSent = false;
    }

    switch (questStep.Kind)
    {
        case QuestStepKind::WalkTo:
        {
            // Stuck on the walk to a quest start's giver. One teleport if QuestTeleport is on; otherwise, or if that teleport was
            // already sent and the bot is still stuck, the start is dropped and the bot grinds. The planner only offers starts on
            // the bot's own map, so the giver's position is on that map too.
            Point const here{ bot.pos_x, bot.pos_y, bot.pos_z };
            if (!state.WalkAnchored || DistanceTo(bot, state.WalkAnchor) > kQuestWalkStuckYards)
            {
                state.WalkAnchor = here;
                state.WalkStillSinceMs = nowMs;
                state.WalkAnchored = true;
                state.QuestTeleportSent = false;
            }
            bool const stuck = nowMs - state.WalkStillSinceMs >= kQuestWalkStuckMs;
            if (stuck && _options.QuestTeleport && !state.QuestTeleportSent)
            {
                wanted = Decision{ ActionType::kTeleport, 0, Point{ questStep.Dest.X, questStep.Dest.Y, questStep.Dest.Z }, bot.map_id };
                state.QuestTeleportSent = true;
                state.WalkStillSinceMs = nowMs;
                if (_options.OnEvent)
                    _options.OnEvent("quest_walk_forced: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) +
                                     " quest=" + std::to_string(questStep.Quest) + " from=" + std::to_string(int(here.X)) + "," +
                                     std::to_string(int(here.Y)) + "," + std::to_string(int(here.Z)) + " to=" +
                                     std::to_string(int(questStep.Dest.X)) + "," + std::to_string(int(questStep.Dest.Y)) + "," +
                                     std::to_string(int(questStep.Dest.Z)));
            }
            else if (stuck)
            {
                if (questStep.StartGiver)
                    _quests.NoteNoRoute(state.Quest, nowMs);
                else
                    _quests.NoteWalkStuck(state.Quest, questStep.Quest, nowMs); // a hand-in or hunting walk with no route: left alone for a while, the bot grinds

                state.QuestTeleportSent = false;
                state.WalkStillSinceMs = 0;
                state.WalkAnchored = false;
                if (_options.OnEvent)
                    _options.OnEvent("quest_walk_blocked: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) +
                                     " quest=" + std::to_string(questStep.Quest) + " reason=" +
                                     (_options.QuestTeleport ? "teleport_stuck" : "teleport_off"));
                questStep = {};
                wanted = Decision{ ActionType::kStop, 0 };
            }
            else
            {
                wanted = Decision{ ActionType::kMoveTo, 0, Point{ questStep.Dest.X, questStep.Dest.Y, questStep.Dest.Z } };
                moveWhy = "quest_walk";
            }
            break;
        }
        case QuestStepKind::Hunt:
            // Hunt with the target area equal to the bot's own place (no areas known): stand and wait for the creatures.
            if (questStep.Dest.X != bot.pos_x || questStep.Dest.Y != bot.pos_y)
            {
                wanted = Decision{ ActionType::kMoveTo, 0, Point{ questStep.Dest.X, questStep.Dest.Y, questStep.Dest.Z } };
                moveWhy = "quest_hunt";
            }
            else
                wanted = Decision{ ActionType::kStop, 0 };
            break;
        case QuestStepKind::Accept:
        case QuestStepKind::HandIn:
        case QuestStepKind::Abandon:
        case QuestStepKind::Reward:
        {
            // Reading delay (ReadDelay, off by default): the bot reads the quest for a few seconds before it accepts. It stands meanwhile.
            if (questStep.Kind == QuestStepKind::Accept && bot.in_combat == 0 &&
                !realism::ReadReady(_options.ReadDelay, state.QuestRead, questStep.Quest, _options.RandomSeed, bot.guid, nowMs))
                break;
            realism::Close(state.QuestRead);
            BotAction quest;
            quest.bot_guid = bot.guid;
            quest.type = ActionType::kQuest;
            std::uint32_t const operation = questStep.Kind == QuestStepKind::Accept ? kQuestAccept
                                            : questStep.Kind == QuestStepKind::HandIn  ? kQuestHandIn
                                            : questStep.Kind == QuestStepKind::Reward ? kQuestReward
                                                                                      : kQuestAbandon;
            quest.spell_id = QuestCode(operation, questStep.Quest);
            // A stop first when the bot was walking, so the quest action is the last one applied and its answer is the one the policy reads.
            if (!(state.Last && state.Last->Type == ActionType::kStop))
            {
                BotAction stop;
                stop.bot_guid = bot.guid;
                stop.type = ActionType::kStop;
                actions.push_back(stop);
                state.Last = Decision{ ActionType::kStop, 0 };
            }
            actions.push_back(quest);
            _quests.NoteSent(state.Quest, operation, questStep.Quest, nowMs);
            wanted = Decision{ ActionType::kStop, 0 };
            break;
        }
        case QuestStepKind::Wait:
        case QuestStepKind::None:
            break;
    }

    // A goto order: walk to the point, then hold there (the order also set stay).
    if (state.GotoActive)
    {
        if (nowMs >= state.GotoUntilMs || DistanceTo(bot, state.GotoPoint) <= 3.0f)
        {
            state.GotoActive = false;
            // A train order: arrived (a timeout drops it), so the bot learns from the trainer in reach. The core refuses when none is.
            if (state.TrainAfterGoto && nowMs < state.GotoUntilMs)
            {
                BotAction train;
                train.bot_guid = bot.guid;
                train.type = ActionType::kTrain;
                actions.push_back(train);
                state.LastTrainLevel = bot.level;             // the periodic check does not repeat it right away
                state.NextTrainMs = nowMs + _options.TrainEveryMs;
            }
            state.TrainAfterGoto = false;
        }
        else
        {
            wanted = Decision{ ActionType::kMoveTo, 0, state.GotoPoint };
            moveWhy = "state.GotoPoint";
            routeBusy = true;
        }
    }
}

void BotPolicy::TurnIdle(DecideTick& tick, BotTurn& turn)
{
    std::uint64_t nowMs = tick.nowMs;
    std::vector<BotAction>& actions = tick.actions;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto& nearby = turn.nearby;
    auto const& routeOn = turn.routeOn;
    auto& questStep = turn.questStep;
    auto& routeBusy = turn.routeBusy;
    std::vector<std::uint32_t> const& huntEntries = *turn.huntEntries;
    auto& dungeonHold = turn.dungeonHold;
    auto const& noSelfPull = turn.noSelfPull;
    auto& hasTarget = turn.hasTarget;
    auto& wanted = turn.wanted;
    auto& moveWhy = turn.moveWhy;

    // ==== Idle-time dispatch: grinding, errands, hubs, vendor/service trips ====
    // An enemy near keeps the bot from idling, which keeps it from walking to grind spots. Only one the bot would actually pull counts: a bot
    // whose start zone shows only elites, packs of four or more, or creatures well above its level used to stand there for the whole run
    // (all 73 Undead and many Night Elf and Tauren bots of the 8-hour soak), neither fighting nor leaving. A bot that holds position by
    // order (a party waiting in a dungeon, no self pull) still counts any live creature.
    turn.enemyNear = false;
    auto& enemyNear = turn.enemyNear;
    if (dungeonHold || noSelfPull)
    {
        for (NearbyUnit const& unit : nearby)
            enemyNear = enemyNear || (IsLiveMob(unit) && (huntEntries.empty() ? !IsGrey(bot, unit) : true));
    }
    else
        enemyNear = PullTarget(bot, nearby, turn.AvoidedFn(), false, huntEntries, routeOn) != nullptr;
    turn.idle = !hasTarget && !enemyNear && wanted.Type == ActionType::kStop && questStep.Kind == QuestStepKind::None && !routeBusy;
    auto& idle = turn.idle;

    // A town errand (errand_trainers.h): walk to the trainer, then train once in reach. Only for a bot free to walk, as the vendor visit is. The
    // walk is a kMoveTo, so the taxi rules below apply to it as to any walk.
    if (state.Errand && !hasTarget && !enemyNear && wanted.Type == ActionType::kStop && questStep.Kind == QuestStepKind::None && !routeBusy)
    {
        ErrandOrder const& order = *state.Errand;
        float const dist = bot.map_id == order.Map ? std::hypot(bot.pos_x - order.Dest.X, bot.pos_y - order.Dest.Y) : 1e9f;
        if (dist <= kTrainReachYards && order.Auction)
        {
            // At the Auction House: the errand is done here. The listing visit (main.cpp) lists the items; the bot does not wait for it.
            state.ErrandResult = ErrandOutcome::kDone;
            state.Errand.reset();
            if (_options.OnEvent)
                _options.OnEvent("errand_done: bot=" + std::to_string(bot.guid) + " auction");
            wanted = Decision{};
        }
        else if (dist <= kTrainReachYards)
        {
            if (bot.in_combat == 0 && bot.life_state == 0 && nowMs >= state.ErrandTrainSentMs &&
                realism::ReadReady(_options.ReadDelay, state.TrainRead, kTrainReadKey, _options.RandomSeed, bot.guid, nowMs))
            {
                realism::Close(state.TrainRead);
                BotAction train;
                train.bot_guid = bot.guid;
                train.type = ActionType::kTrain;
                actions.push_back(train);
                state.ErrandTrainSentMs = nowMs + kErrandTrainRetryMs;
            }
            wanted = Decision{};
        }
        else
        {
            wanted = Decision{ ActionType::kMoveTo, 0, order.Dest };
            moveWhy = "order.Dest";
        }
        routeBusy = true;
        idle = false;
    }

    // Seeing nothing and doing nothing, for hubs (M7): the idle time starts when the bot first is idle, and any activity
    // resets it.
    if (!idle)
    {
        state.Idling = false;
        state.Wandering = false;
    }
    else if (!state.Idling)
    {
        state.Idling = true;
        state.IdleSinceMs = nowMs;
    }
}
