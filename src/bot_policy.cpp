// SPDX-License-Identifier: GPL-2.0-or-later
#include "bot_policy_internal.h"


bool BotPolicy::SafeToLogOut(BotSnapshot const& bot) const
{
    if (bot.in_combat != 0 || bot.life_state != 0 || bot.group_role != 0 || bot.group_invite_from != 0)
        return false;
    auto const found = _bots.find(bot.guid);
    if (found == _bots.end())
        return true;
    BotState const& state = found->second;
    return !state.ChatStay && state.ChatFollow == 0 && !state.Traveling && !state.SupplyTrip && state.SupplyCheckAtMs == 0 && !state.ServiceUrgent;
}

char const* ActionStatusName(BotActionStatus status)
{
    switch (status)
    {
        case BotActionStatus::kNone: return "none";
        case BotActionStatus::kApplied: return "applied";
        case BotActionStatus::kRefused: return "refused";
        case BotActionStatus::kUnknownSpell: return "unknown spell";
        case BotActionStatus::kNotKnown: return "spell not known";
        case BotActionStatus::kOutOfRange: return "out of range";
        case BotActionStatus::kOnCooldown: return "on cooldown";
        case BotActionStatus::kNotEnoughPower: return "not enough power";
        case BotActionStatus::kNoLineOfSight: return "no line of sight";
        case BotActionStatus::kInvalidTarget: return "invalid target";
        case BotActionStatus::kNotOnline: return "not online";
        case BotActionStatus::kBusy: return "busy casting";
        case BotActionStatus::kMoving: return "moving";
        case BotActionStatus::kNoRoute: return "no route";
        case BotActionStatus::kNeedsFocus: return "needs focus";
    }
    return "unknown";
}

// Reads the result of the bot's last action, once per sequence number, and sets when the spell may be tried again.
std::span<rotation::Row const> BotPolicy::RowsFor(std::uint32_t classId, std::string const& spec) const
{
    if (_options.Rotations)
        if (std::vector<rotation::Row> const* rows = rotation::RowsFor(*_options.Rotations, classId, spec))
            return *rows;
    return rotation::RowsForClass(classId);
}

std::string BotPolicy::SpecFor(std::uint64_t botGuid, std::uint32_t classId) const
{
    if (_options.SpecOf)
        if (std::string spec = _options.SpecOf(botGuid); !spec.empty())
            return spec;
    // No spec in the roster: the default of the class and the role the policy gives the bot (a set role, else a warrior in a party tanks, a healer class heals, the rest deal damage).
    std::uint32_t role = 0;
    if (auto const known = _bots.find(botGuid); known != _bots.end())
        role = known->second.Role_ == BotState::Role::Tank ? 1u : known->second.Role_ == BotState::Role::Healer ? 2u : known->second.Role_ == BotState::Role::Dps ? 3u : 0u;
    if (role == 0)
        role = (classId == kWarriorClass && _options.PartySize != 0) ? 1u : HealSpellFor(classId, 99) != 0 ? 2u : 3u;
    return rotation::DefaultSpec(classId, role);
}

std::uint64_t BotPolicy::DurationMsOf(std::uint32_t classId, std::uint32_t spell, std::string const& spec) const
{
    for (rotation::Row const& row : RowsFor(classId, spec))
        if (row.Spell == spell && row.DurationSec != 0)
            return std::uint64_t(row.DurationSec) * 1000;
    return DotDurationMs(spell);
}

void BotPolicy::StartErrand(std::uint64_t botGuid, ErrandOrder const& order)
{
    BotState& state = StateOf(botGuid);
    state.Errand = order;
    state.ErrandTrainSentMs = 0;
    state.ErrandRefusals = 0;
    state.ErrandResult = ErrandOutcome::kNone;
}

void BotPolicy::CancelErrand(std::uint64_t botGuid)
{
    auto const it = _bots.find(botGuid);
    if (it != _bots.end())
        it->second.Errand.reset();
}

ErrandOutcome BotPolicy::TakeErrandOutcome(std::uint64_t botGuid)
{
    auto const it = _bots.find(botGuid);
    if (it == _bots.end())
        return ErrandOutcome::kNone;
    ErrandOutcome const result = it->second.ErrandResult;
    it->second.ErrandResult = ErrandOutcome::kNone;
    return result;
}


std::vector<BotPolicy::TalentOrder> BotPolicy::TakeTalentOrders()
{
    std::vector<TalentOrder> orders;
    orders.swap(_talentOrders);
    return orders;
}

// The core refused the order (the bot was in combat): forget that it was sent and look again in a few seconds.
void BotPolicy::TalentOrderFailed(std::uint64_t bot, std::uint64_t nowMs)
{
    auto const known = _bots.find(bot);
    if (known == _bots.end())
        return;
    known->second.TalentLevel = 0;
    known->second.TalentRetryMs = nowMs + 5000;
}

PolicyOptions::ScenarioRoute BotPolicy::ScenarioRouteOf(PolicyOptions const& options)
{
    PolicyOptions::ScenarioRoute route;
    route.Route = options.Route;
    route.RouteMap = options.RouteMap;
    route.RouteBossAt = options.RouteBossAt;
    route.Events = options.Events;
    route.KillPriority = options.KillPriority;
    route.HasPortal = options.HasPortal;
    route.PortalMap = options.PortalMap;
    route.PortalPoint = options.PortalPoint;
    route.PortalInsideMap = options.PortalInsideMap;
    return route;
}

PolicyOptions::ScenarioRoute const& BotPolicy::ScenarioOf(std::uint64_t guid) const
{
    if (_scenarioRouteFn)
        if (PolicyOptions::ScenarioRoute const* own = _scenarioRouteFn(guid))
            return *own;
    return _scenarioRoute;
}

// Parties (M8): the lowest-guid bots, in guid order, cut into parties of PartySize. See bot_policy.h for why this
// is its own method.
void BotPolicy::BuildParties(WorldTickSnapshotView const& view,
                              std::unordered_map<std::uint64_t, std::size_t> const& indexOf,
                              std::unordered_map<std::uint64_t, std::uint64_t>& leaderOf,
                              std::unordered_map<std::uint64_t, std::vector<std::uint64_t>>& membersOf) const
{
    if (_options.PartySize == 0)
        return;

    // A leader or inviter reference that is one of this module's bots, as against a human player (both are players to the core).
    auto const isOurBot = [&indexOf](std::uint64_t ref) { return ref != 0 && indexOf.count(ref & forever_module::kUnitRefCounterMask) != 0; };

    std::vector<std::uint64_t> guids;
    guids.reserve(view.bots.size());
    for (BotSnapshot const& bot : view.bots)
    {
        // A player's own character (an alt), or a bot that sits in, or is invited to, a human's party, is not put into a bot party:
        // the human is the one who invites it and commands it.
        if (_options.IsAlt && _options.IsAlt(bot.guid))
            continue;
        std::uint64_t const heldBy = bot.group_role != 0 ? bot.group_leader_guid : bot.group_invite_from;
        if (heldBy != 0 && !isOurBot(heldBy))
            continue;
        // Not every bot is sociable: PartyPercent of them take part in the parties, chosen by the run's seed, the rest stay on their own.
        if (_options.PartyPercent < 100 &&
            bot_random::Below(bot_random::Draw(_options.RandomSeed, bot.guid, bot_random::kSaltParty), 100) >= _options.PartyPercent)
            continue;
        guids.push_back(bot.guid);
    }
    std::sort(guids.begin(), guids.end());

    // A party is one faction: the server refuses an invite across the sides. Each side's bots, in guid order, are cut into parties of PartySize,
    // and the PartyCount parties with the lowest first guids are kept. When the module can tell the sides, a bot whose side is not known (its race is
    // not in the table) is left out: it could land in a party of the other side, and the server would refuse that invite for ever. Without a side
    // lookup (the tests) every bot has side 0 and they all party together.
    bool const sidesKnown = bool(_options.PartySideOf) || bool(_options.SideOf);
    std::map<std::uint8_t, std::vector<std::uint64_t>> bySide;
    for (std::uint64_t guid : guids)
    {
        std::uint8_t const side = _options.PartySideOf ? _options.PartySideOf(guid) : (_options.SideOf ? _options.SideOf(guid) : std::uint8_t(0));
        if (sidesKnown && side == 0)
            continue;
        bySide[side].push_back(guid);
    }
    // With PartyPercent or RandomPct set the parties are not all of the full size: each is 2 to PartySize bots, the size drawn from its first bot.
    bool const variedSizes = _options.PartySize > 2 && (_options.PartyPercent < 100 || _options.RandomPct > 0);
    std::vector<std::vector<std::uint64_t>> parties;
    for (auto const& side : bySide)
        for (std::size_t from = 0; from < side.second.size();)
        {
            std::size_t size = _options.PartySize;
            if (variedSizes)
                size = 2 + bot_random::Below(bot_random::Draw(_options.RandomSeed, side.second[from], bot_random::kSaltPartySize), _options.PartySize - 1);
            std::size_t const to = std::min<std::size_t>(from + size, side.second.size());
            parties.emplace_back(side.second.begin() + from, side.second.begin() + to);
            from = to;
        }
    std::sort(parties.begin(), parties.end(), [](auto const& a, auto const& b) { return a.front() < b.front(); });
    if (parties.size() > _options.PartyCount)
        parties.resize(_options.PartyCount);

    for (std::vector<std::uint64_t> const& party : parties)
    {
        std::size_t const start = 0;
        std::size_t const end = party.size();
        // A tank leads, whatever its place in guid order (a raid's tanks come from the roster's prot specs). Only without one does a warrior with no role lead.
        auto const isTank = [this](std::uint64_t guid)
        {
            auto const known = _bots.find(guid);
            return known != _bots.end() && known->second.Role_ == BotState::Role::Tank;
        };
        std::size_t leader = end;
        for (std::size_t p = start; p < end && leader == end; ++p)
            if (isTank(party[p]))
                leader = p;
        for (std::size_t p = start; p < end && leader == end; ++p)
        {
            auto const known = _bots.find(party[p]);
            if (ClassFor(_options, party[p]) == kWarriorClass && (known == _bots.end() || known->second.Role_ == BotState::Role::Auto))
                leader = p;
        }
        if (leader == end)
            leader = start;
        std::uint64_t const leaderGuid = party[leader];
        for (std::size_t p = start; p < end; ++p)
        {
            leaderOf[party[p]] = leaderGuid;
            if (p != leader)
                membersOf[leaderGuid].push_back(party[p]);
        }
    }
}

std::vector<BotAction> BotPolicy::Decide(WorldTickSnapshotView const& view, std::uint64_t nowMs)
{
    std::vector<BotAction> actions;
    std::unordered_set<std::uint64_t> present;
    // The route is on per bot when parallel parties are run (each has its own director), else for all.
    auto const routeActiveOf = [this](std::uint64_t guid) { return _routeActiveFn ? _routeActiveFn(guid) : _routeActive; };
    auto const killedBossesOf = [this](std::uint64_t guid) -> std::set<std::uint32_t> const& { return _killedBossesFn ? _killedBossesFn(guid) : _killedBosses; };

    // Parties (M8): the lowest-guid bots, in guid order, cut into parties of PartySize. Computed once per tick.
    std::unordered_map<std::uint64_t, std::size_t> indexOf; // bot guid -> index in view.bots
    for (std::size_t i = 0; i < view.bots.size(); ++i)
        indexOf[view.bots[i].guid] = i;
    // A leader or inviter reference that is one of this module's bots, as against a human player (both are players to the core).
    auto const isOurBot = [&indexOf](std::uint64_t ref) { return ref != 0 && indexOf.count(ref & forever_module::kUnitRefCounterMask) != 0; };

    std::unordered_map<std::uint64_t, std::uint64_t> leaderOf;                // bot guid -> its party's leader
    std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> membersOf;  // leader guid -> the other members
    BuildParties(view, indexOf, leaderOf, membersOf);

    // "tank lead" (chat): the tank bot walks its group through the dungeon. For the dungeon rules the group becomes a party with the tank as its leader: the
    // tank pulls when the party is ready, the others follow it and join the fight. A tank that left the dungeon (its map has no route) stops leading.
    std::unordered_set<std::uint64_t> tankLed;
    for (BotSnapshot const& tank : view.bots)
    {
        auto const known = _bots.find(tank.guid);
        if (known == _bots.end() || !known->second.TankLead)
            continue;
        if (_options.DungeonsByMap.find(tank.map_id) == _options.DungeonsByMap.end() || tank.group_leader_guid == 0)
        {
            if (_options.OnEvent)
                _options.OnEvent("tank_lead_off: bot=" + std::to_string(tank.guid) + " map=" + std::to_string(tank.map_id) + " reason=" + (tank.group_leader_guid == 0 ? "no_group" : "no_route_for_map"));
            known->second.TankLead = false;
            known->second.Events.reset();
            continue;
        }
        tankLed.insert(tank.guid);
        leaderOf[tank.guid] = tank.guid;
        membersOf[tank.guid].clear();
        for (BotSnapshot const& other : view.bots)
            if (other.guid != tank.guid && other.group_leader_guid == tank.group_leader_guid)
            {
                leaderOf[other.guid] = tank.guid;
                membersOf[tank.guid].push_back(other.guid);
            }
    }

    // A leader that has a target has begun a fight: note it before the members decide, so they see the same start whatever the bot order.
    for (auto const& entry : membersOf)
    {
        auto const at = indexOf.find(entry.first);
        if (at == indexOf.end())
            continue;
        BotSnapshot const& leader = view.bots[at->second];
        BotState& leaderState = StateOf(leader.guid);
        if (leader.life_state == 0 && leader.threat_target_guid != 0 && !leaderState.InFight)
        {
            leaderState.InFight = true;
            leaderState.FightStartMs = nowMs;
        }
    }

    // The party of a leader: the leader and its members that are online.
    auto const forParty = [&](std::uint64_t leaderGuid, auto&& fn)
    {
        if (auto const at = indexOf.find(leaderGuid); at != indexOf.end())
            fn(view.bots[at->second]);
        auto const members = membersOf.find(leaderGuid);
        if (members == membersOf.end())
            return;
        for (std::uint64_t member : members->second)
            if (auto const at = indexOf.find(member); at != indexOf.end())
                fn(view.bots[at->second]);
    };
    auto const isRezzer = [&](BotSnapshot const& member)
    {
        return member.life_state == 0 && ResurrectSpellFor(ClassFor(_options, member.guid), member.level) != 0;
    };
    auto const partyFighting = [&](std::uint64_t leaderGuid)
    {
        bool fighting = false;
        forParty(leaderGuid, [&](BotSnapshot const& member) { fighting = fighting || (member.life_state == 0 && member.in_combat != 0); });
        return fighting;
    };
    // The distinct live hostiles this party's members are already fighting (their own threat targets, deduped). kMaxDungeonEngaged caps how far
    // the pack-focus widening below may reach for a hostile nobody has engaged yet (see kMaxDungeonEngaged).
    auto const partyEngagedTargets = [&](std::uint64_t leaderGuid)
    {
        std::set<std::uint64_t> targets;
        forParty(leaderGuid, [&](BotSnapshot const& member)
        {
            if (member.life_state == 0 && member.in_combat != 0 && member.threat_target_guid != 0)
                targets.insert(member.threat_target_guid);
        });
        return targets;
    };
    // Whether another member of this bot's party is already stopping a cast. The snapshot says who casts what, and a member with
    // casting_spell set is mid-cast (cast_remaining_ms is 0 for a spell in flight, which the core still counts as casting).
    // Only the party is looked at, so the cost does not grow with the bot count.
    auto const interruptTaken = [&](BotSnapshot const& bot, std::uint64_t leaderGuid) -> bool
    {
        if (leaderGuid == 0)
            return false; // not in a party of this module's: nobody to coordinate with
        bool taken = false;
        forParty(leaderGuid, [&](BotSnapshot const& member)
        {
            if (member.guid == bot.guid || member.life_state != 0 || member.casting_spell == 0)
                return;
            if (IsInterruptSpell(member.casting_spell))
                taken = true;
        });
        return taken;
    };
    // Whether the party waits for a dead member: 0 no, 1 for a resurrect, 2 for a ghost on its way back. Never in a fight, never
    // for a ghost whose run failed, and every wait is capped.
    auto const deadWait = [&](std::uint64_t leaderGuid) -> std::uint8_t
    {
        if (partyFighting(leaderGuid))
            return 0;
        bool rezzer = false;
        forParty(leaderGuid, [&](BotSnapshot const& member) { rezzer = rezzer || isRezzer(member); });
        std::uint8_t kind = 0;
        forParty(leaderGuid, [&](BotSnapshot const& member)
        {
            if (member.life_state == 0)
                return;
            static BotState const unseen; // a member the policy has not met yet counts as just down
            auto const known = _bots.find(member.guid);
            BotState const& theirs = known == _bots.end() ? unseen : known->second;
            if (member.life_state == 1)
            {
                if (rezzer && (theirs.Life != 1 || nowMs - theirs.DeathMs < _options.WaitForRezMs))
                    kind = std::max<std::uint8_t>(kind, 1);
            }
            else if (!theirs.RunFailed)
            {
                bool const within = theirs.Inside ? nowMs - theirs.InsideSinceMs < _options.WaitForGhostMs
                                    : !theirs.RunStarted || nowMs - theirs.RunStartMs < _options.CorpseRunTimeoutMs;
                if (within)
                    kind = 2;
            }
        });
        return kind;
    };

    // ==== Per-bot decision loop ====
    // Order within one bot's turn: read its last action's result, handle ground-effect avoidance and line-of-sight
    // recovery (both can `continue` before any other logic runs), then dead-bot handling, then (while alive) party
    // role/routing/follow, targeting and pulling, crowd control, pet taming, loot/fight-watchdog bookkeeping, and the
    // economy/social actions (crafting, vendor, chat orders) further below.
    for (std::size_t i = 0; i < view.bots.size(); ++i)
    {
        BotSnapshot const& bot = view.bots[i];
        present.insert(bot.guid);
        BotState& state = StateOf(bot.guid);
        // A scenario bot before its party is inside (the director's Waiting to Entering phases): it stays at the staging place. No quest, no grind spot, no
        // death-loop escape, no pull of its own; it fights only what attacks it (or its party). The 2026-10-29 Onyxia runs: the first tank walked to quest
        // 6582 at Emberstrife's den (10321, level 61 elite) and the others grinded the Wyrmbog's Firemanes, 204 pulls and 90 deaths in 15 minutes.
        bool const stagingHold = _stagingHoldFn && _stagingHoldFn(bot.guid);
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
                        continue;
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
                continue;
            }
            state.AvoidSetMs = 0;
        }


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

        std::span<NearbyUnit const> nearby = view.NearbyOf(i);
        auto const ledBy = leaderOf.find(bot.guid);
        std::uint64_t const deadParty = ledBy == leaderOf.end() ? 0 : ledBy->second;

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
                continue;
            }

            if (bot.life_state == 1)
            {
                if (state.ReleaseSent && nowMs < state.ReleaseSentMs + kReleaseRetryMs)
                    continue;
                std::uint64_t const waited = nowMs - state.DeathMs;
                bool anyAlive = false;
                bool rezzer = false;
                // The core refuses a release only for a corpse that is itself in combat; a fight elsewhere in the party does not hold it (0.36.2: the dungeon
                // ghosts waited out whole fights, 44-87 s, with no rezzer up). A ghost released in a fight waits at the entrance, or goes to the leader once it is quiet.
                bool const fighting = bot.in_combat != 0;
                bool partyFights = fighting;
                if (deadParty != 0)
                    forParty(deadParty, [&](BotSnapshot const& member)
                    {
                        if (member.guid == bot.guid || member.life_state != 0)
                            return;
                        anyAlive = true;
                        rezzer = rezzer || isRezzer(member);
                        partyFights = partyFights || member.in_combat != 0;
                    });
                bool const dungeonParty = routeActiveOf(bot.guid) || (deadParty != 0 && tankLed.count(deadParty) != 0);
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
                        continue;
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
                    continue; // waiting for a resurrect
                if (fighting && waited < releaseCombatHoldMs)
                {
                    // The core refuses a release in combat, and this corpse is still in combat: held back, capped.
                    if (!state.ReleaseHeldLogged)
                    {
                        state.ReleaseHeldLogged = true;
                        event("release_held: bot=" + std::to_string(bot.guid) + " reason=combat would_release=" + reason);
                    }
                    continue;
                }
                BotAction release;
                release.bot_guid = bot.guid;
                release.type = ActionType::kReleaseSpirit;
                actions.push_back(release);
                state.ReleaseSent = true;
                state.ReleaseSentMs = nowMs;
                event(std::string("release: bot=") + std::to_string(bot.guid) + " reason=" + reason + " waited_ms=" + std::to_string(waited) +
                      " rezzer_up=" + (rezzer ? "yes" : "no") + " alive_members=" + (anyAlive ? "yes" : "no"));
                continue;
            }

            // A live Onyxian Warder near the corpse, or near a living member of the ghost's party: the ghost stays a ghost. Bringing it back there (the teleport to the entrance or to
            // the leader, which the core answers with a resurrect at 50% health on entering the dungeon where the corpse is; the leader's revive; the corpse reclaim) put it among
            // Fire Novas again 20 s after it died (2026-10-29: 1935 died 20:46:02, back 20:46:22, dead 20:46:38). This is not DungeonReviveOutOfCombat: that option only governs the
            // leader's revive; the 20 s revival came from the entrance teleport of the ghost branch below, which no option stops.
            if (deadParty != 0 && bot.corpse_valid != 0 && (routeActiveOf(bot.guid) || tankLed.count(deadParty) != 0))
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
                forParty(deadParty, [&](BotSnapshot const& member)
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
                    continue;
                }
            }

            // Dungeon party, out of combat (DungeonReviveOutOfCombat): a ghost is revived by its leader instead of walking back to its corpse, once the
            // leader is alive and not fighting. The core's revive order puts the bot at the leader's side, or where it fell when the leader is on another
            // map. The ghost waits between tries and walks on only once they are used up (kOutOfCombatReviveTries), or when the leader is not up.
            // Only in the dungeon (the owner's rule of 2026-10-22): a ghost still on the outside map walks to the instance door and is revived on
            // entering it, whatever the leader is doing.
            if (_options.DungeonReviveOutOfCombat && deadParty != 0 && deadParty != bot.guid && bot.in_combat == 0 && state.ReviveAttempts < kOutOfCombatReviveTries &&
                (routeActiveOf(bot.guid) || tankLed.count(deadParty) != 0) && bot.corpse_valid != 0 && bot.map_id == bot.corpse_map)
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
                    continue;
                }
            }

            // A ghost: back to the corpse. A corpse on another map (an instance, a ghost is released outside) is reached through the portal.
            if (state.RunFailed)
                continue;
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
                continue;
            }
            // A ghost of a dungeon party goes back to the instance entrance and is revived there (the owner's rule of 2026-10-25), not walked back to its corpse.
            // Outside the instance it is teleported in, and inside it is put at the entrance when it is further off. It waits there for the leader's revive
            // (the revive above). Only when no revive comes within kDungeonEntryWaitMs does the ghost fall back to its corpse run.
            auto const entryOf = _options.DungeonsByMap.find(bot.corpse_map);
            bool const instanceParty = routeActiveOf(bot.guid) || (deadParty != 0 && tankLed.count(deadParty) != 0);
            if (instanceParty && entryOf != _options.DungeonsByMap.end() && entryOf->second.HasEntry && state.EntrySinceMs == 0)
                state.EntrySinceMs = nowMs;
            bool const atEntry = bot.map_id == bot.corpse_map && entryOf != _options.DungeonsByMap.end() && entryOf->second.HasEntry &&
                                 DistanceTo(bot, entryOf->second.Entry) <= kEntryYards;
            // Out of combat with the party's leader up: the ghost does not wait at the entrance or walk to its corpse. It is teleported to the leader and stays
            // by it for the revive (above). A fight in the party brings back the entrance wait below.
            auto const leaderAt = deadParty != 0 && deadParty != bot.guid ? indexOf.find(deadParty) : indexOf.end();
            if (instanceParty && leaderAt != indexOf.end() && view.bots[leaderAt->second].life_state == 0 && !partyFighting(deadParty))
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
                continue;
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
                continue; // at the entrance it waits for the leader's revive (the revive above); the corpse run is not walked
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
                continue;
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
                    continue;
                }
                fail("timeout");
                continue;
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
            continue;
        }

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

        bool const lowHealth = bot.max_health > 0 && std::uint64_t(bot.health) * 100 < std::uint64_t(bot.max_health) * kDisengagePct;

        // ==== Party role, dungeon routing and follow logic (alive bots) ====
        // The bot's class: its own (from the class table) or the configured one. Ranged classes approach to range.
        std::uint32_t const classId = ClassFor(_options, bot.guid);
        // A spec in the roster sets the role of a bot that has none: a tank spec tanks, a healer spec heals, a damage spec does not heal on its own (a shadow priest).
        if (!state.SpecRoleApplied && state.Role_ == BotState::Role::Auto && _options.SpecOf)
        {
            state.SpecRoleApplied = true;
            if (std::string const rosterSpec = _options.SpecOf(bot.guid); !rosterSpec.empty())
                if (std::uint32_t const role = rotation::SpecRole(classId, rosterSpec))
                    state.Role_ = role == 1 ? BotState::Role::Tank : role == 2 ? BotState::Role::Healer : BotState::Role::Dps;
        }
        std::string const spec = SpecFor(bot.guid, classId);

        // The bot's party: a leader is the bot that invites, a member follows its leader. The leader's snapshot is the one
        // this tick's view holds, when the leader is online.
        auto const led = leaderOf.find(bot.guid);
        std::uint64_t const partyLeader = led == leaderOf.end() ? 0 : led->second;
        bool const partyLeaderBot = partyLeader != 0 && partyLeader == bot.guid;
        bool const partyMember = partyLeader != 0 && !partyLeaderBot;
        bool const routeOn = routeActiveOf(bot.guid) || (partyLeader != 0 && tankLed.count(partyLeader) != 0);   // the dungeon party rules apply to this bot
        // A dungeon party's bot on the outside map (a wipe left it at the graveyard, a rescue teleport was refused in combat): it goes back
        // through the instance door and starts no fight on the way. The owner's rule of 2026-10-22: a bot of a dungeon party must not be
        // able to die outside the dungeon; it comes back as a ghost would and is revived in the dungeon it died in.
        // The route, events, kill priority and portal of the bot's own party's dungeon (one of several, or the scenario's only one).
        PolicyOptions::ScenarioRoute const& scenario = ScenarioOf(bot.guid);
        bool const returningToDungeon = routeOn && scenario.HasPortal && scenario.PortalInsideMap != 0 &&
                                       bot.map_id != scenario.PortalInsideMap && bot.in_combat == 0;
        std::uint64_t const leaderRef = partyLeader != 0 ? PlayerRef(partyLeader) : 0;
        BotSnapshot const* leaderSnap = nullptr;
        if (partyMember)
        {
            if (auto const at = indexOf.find(partyLeader); at != indexOf.end())
                leaderSnap = &view.bots[at->second];
        }
        bool const atLeader = leaderSnap && Distance2d(bot, *leaderSnap) <= (state.FollowYards > 0.0f ? state.FollowYards : state.LeaderYards);

        // Who an idle bot follows: a member its leader (and stays put once close). A bot nobody has invited into a
        // group does not follow a nearby player just because it is close (that made every uncontrolled bot latch
        // onto whichever real player walked past it); it only follows when told to (ChatFollow, below) or led.
        std::uint64_t followRef = 0;
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
        std::uint8_t const deadWaitKind = partyLeader != 0 ? deadWait(partyLeader) : std::uint8_t(0);
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

        // Quests (M5): the planner's step for this bot. A party member follows its leader instead, and a hurt or fighting bot
        // leaves quests alone until it is done.
        QuestStep questStep;
        bool routeBusy = false;
        bool const questsAllowed = _quests.Enabled() && !partyMember && !stagingHold && !(routeActiveOf(bot.guid) && scenario.RouteMap != 0) /* a dungeon run: the leader walks the route, not to a quest giver */ && !lowHealth && bot.in_combat == 0 && !state.ChatStay && state.ChatFollow == 0 && nowMs >= state.QuestPausedUntilMs;
        if (questsAllowed)
            questStep = _quests.Next(bot, state.Quest, nowMs, QuestBotInfo{ classId, _options.RaceOf ? _options.RaceOf(bot.guid) : 0u });
        // Every five minutes, what the quest planner is doing for this bot and what keeps it from doing more (the events are the only record,
        // since a planner that takes no quest logs nothing otherwise).
        if (_options.OnEvent && nowMs - state.QuestDiagAtMs >= 300000)
        {
            state.QuestDiagAtMs = nowMs;
            static constexpr char const* kinds[] = { "none", "walk_to", "accept", "hand_in", "hunt", "abandon", "reward", "wait" };
            _options.OnEvent("quest_state: bot=" + std::to_string(bot.guid) + " level=" + std::to_string(bot.level) + " map=" + std::to_string(bot.map_id) +
                             " allowed=" + std::to_string(questsAllowed) + " enabled=" + std::to_string(_quests.Enabled()) +
                             " party=" + std::to_string(partyMember) + " low_health=" + std::to_string(lowHealth) + " in_combat=" + std::to_string(bot.in_combat) +
                             " chat_stay=" + std::to_string(state.ChatStay) + " chat_follow=" + std::to_string(state.ChatFollow != 0) +
                             " step=" + kinds[int(questStep.Kind)] + " quest=" + std::to_string(questStep.Quest) +
                             " log=" + std::to_string(bot.quest_count) + " start_index=" + std::to_string(state.Quest.StartIndex) +
                             " memory_done=" + std::to_string(state.Quest.Done.size()) + " memory_log=" + std::to_string(state.Quest.Progress.size()));
        }
        std::vector<std::uint32_t> const noEntries;
        std::vector<std::uint32_t> const& huntEntries = questStep.Kind == QuestStepKind::Hunt ? questStep.Entries : noEntries;

        // ==== Target acquisition and pulling ====
        // The target is the threat target (even when it is out of view), otherwise the closest live hostile. A member with
        // no threat of its own fights its leader's target, when that is in view.
        // Dungeon pull control (the route is active): the leader pulls only when its party is ready, the others never pull and join a
        // fight once the tank has held it for a moment. A bot already in combat defends itself whatever the rule says.
        bool dungeonHold = false;
        bool tankHeadStart = false;
        if (routeOn && partyLeader != 0 && bot.in_combat == 0)
        {
            if (partyLeaderBot)
            {
                bool ready = true;
                std::string not_ready_reason;
                auto const checkReady = [&](BotSnapshot const& member)
                {
                    if (member.life_state != 0)
                        return;
                    if (member.max_health > 0 && std::uint64_t(member.health) * 100 < std::uint64_t(member.max_health) * (_options.TacticRest ? kPartyReadyHealthPct : kPartyReadyHealthOldPct))
                    {
                        ready = false;
                        not_ready_reason = "health bot=" + std::to_string(member.guid) + " pct=" + std::to_string(std::uint64_t(member.health) * 100 / member.max_health);
                    }
                    else if (member.power_type == 0 && member.max_power > 0 &&
                             std::uint64_t(member.power) * 100 < std::uint64_t(member.max_power) * (_options.TacticRest ? kPartyReadyPowerPct : kPartyReadyPowerOldPct))
                    {
                        ready = false;
                        not_ready_reason = "mana bot=" + std::to_string(member.guid) + " pct=" + std::to_string(std::uint64_t(member.power) * 100 / member.max_power);
                    }
                    else if (Distance2d(bot, member) > kRouteWaitYards && state.RegroupStuck.count(member.guid) == 0)
                    {
                        ready = false;
                        not_ready_reason = "far bot=" + std::to_string(member.guid);
                    }
                };
                // Regroup (0.35.0): how many members are out of range (kRouteWaitYards), and for how long. A member out of range for kRegroupStuckMs is
                // stuck: logged once, and the party no longer waits for it (checkReady and the route walk). Since 0.36.2 it stays stuck until the regroup
                // ends (regroup_done), and only counts as back inside kRegroupClearYards: a member that trails the leader at the edge of kRouteWaitYards
                // flipped the flag (and the log line) on every step in and out of range.
                if (state.RegroupSinceMs == 0)
                    state.RegroupStuck.clear();
                std::uint32_t farMembers = 0;
                for (std::uint64_t member : membersOf[bot.guid])
                {
                    auto const at = indexOf.find(member);
                    bool const flagged = state.RegroupStuck.count(member) != 0;
                    if (at == indexOf.end() || view.bots[at->second].life_state != 0 ||
                        Distance2d(bot, view.bots[at->second]) <= (flagged ? kRegroupClearYards : kRouteWaitYards))
                    {
                        state.RegroupFarSinceMs.erase(member);
                        continue;
                    }
                    ++farMembers;
                    auto const since = state.RegroupFarSinceMs.emplace(member, nowMs).first;
                    if (nowMs - since->second >= kRegroupStuckMs && state.RegroupStuck.insert(member).second)
                    {
                        ++state.RegroupStuckCount;
                        if (_options.OnEvent)
                            _options.OnEvent("regroup_stuck: leader=" + std::to_string(bot.guid) + " bot=" + std::to_string(member) + " yards=" +
                                             std::to_string(int(Distance2d(bot, view.bots[at->second]))) + " in_combat=" + std::to_string(view.bots[at->second].in_combat) +
                                             " waited_ms=" + std::to_string(nowMs - since->second));
                    }
                }
                checkReady(bot);
                for (std::uint64_t member : membersOf[bot.guid])
                    if (auto const at = indexOf.find(member); at != indexOf.end())
                        checkReady(view.bots[at->second]);
                if (ready && deadWaitKind != 0)
                {
                    ready = false;
                    not_ready_reason = deadWaitKind == 1 ? "dead_member_rez" : "dead_member_ghost";
                }
                // The hold ends once the party has been ready for kPullHoldClearMs in a row. A party that flips between ready and not ready every tick (a member
                // that is a ghost one moment and not the next) must not restart the wait each time, or it never ends (the soak's Deadmines run waited for 40 minutes).
                if (ready && state.HoldSinceMs != 0 && nowMs - state.HoldLastNotReadyMs >= kPullHoldClearMs)
                {
                    if (_options.OnEvent)
                        _options.OnEvent("pull hold ended: leader=" + std::to_string(bot.guid) + " waited_ms=" + std::to_string(nowMs - state.HoldSinceMs));
                    state.HoldSinceMs = 0;
                }
                // A regroup starts when a member is dead or out of range, and ends once nobody is dead or out of range and the party is ready again (the pull
                // hold has ended). A stuck member still out of range keeps it on: the stuck set must not be dropped while the member is still far.
                if (!ready && state.RegroupSinceMs == 0 && (deadWaitKind != 0 || farMembers != 0))
                {
                    state.RegroupSinceMs = nowMs;
                    state.RegroupStuckCount = 0;
                    if (_options.OnEvent)
                        _options.OnEvent("regroup_start: leader=" + std::to_string(bot.guid) + " reason=" + (deadWaitKind != 0 ? "dead_member" : "member_far") +
                                         " far=" + std::to_string(farMembers));
                }
                if (ready && state.RegroupSinceMs != 0 && state.HoldSinceMs == 0 && farMembers == 0 && deadWaitKind == 0)
                {
                    if (_options.OnEvent)
                        _options.OnEvent("regroup_done: leader=" + std::to_string(bot.guid) + " waited_ms=" + std::to_string(nowMs - state.RegroupSinceMs) +
                                         " stuck=" + std::to_string(state.RegroupStuckCount));
                    state.RegroupSinceMs = 0;
                    state.RegroupStuck.clear();
                }
                if (!ready)
                {
                    if (state.HoldSinceMs == 0)
                    {
                        state.HoldSinceMs = nowMs;
                        if (_options.OnEvent)
                            _options.OnEvent("pull hold started: leader=" + std::to_string(bot.guid) + " reason=" + not_ready_reason);
                    }
                    state.HoldLastNotReadyMs = nowMs;
                }
                dungeonHold = !ready ? nowMs - state.HoldSinceMs < kPullHoldMaxMs : false;
            }
            else
            {
                dungeonHold = true;
                auto const tank = _bots.find(partyLeader);
                tankHeadStart = tank != _bots.end() && tank->second.InFight && nowMs - tank->second.FightStartMs < kTankHeadStartMs;
            }
        }
        if (returningToDungeon)
            dungeonHold = true; // walking back to the dungeon pulls nothing

        NearbyUnit const* targetUnit = nullptr;
        auto const avoided = [&state, nowMs](std::uint64_t ref)
        {
            auto const until = state.AvoidUntilMs.find(ref);
            return until != state.AvoidUntilMs.end() && nowMs < until->second;
        };
        std::uint64_t targetRef = bot.threat_target_guid;
        if (targetRef != 0 && avoided(targetRef))
            targetRef = 0; // it refused us: the bot does not fight it for now
        if (returningToDungeon)
            targetRef = 0; // nothing on the way back is worth a fight: a bot that stopped to clear the road died outside the dungeon
        // Chat orders: a passive or staying bot starts no fight of its own; a pull order points the bot at the speaker's target.
        bool groupFighting = false;
        if (bot.group_leader_guid != 0)
            for (BotSnapshot const& other : view.bots)
                groupFighting = groupFighting || (other.group_leader_guid == bot.group_leader_guid && other.in_combat != 0 && other.life_state == 0);
        if (partyLeader != 0 && partyFighting(partyLeader))
            groupFighting = true;
        bool const noSelfPull = (state.Stance_ == BotState::Stance::Passive || state.ChatStay ||
                                 (state.Stance_ == BotState::Stance::Defensive && !groupFighting && state.Role_ != BotState::Role::Tank)) &&
                                bot.in_combat == 0;
        if (state.Focus != 0 && (bot.in_combat != 0 || groupFighting))
        {
            if (FindLiveMob(nearby, state.Focus))
            {
                state.FocusMissingMs = 0;
                if (!avoided(state.Focus))
                    targetRef = state.Focus; // the focus target first
            }
            else if (state.FocusMissingMs == 0)
                state.FocusMissingMs = nowMs;
            else if (nowMs - state.FocusMissingMs > 30000)
            {
                state.Focus = 0; // out of view for a good while: forgotten
                state.FocusMissingMs = 0;
            }
        }
        if (state.ForcedTarget != 0)
        {
            if (nowMs >= state.ForcedUntilMs || !FindLiveMob(nearby, state.ForcedTarget))
                state.ForcedTarget = 0;
            else if (targetRef == 0 && !avoided(state.ForcedTarget))
                targetRef = state.ForcedTarget;
        }
        if (targetRef == 0 && partyMember && leaderSnap && leaderSnap->threat_target_guid != 0 && !tankHeadStart && !noSelfPull &&
            !avoided(leaderSnap->threat_target_guid))
        {
            // A leader that has not engaged the target (not in combat: it is still walking to it) and that stands on another level from the bot is not assisted: the
            // bot would stand under it for as long as the leader tried (see OnOtherLevel).
            NearbyUnit const* assist = FindLiveMob(nearby, leaderSnap->threat_target_guid);
            if (assist && leaderSnap->in_combat == 0 && bot.in_combat == 0 && OnOtherLevel(bot, *assist))
                assist = nullptr;
            if (assist)
            {
                targetRef = assist->guid;
                // Focus fire (the dungeon's kill priority): a damage dealer fights the best-ranked live hostile of the tank's pack, not just the tank's own target: the casters
                // and healers of the pack first (the dungeon file's kill_priority), then any unit that is casting right now, then the weakest.
                bool const canInterrupt = classId == 2 || classId == 4 || classId == 7 || classId == 8; // paladin, rogue, shaman, mage: an interrupt row in the rotation
                bool const interruptNow = _options.TacticInterrupt && canInterrupt && !interruptTaken(bot, partyLeader);
                // Deadmines Foundry (Gilnid): a Goblin Engineer (622) casts Summon Remote-Controlled Golem (3605), an add that is immune to most
                // abilities and despawns only when its summoner dies (Wowhead's Deadmines guide: "kill the GOBLIN first"). The snapshot does not
                // say which spell a nearby unit is casting (NearbyUnit::casting is a bool), so this cannot be limited to that one cast; instead,
                // once a golem is up, every engineer nearby outranks everything else for every damage dealer, with or without a tactic switched
                // on, the same way avoiding a ground effect is not optional. Without this, four engineers kept the pack resupplied with golems
                // faster than a 5-bot party could add them to its kill order on its own (verified on the sim: deaths to entry 2520 did not stop
                // with focus and interrupt alone).
                bool golemNearby = false;
                for (NearbyUnit const& unit : nearby)
                    if (unit.is_hostile && unit.health != 0 && forever_module::UnitRefEntry(unit.guid) == kRemoteControlledGolemEntry)
                    {
                        golemNearby = true;
                        break;
                    }
                // Ragefire's Searing Blade Warlock, once its Voidwalker Minion is in view (see kSearingBladeWarlockEntry): it comes first, as the
                // engineer does with a golem up, and from farther than the pack reach, because the minion goes away when the Warlock dies.
                bool voidwalkerNearby = false;
                for (NearbyUnit const& unit : nearby)
                    if (unit.is_hostile && unit.health != 0 && forever_module::UnitRefEntry(unit.guid) == kVoidwalkerMinionEntry)
                    {
                        voidwalkerNearby = true;
                        break;
                    }
                auto const isSummonerToKill = [golemNearby, voidwalkerNearby](NearbyUnit const& unit)
                {
                    std::uint32_t const entry = forever_module::UnitRefEntry(unit.guid);
                    return (golemNearby && entry == kGoblinEngineerEntry) || (voidwalkerNearby && entry == kSearingBladeWarlockEntry);
                };
                if (routeOn && HealSpellFor(classId, bot.level) == 0 && ((_options.TacticFocus && !scenario.KillPriority.empty()) || interruptNow || golemNearby || voidwalkerNearby))
                {
                    auto const rankOf = [this, &scenario](NearbyUnit const& unit)
                    {
                        if (!_options.TacticFocus)
                            return std::uint32_t(1000);
                        auto const at = scenario.KillPriority.find(forever_module::UnitRefEntry(unit.guid));
                        return at == scenario.KillPriority.end() ? std::uint32_t(1000) : at->second;
                    };
                    NearbyUnit const* best = assist;
                    // A bot that can interrupt turns on a casting unit of the pack first (its rotation then fires the interrupt at its target); then
                    // an engineer with a golem already up (see above); then the priority list; then the casters; then the weakest.
                    auto const keyOf = [&rankOf, &isSummonerToKill, interruptNow](NearbyUnit const& unit)
                    {
                        int tier = 2;
                        if (interruptNow && unit.casting != 0)
                            tier = 0;
                        else if (isSummonerToKill(unit))
                            tier = 1;
                        return std::make_tuple(tier, rankOf(unit), unit.casting != 0 ? 0 : 1, unit.health);
                    };
                    // kMaxDungeonEngaged: past this many distinct hostiles already fought, the scan no longer reaches for a hostile nobody told
                    // the party about (an unlisted creature it just happens to notice nearby, rank 1000 below); it still freely re-targets among
                    // the hostiles already in the fight, interrupts one, and reaches for any of the dungeon file's own kill_priority list whatever
                    // the count already engaged -- that list is the dungeon author's own "kill this one first, whatever else is going on" (the
                    // Searing Blade camp's own Warlock and its Voidwalker Minion companion, entry 8996, which joins the pack's melee late and was
                    // 63% of a soak's Ragefire deaths once it did: capping the reach for either past two engaged would have silenced the one tool
                    // meant to make the party turn on them). The interrupt and golem-engineer tiers are exempt the same way.
                    std::set<std::uint64_t> const engaged = partyEngagedTargets(partyLeader);
                    bool const atEngagementCap = engaged.size() >= kMaxDungeonEngaged;
                    auto const reachable = [&](NearbyUnit const& unit)
                    {
                        bool const specialTier = (interruptNow && unit.casting != 0) || isSummonerToKill(unit) || rankOf(unit) != 1000;
                        return specialTier || engaged.count(unit.guid) != 0 || !atEngagementCap;
                    };
                    for (NearbyUnit const& unit : nearby)
                        if (unit.is_hostile && unit.health != 0 && IsLiveMob(unit) && !avoided(unit.guid) && unit.guid != assist->guid &&
                            std::hypot(unit.pos_x - assist->pos_x, unit.pos_y - assist->pos_y) <=
                                (voidwalkerNearby && forever_module::UnitRefEntry(unit.guid) == kSearingBladeWarlockEntry ? kSummonerReachYards : kFocusPackYards) &&
                            reachable(unit) &&
                            keyOf(unit) < keyOf(*best))
                            best = &unit;
                    if (best != assist)
                    {
                        targetRef = best->guid;
                        if (state.FocusLoggedFor != best->guid && _options.OnEvent)
                        {
                            bool const engineerUnderGolemPressure = golemNearby && forever_module::UnitRefEntry(best->guid) == kGoblinEngineerEntry;
                            state.FocusLoggedFor = best->guid;
                            _options.OnEvent(std::string(interruptNow && best->casting != 0 ? "interrupt_target: bot=" : engineerUnderGolemPressure ? "golem_engineer: bot=" : "focus: bot=") + std::to_string(bot.guid) + " entry=" + std::to_string(forever_module::UnitRefEntry(best->guid)) + " instead_of=" +
                                             std::to_string(forever_module::UnitRefEntry(assist->guid)) + (best->casting != 0 ? " casting" : ""));
                        }
                    }
                }
            }
        }
        // ==== Crowd control tactics ====
        // Cc tactic: a rogue, mage or warlock with a cc spell for its level (CcSpellForClass, CcLearnLevel) holds one extra member of
        // the tank's pack -- a unit nobody has engaged yet (partyEngagedTargets, the same set focus and interrupt use), not casting
        // (interrupt's to pick, not held mid-cast for nothing) and not the dungeon's own kill priority (never hold what the party
        // means to kill first), of a creature type the spell affects (CcAffects, CreatureTypeOf). One bot, one unit at a time, the
        // same rule interrupt uses for a cast: a party member already holding one (another bot's Dots entry for a cc spell, still
        // running) means this bot does not start a second. Once a unit is held the bot keeps its own normal target and rotation
        // (below); only the GCD that sends the hold, and later its refresh a little before it would run out (kCcRefreshBeforeMs), is
        // spent on it. A pack with no match for any cc-capable class present (no learned spell, or no pack member of a type it
        // affects) does nothing, same as a fight with nothing to interrupt.
        if (_options.TacticCc && routeOn && partyMember && leaderSnap && leaderSnap->threat_target_guid != 0 && nowMs >= state.NextCastMs && !IsCasting(bot))
        {
            std::uint32_t const ccSpell = CcSpellForClass(classId);
            std::uint32_t const ccLearn = ccSpell != 0 ? CcLearnLevel(ccSpell) : 0;
            if (ccSpell != 0 && (bot.level == 0 || bot.level >= ccLearn) && CanPay(_options.SpellCosts.get(), ccSpell, bot))
            {
                auto const retry = state.RetryAtMs.find(ccSpell);
                if (retry == state.RetryAtMs.end() || nowMs >= retry->second)
                {
                    auto const mine = state.Dots.find(ccSpell);
                    NearbyUnit const* held = mine != state.Dots.end() ? FindLiveMob(nearby, mine->second.Target) : nullptr;
                    bool const dueForRefresh = held != nullptr && nowMs + kCcRefreshBeforeMs >= mine->second.UntilMs;
                    NearbyUnit const* candidate = (held != nullptr && dueForRefresh) ? held : nullptr;
                    if (held == nullptr)
                    {
                        // Someone else of this party already holds a unit: only one of the pack is held at a time.
                        bool otherHolder = false;
                        forParty(partyLeader, [&](BotSnapshot const& member)
                        {
                            if (otherHolder || member.guid == bot.guid)
                                return;
                            auto const known = _bots.find(member.guid);
                            if (known == _bots.end())
                                return;
                            for (std::uint32_t const otherSpell : { kSapSpell, kPolymorphSpell, kBanishSpell })
                            {
                                auto const theirs = known->second.Dots.find(otherSpell);
                                if (theirs != known->second.Dots.end() && nowMs < theirs->second.UntilMs)
                                    otherHolder = true;
                            }
                        });
                        if (!otherHolder)
                        {
                            NearbyUnit const* tankTarget = FindLiveMob(nearby, leaderSnap->threat_target_guid);
                            if (tankTarget != nullptr)
                            {
                                std::set<std::uint64_t> const engaged = partyEngagedTargets(partyLeader);
                                for (NearbyUnit const& unit : nearby)
                                    if (unit.is_hostile && unit.health != 0 && IsLiveMob(unit) && !avoided(unit.guid) && unit.elite == 0 &&
                                        unit.casting == 0 && // a casting unit is interrupt's to pick, not held mid-cast for nothing
                                        engaged.count(unit.guid) == 0 &&
                                        scenario.KillPriority.find(forever_module::UnitRefEntry(unit.guid)) == scenario.KillPriority.end() &&
                                        CcAffects(ccSpell, CreatureTypeOf(forever_module::UnitRefEntry(unit.guid))) &&
                                        std::hypot(unit.pos_x - tankTarget->pos_x, unit.pos_y - tankTarget->pos_y) <= kFocusPackYards)
                                    {
                                        candidate = &unit;
                                        break;
                                    }
                            }
                        }
                    }
                    if (candidate != nullptr)
                    {
                        BotAction cc;
                        cc.bot_guid = bot.guid;
                        cc.type = ActionType::kCastSpell;
                        cc.spell_id = ccSpell;
                        cc.target_guid = candidate->guid;
                        actions.push_back(cc);
                        state.CastTarget[ccSpell] = candidate->guid;
                        state.NextCastMs = nowMs + kGlobalCooldownMs;
                        if (_options.OnEvent && mine == state.Dots.end())
                            _options.OnEvent("cc_target: bot=" + std::to_string(bot.guid) + " spell=" + std::to_string(ccSpell) + " entry=" +
                                             std::to_string(forever_module::UnitRefEntry(candidate->guid)));
                        continue;
                    }
                }
            }
        }
        // ==== Hunter pet taming ====
        // A hunter with no pet that has a beast to tame in view does not pull whatever else is standing there (Westfall's hostile Fleshrippers made the 2026-10-26 hunters fight and get hurt
        // before they ever cast Tame Beast); it tames, and defends itself only when something attacks it. The hold of a cast in progress counts as well.
        bool const tamingPhase = classId == kHunterClass && _tamingFn && _tamingFn(bot.guid);
        bool const tameNow = classId == kHunterClass && _options.TameBeasts && !_options.TameBeasts->ByEntry.empty() && bot.life_state == 0 && (bot.group_role == 0 || tamingPhase) &&
                             (bot.level == 0 || bot.level >= kTameMinLevel) && bot.in_combat == 0 && PetOf(bot) == PetKnown::kNone &&
                             (state.TameUntilMs != 0 || (nowMs >= state.TameRetryAtMs && tame::PickTameTarget(bot, nearby, *_options.TameBeasts, kTameSearchYards) != nullptr));
        if (targetRef != 0)
        {
            for (NearbyUnit const& unit : nearby)
                if (unit.guid == targetRef)
                    targetUnit = &unit;
        }
        else if (NearbyUnit const* hostile = (dungeonHold || noSelfPull || tameNow || (stagingHold && bot.in_combat == 0)) ? nullptr
                     : PullTarget(bot, nearby, avoided, bot.in_combat != 0, huntEntries, routeOn))
        {
            targetUnit = hostile;
            targetRef = hostile->guid;
        }
        bool hasTarget = targetRef != 0 && !lowHealth;

        // Taming (Tame Beast). A hunter with no pet, not in a party, out of combat: walk to the nearest beast of data/tame_beasts.txt its level allows, cast Tame Beast on it and
        // hold still for the channel. The snapshot's pet_state tells whether it worked (the core creates the pet when the channel ends). Not for a hunter that fights or runs a route.
        if (classId == kHunterClass && _options.TameBeasts && !_options.TameBeasts->ByEntry.empty() && bot.life_state == 0 && (bot.group_role == 0 || tamingPhase) &&
            (bot.level == 0 || bot.level >= kTameMinLevel))
        {
            if (state.TameUntilMs != 0)
            {
                // Holding: the channel runs. A pet means it worked; combat, a refusal, or the hold running out means it did not.
                // Combat does not end the hold while the channel runs: a mob that attacks does not stop a channel, and walking away from the beast would. It ends when the bot is hurt (under half
                // its health) or no longer casting.
                bool const hurtInCombat = bot.in_combat != 0 && (!IsCasting(bot) || std::uint64_t(bot.health) * 2 < bot.max_health);
                auto const refused = state.RetryAtMs.find(kTameBeast);
                bool const refusedNow = refused != state.RetryAtMs.end() && refused->second > state.TameCastAtMs && nowMs >= state.TameCastAtMs + 2 * kGlobalCooldownMs;
                if (PetOf(bot) == PetKnown::kOut)
                {
                    state.TameUntilMs = 0;
                    state.TameFailures = 0;
                    if (_options.OnEvent)
                        _options.OnEvent("tame_done: bot=" + std::to_string(bot.guid) + " target=" + std::to_string(state.TameTarget));
                }
                else if (hurtInCombat || refusedNow || nowMs >= state.TameUntilMs)
                {
                    state.TameUntilMs = 0;
                    ++state.TameFailures;
                    state.TameRetryAtMs = nowMs + (state.TameFailures >= kTameMaxFailures ? kTameGiveUpMs : kTameRetryMs);
                    if (state.TameFailures >= kTameMaxFailures)
                        state.TameFailures = 0;
                    if (_options.OnEvent)
                        _options.OnEvent("tame_failed: bot=" + std::to_string(bot.guid) + " target=" + std::to_string(state.TameTarget) +
                                         (hurtInCombat ? " why=combat" : refusedNow ? " why=refused" : " why=no_pet"));
                }
                else
                    continue; // still channelling: stand still, send nothing
            }
            if (state.TameUntilMs == 0 && !hasTarget && bot.in_combat == 0 && !IsCasting(bot) && !state.Sitting && nowMs >= state.NextCastMs && nowMs >= state.TameRetryAtMs &&
                PetOf(bot) == PetKnown::kNone && !(state.RetryAtMs.count(kTameBeast) && nowMs < state.RetryAtMs[kTameBeast]))
            {
                NearbyUnit const* beast = tame::PickTameTarget(bot, nearby, *_options.TameBeasts, kTameSearchYards);
                if (!beast)
                    state.TameRetryAtMs = nowMs + kTameRetryMs / 2; // nothing to tame in view: look again soon
                else if (rotation::DistanceYards(bot, *beast) > kTameCastYards)
                {
                    if (state.TameTarget != beast->guid || bot.moving == 0)
                    {
                        BotAction walk;
                        walk.bot_guid = bot.guid;
                        walk.type = ActionType::kFollow;
                        walk.target_guid = beast->guid;
                        actions.push_back(walk);
                        state.TameTarget = beast->guid;
                        state.Last = Decision{ ActionType::kFollow, beast->guid };
                    }
                    continue;
                }
                else if (bot.moving != 0)
                {
                    BotAction stop;
                    stop.bot_guid = bot.guid;
                    stop.type = ActionType::kStop;
                    actions.push_back(stop);
                    state.Last = Decision{ ActionType::kStop, 0 };
                    state.Approaching = false;
                    continue;
                }
                else
                {
                    BotAction tameCast;
                    tameCast.bot_guid = bot.guid;
                    tameCast.type = ActionType::kCastSpell;
                    tameCast.spell_id = kTameBeast;
                    tameCast.target_guid = beast->guid;
                    actions.push_back(tameCast);
                    state.CastTarget[kTameBeast] = beast->guid;
                    state.NextCastMs = nowMs + kGlobalCooldownMs;
                    state.TameTarget = beast->guid;
                    state.TameCastAtMs = nowMs;
                    state.TameUntilMs = nowMs + kTameHoldMs;
                    if (_options.OnEvent)
                        _options.OnEvent("tame: bot=" + std::to_string(bot.guid) + " target=" + std::to_string(beast->guid) + " level=" + std::to_string(beast->level) +
                                         " dist=" + std::to_string(int(rotation::DistanceYards(bot, *beast))));
                    continue;
                }
            }
            // In the scenario's Taming phase a hunter with no pet stays where the teleport put it, whatever is or is not in view: a grouped bot would follow its leader back to the
            // staging place (the party forms before the phase), and the scenario would teleport it out again.
            if (tamingPhase && state.TameUntilMs == 0 && !hasTarget && PetOf(bot) == PetKnown::kNone)
            {
                if (bot.moving != 0)
                {
                    BotAction stop;
                    stop.bot_guid = bot.guid;
                    stop.type = ActionType::kStop;
                    actions.push_back(stop);
                    state.Last = Decision{ ActionType::kStop, 0 };
                    state.Approaching = false;
                }
                continue;
            }
        }

        // ==== Loot tracking and fight watchdog ====
        // The target of this fight is looted once it is dead: remember it, and forget the ones too old to still be on the ground.
        if (hasTarget && forever_module::UnitRefType(targetRef) == 8)
            state.FoughtAtMs[targetRef] = nowMs;
        for (auto it = state.FoughtAtMs.begin(); it != state.FoughtAtMs.end();)
            it = nowMs - it->second > kLootWindowMs ? state.FoughtAtMs.erase(it) : std::next(it);
        for (auto it = state.LootedUntilMs.begin(); it != state.LootedUntilMs.end();)
            it = nowMs >= it->second ? state.LootedUntilMs.erase(it) : std::next(it);
        float const distance = targetUnit ? rotation::DistanceYards(bot, *targetUnit) : bot.target_distance;

        // A fight ends a quest walk's stall timer: the quest block below only runs out of combat, so the timer is reset here.
        if (bot.in_combat != 0)
        {
            state.WalkStillSinceMs = 0;
            state.WalkAnchored = false;
        }

        // Fight watchdog: a visible hostile whose health, and the bot's own, have not gone down for kFightStallMs is unreachable,
        // evading or out of line of sight (the 8-hour soak had a fight of two hours). A threat the snapshot keeps set but that is not
        // in view is the same: telemetry holds the fight open until the guid clears, and the nearby list is all the bot can fight.
        // A target the bot is not walking to and is not in combat with (it does not answer) stalls the same way: the dungeon route and
        // its events only run while the bot has no target, so such a target must not hold the party for ever (run 9 held the leader for an hour).
        // A walk to a target is ended by the approach timeout instead. A gap between snapshots does not restart the timer; the stall continued while this tick was late.
        bool const unseenThreat = hasTarget && targetUnit == nullptr;
        bool const visibleStall = hasTarget && targetUnit && targetUnit->is_hostile && (bot.in_combat != 0 || !state.Approaching);
        if (unseenThreat || visibleStall)
        {
            if (state.StallTarget != targetRef)
            {
                state.StallTarget = targetRef;
                state.StallSinceMs = nowMs;
            }
            else if (targetUnit && (targetUnit->health < state.StallTargetHealth || bot.health < state.StallBotHealth))
                state.StallSinceMs = nowMs;
            state.StallLastMs = nowMs;
            if (targetUnit)
            {
                state.StallTargetHealth = targetUnit->health;
                state.StallBotHealth = bot.health;
            }
            if (nowMs - state.StallSinceMs > kFightStallMs)
            {
                AvoidStalledUnit(state.AvoidUntilMs, state.StallCount, targetRef, nowMs);
                if (_options.OnEvent)
                    _options.OnEvent("fight_stalled: bot=" + std::to_string(bot.guid) + " target_entry=" + std::to_string(forever_module::UnitRefEntry(targetRef)) +
                                     " target_hp=" + (targetUnit ? std::to_string(targetUnit->health) : std::string("unseen")) +
                                     " distance=" + std::to_string(int(distance)));
                state.StallTarget = 0;
                // Given up for good this tick too: the bot must not keep attacking it until the next one.
                targetRef = 0;
                targetUnit = nullptr;
                hasTarget = false;
            }
        }
        else
            state.StallTarget = 0;

        Decision wanted;
        char const* moveWhy = ""; // which part of the policy chose a kMoveTo (logged for a party in a dungeon run, move_why)
        int kiteRetry = -1;       // the AoeKitePoint retry that picked a kite move: 0 is the entry retreat (with an entry), more is the circle (move_why)
        // An Onyxian Warder close to a party member (not its leader, the tank): the nearest one in view that is alive, and where a healer may step to see a member it could not heal.
        // Not tied to the bot's own in_combat: the 2026-10-29 pull left the non-tank bots out of combat for 55 s, standing 5 yards from the Warder.
        NearbyUnit const* aoeWarder = nullptr;
        std::optional<Point> healLosPoint;
        std::optional<Point> kiteEntry;   // the entry to retreat toward (AoeKitePoint)
        if (partyMember)
        {
            auto const dungeon = _options.DungeonsByMap.find(bot.map_id);
            if (dungeon != _options.DungeonsByMap.end() && dungeon->second.HasEntry)
                kiteEntry = dungeon->second.Entry;
            else if (bot.map_id == kOnyxiaMap)
                kiteEntry = Point{ kOnyxiaEntryX, kOnyxiaEntryY, kOnyxiaEntryZ };
        }
        if (partyMember)
        {
            float nearestWarder = std::numeric_limits<float>::max();
            for (NearbyUnit const& unit : nearby)
                if (IsLiveMob(unit) && forever_module::UnitRefEntry(unit.guid) == kOnyxianWarderEntry)
                {
                    float const away = std::hypot(bot.pos_x - unit.pos_x, bot.pos_y - unit.pos_y);
                    if (away < nearestWarder)
                    {
                        nearestWarder = away;
                        aoeWarder = &unit;
                    }
                }
            if (bot.in_combat != 0 && nowMs < state.HealLosUntilMs && state.HealLosTarget != 0 && aoeWarder)
                for (BotSnapshot const& other : view.bots)
                    if (PlayerRef(other.guid) == state.HealLosTarget && other.map_id == bot.map_id)
                    {
                        // Toward the member, or to either side of that line, whichever keeps the kite distance from every Warder in view.
                        float tx = other.pos_x - bot.pos_x;
                        float ty = other.pos_y - bot.pos_y;
                        float const length = std::hypot(tx, ty);
                        if (length < 0.1f)
                            break;
                        tx /= length;
                        ty /= length;
                        float const step = float(_options.HealerLosMoveYards);
                        float const tries[3][2] = { { tx, ty }, { -ty, tx }, { ty, -tx } };
                        for (auto const& direction : tries)
                        {
                            Point const dest{ bot.pos_x + direction[0] * step, bot.pos_y + direction[1] * step, other.pos_z };
                            bool clear = true;
                            for (NearbyUnit const& unit : nearby)
                                if (IsLiveMob(unit) && forever_module::UnitRefEntry(unit.guid) == kOnyxianWarderEntry &&
                                    std::hypot(dest.X - unit.pos_x, dest.Y - unit.pos_y) < kAoeStandYards - 0.5f)   // not into the band the kite branch above clears
                                    clear = false;
                            if (clear)
                            {
                                healLosPoint = dest;
                                break;
                            }
                        }
                        break;
                    }
        }

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
        {
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
        else
        {
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
                if (routeActiveOf(bot.guid) && (scenario.RouteMap == 0 || bot.map_id == scenario.RouteMap))
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
            std::set<std::uint32_t> const& killedNow = routeActiveOf(bot.guid) ? killedBossesOf(bot.guid) : _tankKilled[bot.map_id];
            if (routePoints && !routeActiveOf(bot.guid) && routeBoss && !routeBoss->empty())
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
                bool together = true;
                for (std::uint64_t member : membersOf[bot.guid])
                {
                    auto const at = indexOf.find(member);
                    if (at == indexOf.end() || view.bots[at->second].life_state != 0)
                        continue;
                    together = together && (state.RegroupStuck.count(member) != 0 || Distance2d(bot, view.bots[at->second]) <= kRouteWaitYards);
                }
                if (state.TankLead && !routeActiveOf(bot.guid))
                {
                    // The group's player: the tank does not run off without them. A player out of its view counts as far.
                    bool playerNear = false;
                    for (NearbyUnit const& unit : nearby)
                        if (unit.guid == bot.group_leader_guid)
                            playerNear = rotation::DistanceYards(bot, unit) <= kRouteWaitYards;
                    together = together && playerNear;
                }
                together = together && deadWaitKind == 0;
                bool const strong = bot.max_health == 0 ||
                                    std::uint64_t(bot.health) * 100 >= std::uint64_t(bot.max_health) * kRouteMinHealthPct;
                // Mana: a party whose healer (or any caster) is dry walks into the next pack and loses it. The members sit and drink while the leader waits, at most kRouteManaMaxMs.
                bool manaLowInParty = false;
                {
                    auto const dry = [](BotSnapshot const& member)
                    {
                        return member.life_state == 0 && member.power_type == 0 && member.max_power > 0 &&
                               std::uint64_t(member.power) * 100 < std::uint64_t(member.max_power) * kRouteMinManaPct;
                    };
                    manaLowInParty = dry(bot);
                    for (std::uint64_t member : membersOf[bot.guid])
                        if (auto const at = indexOf.find(member); at != indexOf.end())
                            manaLowInParty = manaLowInParty || dry(view.bots[at->second]);
                    if (!manaLowInParty || bot.in_combat != 0)
                        state.RouteManaSinceMs = 0;
                    else if (state.RouteManaSinceMs == 0)
                        state.RouteManaSinceMs = nowMs;
                }
                bool const manaReady = !manaLowInParty || nowMs - state.RouteManaSinceMs >= kRouteManaMaxMs;
                // A waypoint the core finds no route to (a spawn on a ledge or off the navmesh, a generated point inside a wall) would hold the whole run for
                // good: after kRouteNoRouteSkip refusals in a row the leader gives that point up and goes on to the next one.
                // A boss point is never skipped: the boss stands between the party and the rest of the dungeon (a locked door opens when it dies, or it guards
                // the way). A boss that cannot be reached stops the run at that point, which the result reports as the furthest point reached.
                bool recovering = false;   // the leader was just put back on the route: no walking order this tick
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
                if (!recovering && atEntry && nowMs >= state.NextRecoverMs)
                    if (auto const dungeon = _options.DungeonsByMap.find(bot.map_id); dungeon != _options.DungeonsByMap.end())
                        for (std::uint64_t member : membersOf[bot.guid])
                        {
                            auto const at = indexOf.find(member);
                            if (at == indexOf.end())
                                continue;
                            BotSnapshot const& other = view.bots[at->second];
                            if (other.life_state != 0 || other.in_combat != 0 || other.map_id != bot.map_id || DistanceTo(other, dungeon->second.Entry) <= kLostLeaderYards)
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
                // Events (dungeon_events.h): a door to open, a charge to fire. While one runs the leader does only what it asks.
                bool eventBusy = false;
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

        // ==== Idle-time dispatch: grinding, errands, hubs, vendor/service trips ====
        // An enemy near keeps the bot from idling, which keeps it from walking to grind spots. Only one the bot would actually pull counts: a bot
        // whose start zone shows only elites, packs of four or more, or creatures well above its level used to stand there for the whole run
        // (all 73 Undead and many Night Elf and Tauren bots of the 8-hour soak), neither fighting nor leaving. A bot that holds position by
        // order (a party waiting in a dungeon, no self pull) still counts any live creature.
        bool enemyNear = false;
        if (dungeonHold || noSelfPull)
        {
            for (NearbyUnit const& unit : nearby)
                enemyNear = enemyNear || (IsLiveMob(unit) && (huntEntries.empty() ? !IsGrey(bot, unit) : true));
        }
        else
            enemyNear = PullTarget(bot, nearby, avoided, false, huntEntries, routeOn) != nullptr;
        bool idle = !hasTarget && !enemyNear && wanted.Type == ActionType::kStop && questStep.Kind == QuestStepKind::None && !routeBusy;

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
                if (bot.in_combat == 0 && bot.life_state == 0 && nowMs >= state.ErrandTrainSentMs)
                {
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

        // Services and travel (M6). An idle bot due a vendor visit walks to the nearest vendor on its map, unless it is a
        // party member (it follows its leader). The visit is sent once the bot is in reach. A walk longer than
        // kTravelTimeoutMs is dropped and the visit waits for its next interval. A walking bot is not idle, so it neither
        // sits nor trains on the way.
        bool const urgentService = state.ServiceUrgent && !hasTarget && bot.in_combat == 0 && !lowHealth && !state.InFight;

        // Supplies (layout 15, supply_policy.h). A trip made earlier is judged once the core has counted again: more pieces than before is a
        // success; otherwise the vendor had nothing the bot could buy (or the bot never reached it) and the next trip waits longer.
        supply::Facts const supplyFacts = FactsOf(bot);
        if (state.SupplyCheckAtMs != 0 && nowMs >= state.SupplyCheckAtMs)
        {
            state.SupplyCheckAtMs = 0;
            bool const improved = supply::Improved(state.SupplyAsked, state.SupplyBefore, supplyFacts);
            state.SupplyFailures = improved ? 0 : state.SupplyFailures + 1;
            if (!improved)
                state.NextSupplyMs = nowMs + supply::BackoffMs(_options.Supplies, state.SupplyFailures);
            if (_options.OnEvent)
                _options.OnEvent(std::string(improved ? "supply_trip_done" : "supply_trip_failed") + ": bot=" + std::to_string(bot.guid) +
                                 " ammo=" + std::to_string(supplyFacts.AmmoCount) + " food=" + std::to_string(supplyFacts.FoodCount) +
                                 " drink=" + std::to_string(supplyFacts.DrinkCount) + " hpot=" + std::to_string(supplyFacts.HealthPotions) +
                                 " mpot=" + std::to_string(supplyFacts.ManaPotions) + " bandage=" + std::to_string(supplyFacts.Bandages) +
                                 " failures=" + std::to_string(state.SupplyFailures));
        }
        // A party member follows its leader, and a bot held by a chat order stays where it was told: neither goes shopping.
        supply::Needs const supplyNeeds = (partyMember || state.ChatStay || state.ChatFollow != 0) ? supply::Needs{}
                                                                                                    : supply::Assess(_options.Supplies, supplyFacts);
        supply::Stop const supplyStop = supply::NextStop(supplyNeeds);
        bool const supplyDue = supplyStop != supply::Stop::None && nowMs >= state.NextSupplyMs && state.SupplyCheckAtMs == 0;
        bool const serviceDue = _options.ServiceEveryMs != 0 && nowMs >= state.NextServiceMs;
        bool const shopping = (idle || urgentService) && !state.ChatStay && (serviceDue || supplyDue);
        if (!shopping && state.Traveling)
            state.Traveling = false; // a walk to a vendor cut short (a fight, an order) starts its timer again next time

        // Services and travel (M6). An idle bot due a vendor visit walks to the nearest vendor on its map, unless it is a
        // party member (it follows its leader). The visit is sent once the bot is in reach. A walk longer than
        // kTravelTimeoutMs is dropped and the visit waits for its next interval. A walking bot is not idle, so it neither
        // sits nor trains on the way. A bot short of ammunition, food or drink walks to a vendor that sells it instead (supplies).
        if (shopping)
        {
            // The nearest vendor on the bot's map, found by the core (layout 6); the nearest repairman instead when the gear is worn
            // (layout 9). None on the map, or a party member: no walk.
            bool const needRepair = bot.repair_valid != 0 && bot.durability_pct < kRepairBelowPct;
            // Worn gear first: the repairman is the stop then, and the supplies wait for the next trip (a few minutes).
            bool const supplyStopNow = supplyDue && !needRepair;
            bool const ammoStop = supplyStopNow && supplyStop == supply::Stop::Ammo;
            bool const goodsStop = supplyStopNow && supplyStop == supply::Stop::Goods;
            bool const reagentStop = supplyStopNow && supplyStop == supply::Stop::Reagent;
            Point const vendor = supplyStopNow ? (ammoStop      ? Point{ bot.ammo_vendor_x, bot.ammo_vendor_y, bot.ammo_vendor_z }
                                                  : goodsStop   ? Point{ bot.goods_vendor_x, bot.goods_vendor_y, bot.goods_vendor_z }
                                                  : reagentStop ? Point{ bot.reagent_vendor_x, bot.reagent_vendor_y, bot.reagent_vendor_z }
                                                                : Point{ bot.food_vendor_x, bot.food_vendor_y, bot.food_vendor_z })
                                 : needRepair  ? Point{ bot.repair_x, bot.repair_y, bot.repair_z }
                                               : Point{ bot.vendor_x, bot.vendor_y, bot.vendor_z };
            bool const vendorExists = supplyStopNow || needRepair || bot.vendor_valid != 0;
            bool const hasVendor = !partyMember && vendorExists;
            bool const inReach = vendorExists && DistanceTo(bot, vendor) <= kVendorReachYards;
            if (hasVendor && !inReach)
            {
                if (!state.Traveling)
                {
                    state.Traveling = true;
                    state.TravelStartMs = nowMs;
                    state.SupplyTrip = supplyStopNow;
                }
                if (nowMs - state.TravelStartMs <= kTravelTimeoutMs)
                {
                    wanted = Decision{ ActionType::kMoveTo, 0, vendor };
                    moveWhy = "vendor";
                    idle = false;
                }
                else
                {
                    state.Traveling = false;
                    state.ServiceUrgent = false;
                    if (state.SupplyTrip)
                    {
                        // The walk never got there: a trip that bought nothing.
                        state.SupplyTrip = false;
                        state.NextSupplyMs = nowMs + supply::BackoffMs(_options.Supplies, ++state.SupplyFailures);
                    }
                    else
                        state.NextServiceMs = nowMs + _options.ServiceEveryMs;
                }
            }
            else if (inReach || !vendorExists)
            {
                // In reach of a vendor, or none on the map at all: the visit is tried where the bot stands and the core refuses it when
                // no NPC is in range. What is left is a party member with a vendor it may not walk to, and it is sent nothing: it follows
                // its leader, and an action it will have refused still spent a slot of the tick's action budget (kRequestedActions is 2 a
                // bot, and PublishActions drops the whole tick when the list runs over) and pushed the next visit a whole ServiceEveryMs
                // out. At the 1500-bot ceiling that is 1200 refused actions on one tick, over the budget the core granted.
                state.Traveling = false;
                state.SupplyTrip = false;
                if (state.ServiceUrgent)
                {
                    state.ServiceUrgent = false;
                    _quests.NoteServiced(state.Quest); // the bags are emptier: the refused hand-ins are tried again
                }
                BotAction vendor;
                vendor.bot_guid = bot.guid;
                vendor.type = ActionType::kVendor;
                vendor.spell_id = kServiceSell | kServiceRepair | kServiceEquip;
                // Nearly full bags (113 of 300 soak bots had every slot taken by cloth, meat and spare bags and could not turn in a quest): the visit also
                // sells the white junk, not only the grey items.
                if (bot.bag_free_slots <= kBagsLowSlots)
                    vendor.spell_id |= kServiceSellJunk;
                // Layout 13: the NPC's guid, not just its position. An older core leaves it 0 and the core resolves the
                // nearest vendor of the map instead.
                vendor.target_guid = needRepair ? bot.repair_guid : bot.vendor_guid;
                if (supplyStopNow)
                {
                    // Layout 15: buy what is short. The core picks the items from this vendor's stock and buys up to the counts given here.
                    supply::Request const request = supply::RequestFor(_options.Supplies, supplyNeeds);
                    vendor.target_guid = ammoStop ? bot.ammo_vendor_guid : goodsStop ? bot.goods_vendor_guid
                                       : reagentStop ? bot.reagent_vendor_guid : bot.food_vendor_guid;
                    if (request.Ammo != 0)
                        vendor.spell_id |= kServiceBuyAmmo;
                    if (request.Food != 0)
                        vendor.spell_id |= kServiceBuyFood;
                    if (request.Drink != 0)
                        vendor.spell_id |= kServiceBuyDrink;
                    if (request.HealthPotion)
                        vendor.spell_id |= kServiceBuyHealthPotion;
                    if (request.ManaPotion)
                        vendor.spell_id |= kServiceBuyManaPotion;
                    if (request.Bandage)
                        vendor.spell_id |= kServiceBuyBandage;
                    if (request.Reagent)
                        vendor.spell_id |= kServiceBuyReagent;
                    vendor.dest_x = float(request.Ammo);
                    vendor.dest_y = float(request.Food);
                    vendor.dest_z = float(request.Drink);
                    state.NextSupplyMs = nowMs + _options.Supplies.TripEveryMs;
                    state.SupplyCheckAtMs = nowMs + kSupplyJudgeAfterMs;
                    state.SupplyAsked = supplyNeeds;
                    state.SupplyBefore = supplyFacts;
                    if (_options.OnEvent)
                        _options.OnEvent("supply_trip: bot=" + std::to_string(bot.guid) + " level=" + std::to_string(bot.level) + " ammo=" +
                                         std::to_string(supplyFacts.AmmoCount) + " food=" + std::to_string(supplyFacts.FoodCount) + " drink=" +
                                         std::to_string(supplyFacts.DrinkCount) + " hpot=" + std::to_string(supplyFacts.HealthPotions) + " mpot=" +
                                         std::to_string(supplyFacts.ManaPotions) + " bandage=" + std::to_string(supplyFacts.Bandages) +
                                         " copper=" + std::to_string(bot.copper));
                }
                actions.push_back(vendor);
                if (!supplyStopNow || serviceDue)
                    state.NextServiceMs = nowMs + _options.ServiceEveryMs;
            }
        }

        // Hubs (M7): a bot that has been idle for kWanderIdleMs walks to the next hub of its map, and waits there until the
        // next walk. A hurt bot rests instead, and a party member follows its leader. Anything that comes near stops the walk.
        // Mana (the soak's hunter spent most of its time at a third of its mana, because only health made a bot sit): an idle mana user
        // below kRestManaBelowPct sits and drinks until kRestManaUntilPct, at most kManaRestMaxMs at a time (and not again for a while
        // when that was not enough: mana that does not come back is not waited for). Anything else the bot does ends the rest.
        bool const manaUser = bot.power_type == 0 && bot.max_power > 0;
        bool manaResting = false;
        if (idle && manaUser)
        {
            if (state.ManaRestSinceMs == 0 && nowMs >= state.ManaRestBlockedUntilMs &&
                std::uint64_t(bot.power) * 100 < std::uint64_t(bot.max_power) * kRestManaBelowPct)
                state.ManaRestSinceMs = nowMs;
            if (state.ManaRestSinceMs != 0)
            {
                if (std::uint64_t(bot.power) * 100 >= std::uint64_t(bot.max_power) * kRestManaUntilPct)
                    state.ManaRestSinceMs = 0;
                else if (nowMs - state.ManaRestSinceMs > kManaRestMaxMs)
                {
                    state.ManaRestSinceMs = 0;
                    state.ManaRestBlockedUntilMs = nowMs + kManaRestBlockMs;
                }
                else
                    manaResting = true;
            }
        }
        else
            state.ManaRestSinceMs = 0;
        bool const healthy = !manaResting && (bot.max_health == 0 || std::uint64_t(bot.health) * 100 >= std::uint64_t(bot.max_health) * state.RestPct);
        auto const hubs = _options.HubsByMap.find(bot.map_id);
        bool const grindMode = _options.Grind && !_options.Grind->Spots.empty() && bot.level != 0;
        // How long this bot idles before its next walk: kWanderIdleMs varied by twice RandomPct (+-50 % at 25), drawn once per walk, so the bots do not all
        // set off at the same moment.
        auto const wanderIdle = [&]() -> std::uint64_t
        {
            if (_options.RandomPct == 0)
                return kWanderIdleMs;
            if (state.WanderIdleMs == 0)
                state.WanderIdleMs = bot_random::Vary(kWanderIdleMs, std::min<std::uint32_t>(_options.RandomPct * 2, 90),
                                                      bot_random::Draw(_options.RandomSeed, bot.guid, bot_random::kSaltIdle, state.WanderCycles));
            return state.WanderIdleMs;
        };
        // Walking to the anvil, forge or fire a recipe needs (SDK 0.18, after kNeedsFocus): nothing in view and healthy, the bot goes there; arrived, it asks the core again; a walk
        // with no route, or too long, is over and the skill waits half an hour.
        if (state.CraftWalking && _options.Foci && state.CraftWalkIndex < _options.Foci->size() && idle && healthy)
        {
            crafting::FocusPoint const& point = (*_options.Foci)[state.CraftWalkIndex];
            bool const arrived = crafting::InReach(point, bot.map_id, bot.pos_x, bot.pos_y);
            bool const blocked = state.WalkBlockedAtMs != 0 && state.WalkBlockedAtMs >= state.CraftWalkStartMs;
            if (arrived || blocked || nowMs - state.CraftWalkStartMs > kCraftWalkTimeoutMs)
            {
                state.CraftWalking = false;
                state.CraftRetryAtMs[state.CraftWalkLine] = arrived ? nowMs : nowMs + kCraftWalkFailMs;
            }
            else
            {
                wanted = Decision{ ActionType::kMoveTo, 0, Point{ point.X, point.Y, point.Z } };
                moveWhy = "point_walk";
                idle = false;
            }
        }
        else if (idle && healthy && _options.WanderWhenIdle && grindMode && nowMs >= state.GrindRetryAtMs && !partyMember && !stagingHold && !state.ChatStay && state.ChatFollow == 0)
        {
            // Leveling: after a quiet spell the bot walks to a grind spot that suits its level (nearest first, not the ones it just left), and
            // stays to fight what lives there until it sees nothing for a while again.
            if (state.Wandering)
            {
                bool const arrived = DistanceTo(bot, state.GrindTarget) <= kHubReachYards;
                bool const timedOut = nowMs - state.WanderStartMs > kWanderTimeoutMs || (state.WalkBlockedAtMs != 0 && state.WalkBlockedAtMs >= state.WanderStartMs); // no route: the walk ends, not re-sent every tick
                if (arrived || timedOut)
                {
                    state.Wandering = false;
                    state.IdleSinceMs = nowMs;
                }
                else
                {
                    wanted = Decision{ ActionType::kMoveTo, 0, state.GrindTarget };
                    moveWhy = "state.GrindTarget";
                    idle = false;
                }
            }
            else if (nowMs - state.IdleSinceMs >= wanderIdle())
            {
                std::uint8_t const side = _options.SideOf ? _options.SideOf(bot.guid) : std::uint8_t(0);
                std::vector<std::size_t> const spots = GrindSpotsFor(*_options.Grind, bot.map_id, bot.level, side, bot.pos_x, bot.pos_y, state.GrindVisited);
                if (!spots.empty())
                {
                    // Not always the nearest spot: with randomness on the bot takes one of the nearest few, and stands somewhere around it, not on the
                    // spot's exact point, so the bots of a level do not pile up on one place.
                    std::size_t pick = 0;
                    if (_options.RandomPct > 0 && spots.size() > 1)
                        pick = bot_random::Below(bot_random::Draw(_options.RandomSeed, bot.guid, bot_random::kSaltGrindPick, state.WanderCycles),
                                                 std::uint32_t(std::min<std::size_t>(spots.size(), kGrindPickAmong)));
                    GrindSpot const& spot = _options.Grind->Spots[spots[pick]];
                    state.GrindTarget = Point{ spot.X, spot.Y, spot.Z };
                    if (_options.RandomPct > 0)
                    {
                        double const angle = bot_random::Unit(bot_random::Draw(_options.RandomSeed, bot.guid, bot_random::kSaltJitter, std::uint64_t(state.WanderCycles) * 2)) * 6.283185307;
                        double const radius = kStandJitterYards * std::sqrt(bot_random::Unit(bot_random::Draw(_options.RandomSeed, bot.guid, bot_random::kSaltJitter, std::uint64_t(state.WanderCycles) * 2 + 1)));
                        state.GrindTarget.X += float(std::cos(angle) * radius);
                        state.GrindTarget.Y += float(std::sin(angle) * radius);
                    }
                    ++state.WanderCycles;
                    state.WanderIdleMs = 0; // the next walk draws its own idle time
                    state.GrindVisited.push_back(spots[pick]);
                    if (state.GrindVisited.size() > kGrindMemory)
                        state.GrindVisited.erase(state.GrindVisited.begin());
                    state.Wandering = true;
                    state.WanderStartMs = nowMs;
                    wanted = Decision{ ActionType::kMoveTo, 0, state.GrindTarget };
                    moveWhy = "state.GrindTarget";
                    idle = false;
                    if (_options.OnEvent)
                        _options.OnEvent("grind_spot: bot=" + std::to_string(bot.guid) + " level=" + std::to_string(bot.level) + " map=" + std::to_string(bot.map_id) +
                                         " spot=" + std::to_string(spots[pick]) + " levels=" + std::to_string(spot.MinLevel) + "-" + std::to_string(spot.MaxLevel));
                }
                else
                {
                    // Nothing on this map for this level (a start zone the spot file does not cover, such as the new races' island): the bot
                    // explores on foot for a minute, as a bot without grind data does, and then looks again. Without this it stood at its
                    // start for the whole run.
                    state.IdleSinceMs = nowMs;
                    state.GrindRetryAtMs = nowMs + kGrindRetryMs;
                }
            }
        }
        else if (idle && healthy && _options.WanderWhenIdle && !partyMember && !state.ChatStay && state.ChatFollow == 0 && hubs != _options.HubsByMap.end() && !hubs->second.empty() &&
                 state.HubFailures < kHubFailuresMax)
        {
            std::vector<Point> const& list = hubs->second;
            if (state.Wandering)
            {
                Point const& hub = list[state.NextHub % list.size()];
                bool const arrived = DistanceTo(bot, hub) <= kHubReachYards;
                bool const timedOut = nowMs - state.WanderStartMs > kWanderTimeoutMs || (state.WalkBlockedAtMs != 0 && state.WalkBlockedAtMs >= state.WanderStartMs); // no route: the walk ends, not re-sent every tick
                if (arrived || timedOut)
                {
                    // Next time, the next hub. An arrival waits a full idle interval, as before. A timeout does not: a hub in rock or in
                    // another zone must not cost the bot 30 s of standing every time it goes round the list, so it walks on after a short
                    // pause. Walks that end without arriving are counted: after kHubFailuresMax in a row the bot explores on foot instead.
                    state.Wandering = false;
                    state.HubFailures = arrived ? 0 : state.HubFailures + 1;
                    state.NextHub = (state.NextHub + 1) % list.size();
                    state.IdleSinceMs = arrived ? nowMs : nowMs - (kWanderIdleMs - kHubRetryIdleMs);
                }
                else
                {
                    wanted = Decision{ ActionType::kMoveTo, 0, hub };
                    moveWhy = "hub";
                    idle = false;
                }
            }
            else if (nowMs - state.IdleSinceMs >= wanderIdle())
            {
                ++state.WanderCycles;
                state.WanderIdleMs = 0; // the next walk draws its own idle time
                state.Wandering = true;
                state.WanderStartMs = nowMs;
                wanted = Decision{ ActionType::kMoveTo, 0, list[state.NextHub % list.size()] };
                moveWhy = "list[state.NextHub % list.size()]";
                idle = false;
            }
        }

        // Exploring on foot: a bot with no grind spot and no hub it can reach (a map with neither, or hubs it never got to) would stand where it
        // began and never meet a creature, as 45 bots did for eight hours in the soak. After a short quiet it takes a walk of 50 to 100 yards in a
        // turning direction, and after a few walks tries its hubs again.
        else if (idle && healthy && _options.WanderWhenIdle && (!grindMode || nowMs < state.GrindRetryAtMs) && !partyMember && !state.ChatStay && state.ChatFollow == 0)
        {
            if (state.Wandering)
            {
                bool const arrived = DistanceTo(bot, state.ExploreTarget) <= kExploreReachYards;
                bool const timedOut = nowMs - state.WanderStartMs > kExploreTimeoutMs || (state.WalkBlockedAtMs != 0 && state.WalkBlockedAtMs >= state.WanderStartMs);
                if (arrived || timedOut)
                {
                    state.Wandering = false;
                    state.IdleSinceMs = nowMs; // the next walk starts after kExploreIdleMs
                    if (state.ExploreStep % 8 == 0)
                        state.HubFailures = 0; // try the hubs again now and then
                }
                else
                {
                    wanted = Decision{ ActionType::kMoveTo, 0, state.ExploreTarget };
                    moveWhy = "state.ExploreTarget";
                    idle = false;
                }
            }
            else if (nowMs - state.IdleSinceMs >= bot_random::Vary(kExploreIdleMs, std::min<std::uint32_t>(_options.RandomPct * 2, 90),
                                                                   bot_random::Draw(_options.RandomSeed, bot.guid, bot_random::kSaltIdle, 0x100000000ull + state.ExploreStep)))
            {
                float const angle = float((bot.guid * 2654435761ull) % 628) * 0.01f + float(state.ExploreStep) * 2.399963f;
                float radius = 50.0f + 25.0f * float(state.ExploreStep % 3);
                if (_options.RandomPct > 0)
                    radius = float(bot_random::Vary(std::uint64_t(radius), _options.RandomPct,
                                                    bot_random::Draw(_options.RandomSeed, bot.guid, bot_random::kSaltJitter, 0x200000000ull + state.ExploreStep)));
                ++state.ExploreStep;
                state.ExploreTarget = Point{ bot.pos_x + std::cos(angle) * radius, bot.pos_y + std::sin(angle) * radius, bot.pos_z };
                state.Wandering = true;
                state.WanderStartMs = nowMs;
                wanted = Decision{ ActionType::kMoveTo, 0, state.ExploreTarget };
                moveWhy = "state.ExploreTarget";
                idle = false;
            }
        }

        // A bot that stood on the same spot for a whole check interval, not fighting, says why in one event line (stuck_idle). The 73 Undead bots
        // of the soak did this for eight hours and nothing in the logs said what held them; this line shows which condition did.
        if (_options.OnEvent && nowMs - state.DiagAtMs >= kStuckCheckMs)
        {
            float const moved = std::hypot(bot.pos_x - state.DiagX, bot.pos_y - state.DiagY);
            if (state.DiagAtMs != 0 && moved < 5.0f && bot.in_combat == 0 && !state.Sitting)
            {
                std::size_t hostiles = 0;
                for (NearbyUnit const& unit : nearby)
                    if (IsLiveMob(unit))
                        ++hostiles;
                _options.OnEvent("stuck_idle: bot=" + std::to_string(bot.guid) + " level=" + std::to_string(bot.level) + " map=" + std::to_string(bot.map_id) +
                                 " idle=" + std::to_string(idle) + " hasTarget=" + std::to_string(hasTarget) + " enemyNear=" + std::to_string(enemyNear) +
                                 " wanted=" + std::to_string(int(wanted.Type)) + " quest=" + std::to_string(int(questStep.Kind)) + " route=" + std::to_string(routeBusy) +
                                 " healthy=" + std::to_string(healthy) + " mana=" + std::to_string(manaResting) + " grind=" + std::to_string(grindMode) +
                                 " wandering=" + std::to_string(state.Wandering) + " traveling=" + std::to_string(state.Traveling) + " party=" + std::to_string(partyMember) +
                                 " stay=" + std::to_string(state.ChatStay) + " liveHostiles=" + std::to_string(hostiles) + " nearby=" + std::to_string(nearby.size()) +
                                 " idleForS=" + std::to_string(state.Idling ? (nowMs - state.IdleSinceMs) / 1000 : 0) +
                                 " pos=" + std::to_string(int(bot.pos_x)) + "," + std::to_string(int(bot.pos_y)) + "," + std::to_string(int(bot.pos_z)) +
                                 " in_combat=" + std::to_string(bot.in_combat) + " route_index=" + std::to_string(state.RouteIndex) +
                                 " tank_lead=" + std::to_string(state.TankLead) + " event_active=" + std::to_string(state.Events && state.Events->Active()));
            }
            state.DiagAtMs = nowMs;
            state.DiagX = bot.pos_x;
            state.DiagY = bot.pos_y;
        }

        // Rest: sit down while idle and hurt, and stand before anything moves or fights. The stand goes out before the
        // movement below, in the same action set, so the bot never moves sitting.
        // Idle means no fight and no player to follow, with no live hostile anywhere near: a sitting bot is easy prey.
        // Creatures only: a hostile player that is not attacking the bot does not stop it from resting (its threat target
        // already does, through hasTarget).
        bool const hurtEnough = bot.max_health > 0 && std::uint64_t(bot.health) * 100 < std::uint64_t(bot.max_health) * state.RestPct;
        bool const stillHurt = bot.max_health > 0 && bot.health < bot.max_health;
        // Waiting for the party (dungeon): sits to recover health, and mana for a mana user, even with creatures in view.
        bool const holdIdle = dungeonHold && !hasTarget && wanted.Type == ActionType::kStop && !routeBusy;
        // Dungeon drink: any mana user in a dungeon party drinks at kDungeonDrinkBelowPct and drinks up to kDungeonDrinkUpToPct.
        // This overrides the normal rest share so the pull does not go out with anyone OOM. The owner's rule of 2026-10-23:
        // a pull should not start with the healer (or any mana user) OOM.
        bool const dungeonDrink = holdIdle && manaUser && routeActiveOf(bot.guid) &&
                                  std::uint64_t(bot.power) * 100 < std::uint64_t(bot.max_power) * kDungeonDrinkBelowPct;
        bool const manaLow = holdIdle && manaUser &&
                             std::uint64_t(bot.power) * 100 < std::uint64_t(bot.max_power) * (state.Sitting ? kPartyReadySitHealthPct : (_options.TacticRest ? kPartyReadyPowerPct : kPartyReadyPowerOldPct));
        // The Rest tactic: the party waits for every member to reach the pull share (90 %), so a member below it sits to get there (it only sat below its own rest share).
        bool const healthLow = holdIdle && _options.TacticRest && bot.max_health > 0 &&
                               std::uint64_t(bot.health) * 100 < std::uint64_t(bot.max_health) * kPartyReadyHealthPct;
        // A dungeon mana user drinks up to the dungeon drink cap; others use the normal manaLow threshold.
        bool const drinkCap = dungeonDrink ? kDungeonDrinkUpToPct : (state.Sitting ? kPartyReadySitHealthPct : (_options.TacticRest ? kPartyReadyPowerPct : kPartyReadyPowerOldPct));
        bool const manaStillLow = state.Sitting && dungeonDrink &&
                                  std::uint64_t(bot.power) * 100 < std::uint64_t(bot.max_power) * drinkCap;
        bool const sitWanted = (idle || holdIdle) && (state.Sitting ? (stillHurt || manaLow || manaStillLow || manaResting) : (hurtEnough || healthLow || manaLow || manaResting)) &&
                               nowMs >= state.RestRetryAtMs;
        if (sitWanted != state.Sitting)
        {
            BotAction rest;
            rest.bot_guid = bot.guid;
            rest.type = ActionType::kRest;
            rest.spell_id = sitWanted ? 1 : 0;
            actions.push_back(rest);
            state.Sitting = sitWanted;
        }

        // Eat or drink while resting (layout 15): a bot sat down because it is hurt or low on mana uses food or drink it carries, which restores
        // far faster than sitting alone. Not while moving or fighting, and not again until the last piece has done its work.
        if (_options.Supplies.Enabled && state.Sitting && sitWanted && bot.moving == 0 && bot.in_combat == 0 && nowMs >= state.NextConsumeMs)
        {
            std::uint32_t const healthPct = bot.max_health > 0 ? std::uint32_t(std::uint64_t(bot.health) * 100 / bot.max_health) : 100;
            std::uint32_t const manaPct = manaUser && bot.max_power > 0 ? std::uint32_t(std::uint64_t(bot.power) * 100 / bot.max_power) : 100;
            std::uint32_t what = supply::ConsumeNow(_options.Supplies, supplyFacts, healthPct, manaPct);
            if (what == 0)
                what = supply::BandageNow(_options.Supplies, supplyFacts, healthPct); // layout 16: no food to eat, so a bandage
            if (what != 0)
            {
                BotAction eat;
                eat.bot_guid = bot.guid;
                eat.type = ActionType::kConsume;
                eat.spell_id = what;
                actions.push_back(eat);
                state.NextConsumeMs = nowMs + (what == 5 ? _options.Supplies.BandageEveryMs : _options.Supplies.ConsumeEveryMs);
            }
        }

        // Potions (layout 16): a bot in a fight that is badly hurt, or a mana user nearly out of mana, drinks one. It costs no global cooldown and
        // no standing still, so it goes alongside whatever the bot does; the game's shared potion cooldown is why it is not asked for sooner.
        if (_options.Supplies.Enabled && _options.Supplies.Potions && bot.in_combat != 0 && nowMs >= state.NextPotionMs)
        {
            std::uint32_t const healthPct = bot.max_health > 0 ? std::uint32_t(std::uint64_t(bot.health) * 100 / bot.max_health) : 100;
            std::uint32_t const manaPct = manaUser && bot.max_power > 0 ? std::uint32_t(std::uint64_t(bot.power) * 100 / bot.max_power) : 100;
            if (std::uint32_t const which = supply::PotionNow(_options.Supplies, supplyFacts, healthPct, manaPct))
            {
                BotAction potion;
                potion.bot_guid = bot.guid;
                potion.type = ActionType::kConsume;
                potion.spell_id = which;
                actions.push_back(potion);
                state.NextPotionMs = nowMs + _options.Supplies.PotionEveryMs;
            }
        }

        bool const levelUp = bot.level != 0 && bot.level != state.LastTrainLevel;
        // A sitting bot (resting, or drinking) is not available for a trainer check, exactly as a walking bot is not: it stands first.
        if (idle && !state.Sitting && _options.TrainEveryMs != 0 && (levelUp || nowMs >= state.NextTrainMs))
        {
            BotAction train;
            train.bot_guid = bot.guid;
            train.type = ActionType::kTrain;
            actions.push_back(train);
            state.LastTrainLevel = bot.level;
            state.NextTrainMs = nowMs + _options.TrainEveryMs;
        }

        // A player's party invite is accepted (AcceptInvites, from the chat command switch): the player then commands the bot in chat. An invite
        // from one of this module's bots is not a player's: the party code above answers those (a second accept here was refused as "no pending
        // invite", and a leader even tried to accept the party it leads).
        if (_options.AcceptInvites && bot.group_invite_from != 0 && !isOurBot(bot.group_invite_from) && bot.in_combat == 0 &&
            nowMs >= state.InviteAcceptAtMs && forever_module::UnitRefType(bot.group_invite_from) == kPlayerType)
        {
            BotAction accept;
            accept.bot_guid = bot.guid;
            accept.type = ActionType::kGroup;
            accept.spell_id = kGroupAccept;
            actions.push_back(accept);
            state.InviteAcceptAtMs = nowMs + kAcceptInviteRetryMs;
        }

        // Party actions (M8), out of combat. The leader invites each member that is not in its party yet, and invites a
        // member again only after kInviteRetryMs. A member accepts its leader's invite, and leaves a party that is not its
        // leader's. Each is sent once; a refused one is retried after kPartyRetryMs.
        if (_options.PartySize != 0 && bot.in_combat == 0 && nowMs >= state.PartyRetryAtMs)
        {
            if (partyLeaderBot)
            {
                // A leader that sits in another bot's party (old parties come back from the saved game), or holds an invite to one, leaves it first:
                // a member cannot invite, and a pending invite on it would be refused for the others.
                bool const foreign = (bot.group_role != 0 && bot.group_leader_guid != leaderRef && isOurBot(bot.group_leader_guid)) ||
                                     (bot.group_role == 0 && bot.group_invite_from != leaderRef && isOurBot(bot.group_invite_from));
                // (a leader that has just invited holds a pending invite to its own party: that one is not foreign, leaving would drop the party)
                if (foreign)
                {
                    BotAction leave;
                    leave.bot_guid = bot.guid;
                    leave.type = ActionType::kGroup;
                    leave.spell_id = kGroupLeave;
                    actions.push_back(leave);
                    state.PartyRetryAtMs = nowMs + kPartyRetryMs;
                }
                std::vector<std::uint64_t> const none;
                for (std::uint64_t member : foreign ? none : membersOf[bot.guid])
                {
                    auto const at = indexOf.find(member);
                    if (at == indexOf.end())
                        continue; // not online
                    BotSnapshot const& other = view.bots[at->second];
                    if (other.group_leader_guid == leaderRef || other.group_invite_from == leaderRef)
                        continue; // in this party already, or invited and waiting for the answer
                    if ((other.group_role != 0 && other.group_leader_guid != leaderRef) ||
                        (other.group_role == 0 && other.group_invite_from != 0 && other.group_invite_from != leaderRef))
                        continue; // in another party, or invited to one: it leaves or declines on its own turn, then it is invited
                    auto const invited = state.InvitedAtMs.find(member);
                    if (invited != state.InvitedAtMs.end() && nowMs < invited->second + kInviteRetryMs)
                        continue;

                    BotAction invite;
                    invite.bot_guid = bot.guid;
                    invite.type = ActionType::kGroup;
                    invite.spell_id = kGroupInvite;
                    invite.target_guid = PlayerRef(member);
                    actions.push_back(invite);
                    state.InvitedAtMs[member] = nowMs;
                }
                // 40-man raid support (doc 108): this leader's roster is a raid (PolicyOptions::Raid). ConvertToRaid works at
                // any party size (Group.cpp), so it is asked for as soon as this bot leads one, well before the 6th member
                // would otherwise be refused by the 5-member party cap; repeated at kRaidConvertRetryMs since the core gives
                // no snapshot signal of success and the ask is a harmless no-op once already a raid. Each confirmed member
                // (group_leader_guid already this leader's) is then put in the subgroup its position in the roster implies --
                // 5 per subgroup, leader first -- so an 8x5 roster (ScenarioRoster order) lands one tank/healer/dps group per
                // subgroup, the same split a 5-man scenario's roster gives a single party.
                if (_options.Raid)
                {
                    if (!foreign && nowMs >= state.RaidConvertRetryAtMs)
                    {
                        BotAction convert;
                        convert.bot_guid = bot.guid;
                        convert.type = ActionType::kGroup;
                        convert.spell_id = kGroupConvertRaid;
                        actions.push_back(convert);
                        state.RaidConvertRetryAtMs = nowMs + kRaidConvertRetryMs;
                    }
                    std::vector<std::uint64_t> const& roster = membersOf[bot.guid];
                    for (std::size_t m = 0; m < roster.size(); ++m)
                    {
                        std::uint64_t const member = roster[m];
                        auto const at = indexOf.find(member);
                        if (at == indexOf.end())
                            continue; // not online
                        BotSnapshot const& other = view.bots[at->second];
                        if (other.group_leader_guid != leaderRef)
                            continue; // not a confirmed member of this raid yet; assigned once it has joined
                        // The leader itself holds subgroup 0's first slot (not counted in `roster`), so a member's 1-based
                        // position (m + 1) cut by kRaidSubgroupSize gives its subgroup.
                        std::uint8_t const subgroup = static_cast<std::uint8_t>((m + 1) / kRaidSubgroupSize);
                        auto const sentAt = state.RaidAssignRetryAtMs.find(member);
                        auto const sentWhat = state.RaidSubgroupSent.find(member);
                        bool const alreadySent = sentWhat != state.RaidSubgroupSent.end() && sentWhat->second == subgroup &&
                                                  sentAt != state.RaidAssignRetryAtMs.end() && nowMs < sentAt->second;
                        if (alreadySent)
                            continue;
                        BotAction assign;
                        assign.bot_guid = bot.guid;
                        assign.type = ActionType::kGroup;
                        assign.spell_id = kGroupAssignSubgroup;
                        assign.target_guid = PlayerRef(member);
                        assign.dest_x = float(subgroup);
                        actions.push_back(assign);
                        state.RaidSubgroupSent[member] = subgroup;
                        state.RaidAssignRetryAtMs[member] = nowMs + kRaidAssignRetryMs;
                    }
                }
            }
            else if (partyMember)
            {
                BotAction party;
                party.bot_guid = bot.guid;
                party.type = ActionType::kGroup;
                if (bot.group_role == 0 && bot.group_invite_from == leaderRef)
                    party.spell_id = kGroupAccept;
                else if (bot.group_role == 0 && isOurBot(bot.group_invite_from))
                    party.spell_id = kGroupLeave; // declines an invite from another bot leader (an invite from a human is left to the player's own accept)
                else if (bot.group_role != 0 && bot.group_leader_guid != leaderRef)
                    party.spell_id = kGroupLeave;
                if (party.spell_id != 0)
                {
                    actions.push_back(party);
                    state.PartyRetryAtMs = nowMs + kPartyRetryMs;
                }
            }
            else if (partyLeader == 0 && bot.group_role != 0 && isOurBot(bot.group_leader_guid) && !(_options.IsAlt && _options.IsAlt(bot.guid)))
            {
                // Not one of the bots chosen for the parties (PartyPercent), but still in a bot party left from an earlier run: it leaves, and plays on
                // its own. A bot in a human's party, and a player's alt, are not touched.
                BotAction leave;
                leave.bot_guid = bot.guid;
                leave.type = ActionType::kGroup;
                leave.spell_id = kGroupLeave;
                actions.push_back(leave);
                state.PartyRetryAtMs = nowMs + kPartyRetryMs;
            }
        }

        // Loot (M7): a creature the bot fought, now dead and close, once the bot is out of combat. One loot per corpse; the core
        // decides whether the bot may take anything from it.
        if (bot.in_combat == 0 && !lowHealth)
        {
            for (NearbyUnit const& unit : nearby)
            {
                if (unit.health != 0 || forever_module::UnitRefType(unit.guid) != 8 || state.LootedUntilMs.count(unit.guid))
                    continue;
                if (!state.FoughtAtMs.count(unit.guid) || rotation::DistanceYards3d(bot, unit) > kLootRangeYards)
                    continue;
                // A groupmate already took this corpse (the core loots all of it for the group): nothing is left for this bot.
                std::uint64_t const lootGroup = bot.group_leader_guid != 0 ? bot.group_leader_guid : bot.guid;
                if (auto const claim = _lootClaims.find(unit.guid); claim != _lootClaims.end() && nowMs < claim->second.UntilMs)
                {
                    if (claim->second.Group == lootGroup)
                    {
                        state.FoughtAtMs.erase(unit.guid);
                        continue;
                    }
                }
                else if (_lootClaims.size() > 512)
                    for (auto it = _lootClaims.begin(); it != _lootClaims.end();)
                        it = nowMs >= it->second.UntilMs ? _lootClaims.erase(it) : std::next(it);
                _lootClaims[unit.guid] = LootClaim{ lootGroup, nowMs + kLootWindowMs };

                BotAction loot;
                loot.bot_guid = bot.guid;
                loot.type = ActionType::kLoot;
                loot.target_guid = unit.guid;
                actions.push_back(loot);
                state.LootedUntilMs[unit.guid] = nowMs + kLootWindowMs;
                state.FoughtAtMs.erase(unit.guid);
                break; // one corpse per tick
            }
        }

        if (IsResurrectSpell(bot.casting_spell) && bot.cast_remaining_ms != 0 && state.Last)
            wanted = *state.Last; // a resurrect cast is not broken by a new movement order

        // Flight points (layout 17): a bot on a flight takes no walking orders; a long walk of a bot that is on its own is replaced by a walk to the nearest
        // flight master and a flight from it (taxi_policy.h). The core refuses a flight that would not bring the bot closer, and the bot then walks.
        if (_options.Taxi.Enabled && wanted.Type == ActionType::kMoveTo && !partyMember && !partyLeaderBot)
        {
            taxi::Facts facts;
            facts.X = bot.pos_x;
            facts.Y = bot.pos_y;
            facts.InFlight = bot.in_flight != 0;
            facts.InCombat = bot.in_combat != 0;
            facts.Alive = bot.life_state == 0;
            facts.MasterValid = bot.taxi_master_valid != 0;
            facts.MasterX = bot.taxi_master_x;
            facts.MasterY = bot.taxi_master_y;
            switch (taxi::Decide(_options.Taxi, facts, wanted.Dest.X, wanted.Dest.Y, nowMs, state.TaxiBlockedUntilMs, state.TaxiWaitUntilMs))
            {
                case taxi::Move::Walk:
                    break;
                case taxi::Move::WalkToMaster:
                    wanted.Dest = Point{ bot.taxi_master_x, bot.taxi_master_y, bot.taxi_master_z };
                    break;
                case taxi::Move::Fly:
                {
                    BotAction fly;
                    fly.bot_guid = bot.guid;
                    fly.type = ActionType::kTaxi;
                    fly.target_guid = bot.taxi_master_guid;
                    fly.spell_id = bot.map_id;
                    fly.dest_x = wanted.Dest.X;
                    fly.dest_y = wanted.Dest.Y;
                    fly.dest_z = wanted.Dest.Z;
                    actions.push_back(fly);
                    state.TaxiWaitUntilMs = nowMs + _options.Taxi.WaitMs;
                    state.TaxiFlight = taxi::Flight{ nowMs, 0, wanted.Dest.X, wanted.Dest.Y, bot.taxi_master_guid };
                    if (_options.TaxiPlaces)
                        state.TaxiFlight.Place = taxi::PlaceName(*_options.TaxiPlaces, bot.map_id, wanted.Dest.X, wanted.Dest.Y);
                    if (_options.OnEvent)
                        _options.OnEvent(taxi::OrderLine(bot.guid, bot.level, bot.map_id, int(std::hypot(bot.pos_x - wanted.Dest.X, bot.pos_y - wanted.Dest.Y)),
                                                         state.TaxiFlight));
                    [[fallthrough]];
                }
                case taxi::Move::Wait:
                    if (state.Last)
                        wanted = *state.Last; // nothing new is sent while the flight starts or runs
                    else
                        wanted = Decision{};
                    break;
            }
        }
        else if (bot.in_flight != 0 && state.Last)
            wanted = *state.Last; // on a flight with nothing to walk to: the stop order would be a new order too

        // Back to the dungeon: whatever the rest of the policy wanted, a bot of a dungeon party outside the instance walks to the instance
        // door — the ghost's route, and the destination the scenario's rescue teleport uses (which the core refuses in combat, so the walk
        // is also what makes the rescue possible).
        if (returningToDungeon)
        {
            wanted = Decision{ ActionType::kMoveTo, 0, scenario.PortalPoint };
            moveWhy = "return_to_instance";
        }

        // Mount for long travel, dismount when the walk is short, combat starts, or the bot arrives.
        if (bot.in_combat == 0 && bot.life_state == 0 && bot.casting_spell == 0 && nowMs >= state.MountRetryMs)
        {
            if (wanted.Type == ActionType::kMoveTo && !state.Mounted && bot.level >= _options.MountMinLevel)
            {
                float const destDist = DistanceTo(bot, wanted.Dest);
                if (destDist > kMountMinDistanceYards)
                {
                    // Stop first and mount last: the core keeps only the last action's answer, and with the stop after the mount its refusal was never seen.
                    BotAction stop;
                    stop.bot_guid = bot.guid;
                    stop.type = ActionType::kStop;
                    actions.push_back(stop);
                    BotAction mount;
                    mount.bot_guid = bot.guid;
                    mount.type = ActionType::kMount;
                    mount.spell_id = 1; // the core interprets 1 as mount up
                    actions.push_back(mount);
                    state.Mounted = true;
                    state.MountSentUp = true;
                    state.MountRetryMs = nowMs + 5000;
                    state.Last = Decision{ ActionType::kStop, 0 };
                    wanted = Decision{ ActionType::kStop, 0 }; // stop moving while the mount cast completes (the stop is already sent: Last equals it, so no second one follows the mount)
                }
            }
            else if (state.Mounted &&
                     (wanted.Type != ActionType::kMoveTo ||
                      DistanceTo(bot, wanted.Dest) <= kMountDismountDistanceYards))
            {
                BotAction dismount;
                dismount.bot_guid = bot.guid;
                dismount.type = ActionType::kMount;
                dismount.spell_id = 0; // 0 = dismount
                actions.push_back(dismount);
                state.Mounted = false;
                state.MountSentUp = false;
                state.MountRetryMs = nowMs + 5000;
            }
        }

        if (_options.OnEvent && scenario.RouteMap != 0 && partyLeader != 0 && (wanted.Type == ActionType::kMoveTo || (wanted.Type == ActionType::kFollow && partyLeaderBot)) &&
            nowMs - state.MoveWhyLoggedMs >= 5000 && (wanted.Type == ActionType::kFollow || std::hypot(wanted.Dest.X - state.MoveWhyDest.X, wanted.Dest.Y - state.MoveWhyDest.Y) > 8.0f))
        {
            state.MoveWhyLoggedMs = nowMs;
            state.MoveWhyDest = wanted.Dest;
            _options.OnEvent("move_why: bot=" + std::to_string(bot.guid) + " why=" + (*moveWhy ? moveWhy : "other") + " type=" + std::to_string(int(wanted.Type)) + " target=" + std::to_string(wanted.Target) + " to=" + std::to_string(int(wanted.Dest.X)) + "," +
                             std::to_string(int(wanted.Dest.Y)) + "," + std::to_string(int(wanted.Dest.Z)) + " from=" + std::to_string(int(bot.pos_x)) + "," +
                             std::to_string(int(bot.pos_y)) + "," + std::to_string(int(bot.pos_z)) + " leader=" + (partyLeaderBot ? "1" : "0") + " combat=" + std::to_string(bot.in_combat) +
                             " kite_retry=" + std::to_string(kiteRetry) + " kite_entry=" + (kiteEntry ? "1" : "0") +
                             (aoeWarder ? " warder=" + std::to_string(int(aoeWarder->pos_x)) + "," + std::to_string(int(aoeWarder->pos_y)) : std::string(" warder=-")));
        }
        // A move to a place the core found no route to a moment ago is not sent again yet.
        bool const blockedMove = wanted.Type == ActionType::kMoveTo && nowMs < state.NoRouteUntilMs &&
                                 std::hypot(wanted.Dest.X - state.NoRouteDest.X, wanted.Dest.Y - state.NoRouteDest.Y) < kNoRouteSameYards;
        if (!blockedMove && (!state.Last || state.Last->Type != wanted.Type || state.Last->Target != wanted.Target ||
            state.Last->Dest.X != wanted.Dest.X || state.Last->Dest.Y != wanted.Dest.Y || state.Last->Dest.Z != wanted.Dest.Z))
        {
            BotAction action;
            action.bot_guid = bot.guid;
            action.type = wanted.Type;
            action.target_guid = wanted.Target;
            action.spell_id = wanted.Type == ActionType::kTeleport ? wanted.Map : 0;
            action.dest_x = wanted.Dest.X;
            action.dest_y = wanted.Dest.Y;
            action.dest_z = wanted.Dest.Z;
            actions.push_back(action);
            state.Last = wanted;
        }

        // A heal order: the healer casts its heal on the speaker's selected friend, once.
        if (state.ForcedHeal != 0 && nowMs >= state.NextCastMs && !(IsCasting(bot)))
        {
            std::uint32_t const orderedSpell = HealSpellFor(classId, bot.level);
            if (orderedSpell != 0 && bot.moving != 0 && (KnownCastTimeSpell(orderedSpell) || state.CastTimeSpells.count(orderedSpell)))
            {
                BotAction stop;
                stop.bot_guid = bot.guid;
                stop.type = ActionType::kStop;
                actions.push_back(stop);
                state.Last = Decision{ ActionType::kStop, 0 };
                state.Approaching = false;
                continue;
            }
            if (orderedSpell != 0)
            {
                BotAction heal;
                heal.bot_guid = bot.guid;
                heal.type = ActionType::kCastSpell;
                heal.spell_id = orderedSpell;
                heal.target_guid = state.ForcedHeal;
                actions.push_back(heal);
                state.CastTarget[orderedSpell] = heal.target_guid;
                state.NextCastMs = nowMs + kGlobalCooldownMs;
            }
            state.ForcedHeal = 0;
            continue;
        }

        // Healing: a healer class in a party heals the party member (itself included) with the least health, below kHealBelowPct, in range.
        // Dungeon priorities: emergency Flash Heal (< 30%) > shield preemptive (< 50% and took damage) > normal Heal (< 75%) > tank at 90%.
        // Allow healing in a dungeon run even before the party is fully formed (the first tick sends invites).
        if ((partyLeader != 0 || routeActiveOf(bot.guid)) && nowMs >= state.NextCastMs && !(IsCasting(bot)))
        {
            std::uint32_t const healSpell = state.Role_ == BotState::Role::Dps ? 0u : HealSpellFor(classId, bot.level);
            auto const retry = healSpell != 0 ? state.RetryAtMs.find(healSpell) : state.RetryAtMs.end();
            // Payment is checked per spell below, so a cheaper heal is not held back by the one this healer cannot afford yet.
            if (healSpell != 0 && (retry == state.RetryAtMs.end() || nowMs >= retry->second))
            {
                std::uint64_t const healGroup = partyLeader != 0 ? partyLeader : bot.guid;
                if (_healClaims.size() > 64)
                    for (auto it = _healClaims.begin(); it != _healClaims.end();)
                        it = nowMs >= it->second.UntilMs ? _healClaims.erase(it) : std::next(it);
                BotSnapshot const* hurt = nullptr;
                std::uint64_t hurtPct = (routeActiveOf(bot.guid) && _options.TacticHeal) ? _options.DungeonHealBelowPct : kHealBelowPct;
                BotSnapshot const* leaderHurt = nullptr;   // the tank with a target: healed first, whatever its share
                std::uint64_t leaderHurtPct = _options.DungeonTankHealBelowPct;
                BotSnapshot const* leaderTopUp = nullptr;  // the route leader out of a fight, below kRouteTopUpPct
                // Emergency target: anyone under kEmergencyHealBelowPct gets Flash Heal first.
                BotSnapshot const* emergencyHurt = nullptr;
                // Shield target: a member under kShieldBelowPct that took damage recently (health dropped since last tick).
                BotSnapshot const* shieldHurt = nullptr;
                BotSnapshot const* tankMember = nullptr;   // the party leader, who tanks: Renew keeps a heal over time on it, Binding Heal pairs it with the priest
                std::uint64_t tankPct = 100;
                std::uint32_t lowCount = 0;                // members under kPrayerBelowPct (Prayer of Healing)
                auto const ready = [&](std::uint32_t spell)
                {
                    auto const at = state.RetryAtMs.find(spell);
                    return at == state.RetryAtMs.end() || nowMs >= at->second;
                };
                auto const consider = [&](BotSnapshot const& member)
                {
                    if (member.life_state != 0 || member.max_health == 0 || Distance2d(bot, member) > kHealRangeYards)
                        return;
                    // Another healer of this party already has this member: leave it, someone else needs the heal more.
                    if (auto const claim = _healClaims.find(member.guid); claim != _healClaims.end() &&
                        nowMs < claim->second.UntilMs && claim->second.Group == healGroup)
                        return;
                    std::uint64_t const pct = std::uint64_t(member.health) * 100 / member.max_health;
                    if (pct < kPrayerBelowPct)
                        ++lowCount;
                    if (member.guid == partyLeader)
                    {
                        tankMember = &member;
                        tankPct = pct;
                    }
                    // Out of a fight the route leader is healed up: it walks on only at kRouteMinHealthPct, and with creatures in view it cannot sit to get there.
                    // Ragefire Chasm, 2026-10-26: leaders stood between 50 and 79% for half an hour, below the 80% to walk and above the healer's 40%.
                    if (member.guid == partyLeader && member.threat_target_guid == 0 && member.in_combat == 0 && bot.in_combat == 0 && routeActiveOf(bot.guid) &&
                        pct < kRouteTopUpPct && pct >= kEmergencyHealBelowPct)
                    {
                        leaderTopUp = &member;
                        return;
                    }
                    if (member.guid == partyLeader && member.threat_target_guid != 0)
                    {
                        if (pct < leaderHurtPct)
                        {
                            leaderHurtPct = pct;
                            leaderHurt = &member;
                        }
                        return;
                    }
                    if (pct < kEmergencyHealBelowPct)
                    {
                        // Emergency: Flash Heal is instant-ish (1.5 s) and we can afford it.
                        if (!emergencyHurt || pct < std::uint64_t(emergencyHurt->health) * 100 / emergencyHurt->max_health)
                            emergencyHurt = &member;
                    }
                    else
                    {
                        // Power Word: Shield is a priest spell; the other healers have no shield to cast.
                        // Not on a warrior or a druid: the absorb starves a rage user of rage (the guide).
                        // Not while Weakened Soul keeps a new shield off it (kShieldLockMs after the last one was ordered): the cast is refused, and with the shield first in the
                        // priority chain the member below 55 % was never healed (Stockade, 2026-10-28: a mage held at 44 % for over 30 s while the priest ordered 15 shields).
                        auto const shielded = state.BuffUntilMs.find({ kPowerWordShield, member.guid == bot.guid ? 0 : PlayerRef(member.guid) });
                        bool const shieldLocked = shielded != state.BuffUntilMs.end() && nowMs < shielded->second;
                        if (pct < kShieldBelowPct && !shieldLocked && classId == 5 && ClassFor(_options, member.guid) != kWarriorClass_ && ClassFor(_options, member.guid) != kDruidClass_)
                        {
                            // Shield preemptive: check if this member's health dropped since the last tick (took damage).
                            // The module keeps a short history via state.StallBotHealth etc; here we approximate by checking
                            // if the member is in combat (taking hits) or was the recent target of a hostile.
                            bool tookDamage = member.in_combat != 0 || (state.FoughtAtMs.size() && state.FoughtAtMs.count(member.threat_target_guid));
                            if (tookDamage && (!shieldHurt || pct < std::uint64_t(shieldHurt->health) * 100 / shieldHurt->max_health))
                                shieldHurt = &member;
                        }
                        // A member between the emergency and the shield threshold is still a normal heal target when no shield
                        // applies (it has not been hit yet): the old else-if chain left it with no heal at all, which the
                        // 2026-10-23 run showed as a priest at 34 % never receiving a direct heal.
                        if (pct < hurtPct)
                        {
                            hurtPct = pct;
                            hurt = &member;
                        }
                    }
                };
                consider(bot);
                auto const lead = leaderOf.find(bot.guid);
                for (BotSnapshot const& other : view.bots)
                    if (other.guid != bot.guid && leaderOf.count(other.guid) && leaderOf[other.guid] == lead->second)
                        consider(other);
                // Priority: emergency > tank holding aggro > shield > normal
                std::uint32_t spellToCast = 0;
                BotSnapshot const* target = nullptr;
                std::string note;
                bool const priest = classId == 5;
                bool const prayer = priest && lowCount >= kPrayerMinLow && bot.level >= kPrayerOfHealingLevel && ready(kPrayerOfHealing);
                // Renew on the tank: while it fights and the last Renew has run out. The priest sees only its own auras, so the timer is the module's.
                bool const renewDue = priest && tankMember && tankMember->in_combat != 0 && bot.level >= kRenewLevel && ready(kRenew) &&
                                      !(state.BuffUntilMs.count({ kRenew, PlayerRef(tankMember->guid) }) && nowMs < state.BuffUntilMs[{ kRenew, PlayerRef(tankMember->guid) }]);
                if (prayer)
                {
                    spellToCast = kPrayerOfHealing; // a party heal around the priest: most of the party is low
                    target = &bot;
                    hurtPct = std::uint64_t(bot.health) * 100 / std::max<std::uint32_t>(bot.max_health, 1);
                    note = "party";
                }
                else if (emergencyHurt)
                {
                    // Flash Heal is a priest spell; the other healers answer an emergency with their own heal.
                    spellToCast = classId == 5 ? kFlashHeal : healSpell;
                    target = emergencyHurt;
                    note = "emergency";
                }
                else if (leaderHurt)
                {
                    spellToCast = healSpell;
                    target = leaderHurt;
                    hurtPct = leaderHurtPct;
                    note = "tank";
                }
                else if (shieldHurt)
                {
                    spellToCast = kPowerWordShield;
                    target = shieldHurt;
                    hurtPct = std::uint64_t(shieldHurt->health) * 100 / shieldHurt->max_health;
                    note = "shield";
                }
                else if (renewDue)
                {
                    spellToCast = kRenew;
                    target = tankMember;
                    hurtPct = tankPct;
                    note = "renew";
                }
                else if (hurt)
                {
                    spellToCast = healSpell;
                    target = hurt;
                    note = "";
                }
                else if (leaderTopUp)
                {
                    spellToCast = healSpell;
                    target = leaderTopUp;
                    hurtPct = std::uint64_t(leaderTopUp->health) * 100 / leaderTopUp->max_health;
                    note = "top_up";
                }
                // Binding Heal heals the priest and the tank together: when both are hurt and the chosen heal is for the tank.
                if (priest && target && target != &bot && target == tankMember && spellToCast != kPrayerOfHealing && spellToCast != kRenew && bot.level >= kBindingHealLevel &&
                    ready(kBindingHeal) && std::uint64_t(bot.health) * 100 / std::max<std::uint32_t>(bot.max_health, 1) < kBindingBelowPct && tankPct < kBindingBelowPct)
                {
                    spellToCast = kBindingHeal;
                    note = note.empty() ? "binding" : note + " binding";
                }
                if (target)
                {
                    // Check we can pay for the chosen spell, so a cheaper heal is used when the healer is low on mana.
                    auto const spellRetry = state.RetryAtMs.find(spellToCast);
                    if ((spellRetry == state.RetryAtMs.end() || nowMs >= spellRetry->second) && CanPay(_options.SpellCosts.get(), spellToCast, bot))
                    {
                        // If we recently lost LoS to the target and are still moving to regain it, wait for movement instead of retrying the cast.
                        if (state.CloserUntilMs > nowMs)
                        {
                            state.NextCastMs = nowMs + 1000;
                            continue;
                        }
                        if (bot.moving != 0 && (KnownCastTimeSpell(spellToCast) || state.CastTimeSpells.count(spellToCast)))
                        {
                            BotAction stop;
                            stop.bot_guid = bot.guid;
                            stop.type = ActionType::kStop;
                            actions.push_back(stop);
                            state.Last = Decision{ ActionType::kStop, 0 };
                            state.Approaching = false;
                        }
                        else
                        {
                            BotAction heal;
                            heal.bot_guid = bot.guid;
                            heal.type = ActionType::kCastSpell;
                            heal.spell_id = spellToCast;
                            heal.target_guid = target->guid == bot.guid ? 0 : PlayerRef(target->guid);
                            actions.push_back(heal);
                            state.CastTarget[spellToCast] = heal.target_guid;
                            if (spellToCast == kRenew)
                                state.BuffUntilMs[{ kRenew, PlayerRef(target->guid) }] = nowMs + kRenewMs;
                            if (spellToCast == kPowerWordShield)
                                state.BuffUntilMs[{ kPowerWordShield, heal.target_guid }] = nowMs + kShieldLockMs;
                            _healClaims[target->guid] = HealClaim{ healGroup, nowMs + kHealClaimMs };
                            if (_options.OnEvent)
                                _options.OnEvent("heal: healer=" + std::to_string(bot.guid) + " spell=" + std::to_string(spellToCast) +
                                                 " target=" + std::to_string(target->guid) + " target_hp_pct=" + std::to_string(hurtPct) +
                                                 (note.empty() ? "" : " " + note));
                            state.NextCastMs = nowMs + kGlobalCooldownMs;
                        }
                        continue;
                    }
                }
            }

            // Healer DPS window: when nobody needs healing and healer has mana, cast offensive spells.
            // Only for priest (class 5): Smite, Mind Blast, Shadow Word: Pain. Only in a dungeon party,
            // only out of combat or when the party is not taking damage, and only when healer mana > 40%.
            if (partyLeader != 0 && classId == 5 && bot.in_combat == 0 && partyFighting(partyLeader) &&
                (state.LosRepositionTarget != 0 ||
                 std::uint64_t(bot.power) * 100 >= std::uint64_t(bot.max_power) * _options.HealerDpsManaPct))
            {
                if (state.LosRepositionTarget != 0 && _options.OnEvent)
                    _options.OnEvent("healer_dps_los_pending: healer=" + std::to_string(bot.guid) +
                                     " target=" + std::to_string(state.LosRepositionTarget));
                // Check if anyone in party needs healing
                bool anyoneHurt = false;
                forParty(partyLeader, [&](BotSnapshot const& member)
                {
                    if (member.life_state != 0 || member.max_health == 0)
                        return;
                    std::uint64_t const pct = std::uint64_t(member.health) * 100 / member.max_health;
                    if (pct < (member.guid == partyLeader ? 80 : 60))
                        anyoneHurt = true;
                });
                if (!anyoneHurt && nowMs >= state.NextCastMs && !(IsCasting(bot)))
                {
                    // Try Smite first (30 yd range, no cooldown), then SWP (DoT), then Mind Blast (high damage, cooldown, needs LoS).
                    // Smite and SWP are safer when LoS is uncertain.
                    static constexpr std::uint32_t kSmite = 585;
                    static constexpr std::uint32_t kSWP = 589;
                    static constexpr std::uint32_t kMindBlast = 8092;
                    std::uint32_t dpsSpell = 0;
                    // Prefer Smite (always works at 30 yd), then SWP, then Mind Blast (needs clear LoS).
                    // If a previous priest DPS cast lost LoS, reposition to its target before retrying any spell.
                    bool const losBlocked = state.CloserUntilMs > nowMs || state.LosRepositionTarget != 0;
                    if (CanPay(_options.SpellCosts.get(), kSmite, bot) &&
                        (state.RetryAtMs.find(kSmite) == state.RetryAtMs.end() || nowMs >= state.RetryAtMs[kSmite]))
                        dpsSpell = kSmite;
                    else if (CanPay(_options.SpellCosts.get(), kSWP, bot) &&
                             (state.RetryAtMs.find(kSWP) == state.RetryAtMs.end() || nowMs >= state.RetryAtMs[kSWP]))
                        dpsSpell = kSWP;
                    else if (!losBlocked && CanPay(_options.SpellCosts.get(), kMindBlast, bot) &&
                             (state.RetryAtMs.find(kMindBlast) == state.RetryAtMs.end() || nowMs >= state.RetryAtMs[kMindBlast]))
                        dpsSpell = kMindBlast;
                    if (dpsSpell != 0)
                    {
                        // Healer DPS is only on the tank's engaged target; never self-pull a nearby pack.
                        std::uint64_t targetGuid = 0;
                        auto const lead = indexOf.find(partyLeader);
                        if (lead != indexOf.end() && view.bots[lead->second].threat_target_guid != 0)
                        {
                            for (NearbyUnit const& unit : nearby)
                                if (unit.guid == view.bots[lead->second].threat_target_guid && unit.is_hostile && unit.health != 0)
                                {
                                    targetGuid = unit.guid;
                                    break;
                                }
                        }
                        if (targetGuid != 0)
                        {
                            BotAction dps;
                            dps.bot_guid = bot.guid;
                            dps.type = ActionType::kCastSpell;
                            dps.spell_id = dpsSpell;
                            dps.target_guid = targetGuid;
                            actions.push_back(dps);
                            state.CastTarget[dpsSpell] = targetGuid;
                            state.NextCastMs = nowMs + kGlobalCooldownMs;
                            if (_options.OnEvent)
                                _options.OnEvent("healer_dps: healer=" + std::to_string(bot.guid) + " spell=" + std::to_string(dpsSpell) +
                                                 " target=" + std::to_string(forever_module::UnitRefEntry(targetGuid)));
                            continue;
                        }
                    }
                }
            }

            // Resurrect (after the heals): a priest, paladin or shaman out of combat with the party quiet resurrects the dead member in range
            // that it is closest to (healers first, then the tank), once the 10 s cast is affordable. A refusal retries after a few seconds.
            std::uint32_t const rezSpell = ResurrectSpellFor(classId, bot.level);
            bool const manaOk = bot.power_type != 0 || bot.max_power == 0 ||
                                std::uint64_t(bot.power) * 100 >= std::uint64_t(bot.max_power) * kRezManaPct;
            if (rezSpell != 0 && bot.in_combat == 0 && !hasTarget && manaOk && !partyFighting(partyLeader))
            {
                BotSnapshot const* pick = nullptr;
                int pickRank = 99;
                float pickDist = 0.0f;
                forParty(partyLeader, [&](BotSnapshot const& dead)
                {
                    if (dead.life_state == 0 || dead.map_id != bot.map_id || dead.resurrect_from != 0)
                        return;
                    float const dist = Distance2d(bot, dead);
                    if (dist > kRezRangeYards)
                        return;
                    auto const retry = state.RezRetryAtMs.find(PlayerRef(dead.guid));
                    if (retry != state.RezRetryAtMs.end() && nowMs < retry->second)
                        return;
                    // Only the closest eligible rezzer casts on a target (a tie: the lowest guid).
                    bool otherCloser = false;
                    forParty(partyLeader, [&](BotSnapshot const& other)
                    {
                        if (other.guid == bot.guid || !isRezzer(other) || other.in_combat != 0 || other.map_id != bot.map_id)
                            return;
                        float const otherDist = Distance2d(other, dead);
                        if (otherDist <= kRezRangeYards && (otherDist < dist || (otherDist == dist && other.guid < bot.guid)))
                            otherCloser = true;
                    });
                    if (otherCloser)
                        return;
                    std::uint32_t const deadClass = ClassFor(_options, dead.guid);
                    int const rank = HealSpellFor(deadClass, 99) != 0 ? 0 : deadClass == kWarriorClass ? 1 : 2;
                    if (!pick || rank < pickRank || (rank == pickRank && dist < pickDist))
                    {
                        pick = &dead;
                        pickRank = rank;
                        pickDist = dist;
                    }
                });
                if (pick)
                {
                    if (bot.moving != 0)
                    {
                        BotAction stop;
                        stop.bot_guid = bot.guid;
                        stop.type = ActionType::kStop;
                        actions.push_back(stop);
                        state.Last = Decision{ ActionType::kStop, 0 };
                        state.Approaching = false;
                    }
                    else
                    {
                        BotAction rez;
                        rez.bot_guid = bot.guid;
                        rez.type = ActionType::kCastSpell;
                        rez.spell_id = rezSpell;
                        rez.target_guid = PlayerRef(pick->guid);
                        actions.push_back(rez);
                        state.CastTarget[rezSpell] = rez.target_guid;
                        state.NextCastMs = nowMs + kGlobalCooldownMs;
                        if (_options.OnEvent)
                            _options.OnEvent("rez_cast: rezzer=" + std::to_string(bot.guid) + " spell=" + std::to_string(rezSpell) + " target=" +
                                             std::to_string(pick->guid) + " dist=" + std::to_string(int(pickDist)) + " life=" + std::to_string(pick->life_state));
                    }
                    continue;
                }
            }
        }

        // Hunter pet (out of combat, standing, nothing to fight). Layout 13 puts the pet in the snapshot (pet_state 0 none,
        // 1 alive, 2 dead, with pet_health and pet_max_health), so its state is fact rather than what refused casts imply;
        // the inference below is the fallback for a core that does not report pets.
        if (classId == kHunterClass && !hasTarget && bot.in_combat == 0 && bot.moving == 0 && wanted.Type == ActionType::kStop && nowMs >= state.NextCastMs &&
            !(bot.casting_spell != 0 && bot.cast_remaining_ms != 0) && !state.Sitting && nowMs >= state.PetRetryAtMs)
        {
            if (state.PetFought)
            {
                state.PetFought = false;
                state.PetProbe = true;
            }
            PetKnown const known = PetOf(bot);
            bool const mendable = bot.level == 0 || bot.level >= rotation::LearnLevel(kMendPet);
            bool const petHurt = bot.pet_max_health != 0 &&
                                 std::uint64_t(bot.pet_health) * 100 < std::uint64_t(bot.pet_max_health) * kPetMendHealthPct;
            std::uint32_t petSpell = 0;
            if (known == PetKnown::kDead)
                petSpell = kRevivePet;                                    // fact: it is dead
            else if (known == PetKnown::kNone && state.PetMissing)
                petSpell = state.PetDead ? kRevivePet : kCallPet;          // inferred: no pet in the snapshot
            else if (known == PetKnown::kOut && petHurt && mendable)
                petSpell = kMendPet;                                      // fact: it is out and hurt
            else if (state.PetProbe)
            {
                if (mendable)
                    petSpell = kMendPet;
                else if (nowMs >= state.PetProbeAtMs)
                    petSpell = kCallPet;
                else
                    state.PetProbe = false;
            }
            if (petSpell != 0)
            {
                auto const retry = state.RetryAtMs.find(petSpell);
                bool const learned = bot.level == 0 || bot.level >= rotation::LearnLevel(petSpell);
                if (learned && (retry == state.RetryAtMs.end() || nowMs >= retry->second))
                {
                    BotAction pet;
                    pet.bot_guid = bot.guid;
                    pet.type = ActionType::kCastSpell;
                    pet.spell_id = petSpell;
                    pet.target_guid = 0; // the pet is the spell's own target
                    actions.push_back(pet);
                    state.CastTarget[petSpell] = 0;
                    state.NextCastMs = nowMs + kGlobalCooldownMs;
                    if (petSpell == kCallPet && !state.PetMissing)
                    {
                        state.PetProbeAtMs = nowMs + kPetProbeMs;
                        state.PetProbe = false;
                    }
                    continue;
                }
            }
        }

        // Upkeep buffs (rotation file rows 'buff'): out of combat, standing, with nothing to fight. Each is cast on the bot, or on every member of its group
        // (party), when its last cast has run out; a refused cast is tried again after the usual retry.
        if (!hasTarget && bot.in_combat == 0 && bot.moving == 0 && nowMs >= state.NextCastMs && !(IsCasting(bot)) &&
            !state.Sitting)
        {
            bool cast = false;
            // Claims that have run out are dropped; the map is bounded so a long run cannot grow it.
            if (_buffClaims.size() > 256)
                for (auto it = _buffClaims.begin(); it != _buffClaims.end();)
                    it = nowMs >= it->second.UntilMs ? _buffClaims.erase(it) : std::next(it);
            std::uint64_t const buffGroup = bot.group_leader_guid != 0 ? bot.group_leader_guid : bot.guid;
            rotation::Facts const buffFacts = rotation::FactsFor(bot, nearby, nullptr, 0);
            for (rotation::Row const& row : RowsFor(classId, spec))
            {
                if (!row.Buff || (bot.level != 0 && (row.MinLevel > bot.level || (row.MaxLevel != 0 && bot.level > row.MaxLevel))))
                    continue;
                // A buff row may carry a condition: a stance only in a party (in_party), a pet summon only with no pet (no_pet).
                if (row.When.Condition != rotation::Cond::Always && !rotation::Allowed(row.When, buffFacts))
                    continue;
                auto const retry = state.RetryAtMs.find(row.Spell);
                if (retry != state.RetryAtMs.end() && nowMs < retry->second)
                    continue;
                if (!CanPay(_options.SpellCosts.get(), row.Spell, bot))
                    continue; // a warrior has no rage out of combat: Battle Shout waits until it has some, instead of a cast the core refuses
                // A pet summon (no_pet) is due whenever the snapshot shows no pet: the party claim and the buff timer below would hold it off for the
                // spell's duration (an hour), long after the demon died. The cast in progress and the global cooldown stop it being sent twice.
                if (row.When.Condition == rotation::Cond::NoPet)
                {
                    BotAction summon;
                    summon.bot_guid = bot.guid;
                    summon.type = ActionType::kCastSpell;
                    summon.spell_id = row.Spell;
                    summon.target_guid = 0;
                    actions.push_back(summon);
                    state.CastTarget[row.Spell] = 0;
                    state.NextCastMs = nowMs + kGlobalCooldownMs;
                    cast = true;
                    break;
                }
                // The recipients: the bot itself (reference 0), and for a party buff the members of its group that are alive and close.
                std::vector<std::uint64_t> recipients{ 0 };
                if (row.Party)
                    for (BotSnapshot const& other : view.bots)
                        if (other.guid != bot.guid && other.life_state == 0 && other.group_leader_guid != 0 && other.group_leader_guid == bot.group_leader_guid &&
                            Distance2d(bot, other) <= kBuffRangeYards)
                            recipients.push_back(PlayerRef(other.guid));
                for (std::uint64_t recipient : recipients)
                {
                    // The bot's own buff is read from the snapshot's auras (layout 13): a buff another player cast over it,
                    // or one it lost to a death, is now seen as the fact it is. A party member's buff is not in this bot's
                    // snapshot, so its timer and the party claim carry it.
                    if (recipient == 0)
                    {
                        if (AuraRunning(bot, row.Spell))
                            continue;
                    }
                    else
                    {
                        auto const mine = state.BuffUntilMs.find({ row.Spell, recipient });
                        if (mine != state.BuffUntilMs.end() && nowMs < mine->second)
                            continue;
                    }
                    // A party member refreshed this buff moments ago: someone else has it, leave it to them.
                    auto const claimed = _buffClaims.find({ row.Spell, recipient });
                    if (claimed != _buffClaims.end() && nowMs < claimed->second.UntilMs && claimed->second.Group == buffGroup)
                        continue;
                    BotAction buff;
                    buff.bot_guid = bot.guid;
                    buff.type = ActionType::kCastSpell;
                    buff.spell_id = row.Spell;
                    buff.target_guid = recipient;
                    actions.push_back(buff);
                    state.CastTarget[row.Spell] = recipient;
                    state.NextCastMs = nowMs + kGlobalCooldownMs;
                    std::uint64_t const untilMs = nowMs + std::uint64_t(row.DurationSec) * 1000;
                    _buffClaims[{ row.Spell, recipient }] = BuffClaim{ buffGroup, untilMs };
                    // A buff this bot casts on itself is claimed under its own player reference as well. Without that, a party mate
                    // deciding a moment later sees { spell, PlayerRef(this bot) } unclaimed and buffs it a second time: the self claim is
                    // keyed 0, the members' by reference, and the snapshot that would show the buff does not exist yet.
                    if (recipient == 0)
                        _buffClaims[{ row.Spell, PlayerRef(bot.guid) }] = BuffClaim{ buffGroup, untilMs };
                    cast = true;
                    break;
                }
                if (cast)
                    break;
            }
            if (cast)
                continue;
        }

        // Crafting (SDK 0.17): a bot with nothing to fight, standing still, makes bandages from its cloth and cooks its meat. That raises First Aid and Cooking, and uses up
        // what fills its bags. SDK 0.18: and the things of its crafting profession (Blacksmithing, Leatherworking, Alchemy, Tailoring, Engineering, Smelting). The core picks the
        // recipe; when there is nothing to make it answers kRefused and the skill waits (ReadResult); when a recipe needs an anvil, forge or fire it answers kNeedsFocus and the
        // bot walks to one. The lines take turns.
        bool const calm = !hasTarget && bot.in_combat == 0 && bot.moving == 0 && bot.life_state == 0 && nowMs >= state.NextCastMs && !(IsCasting(bot)) && !state.Sitting &&
                          !state.CraftWalking;
        if (_options.Crafting && calm)
        {
            bool crafted = false;
            for (std::size_t turn = 0; turn < kCraftLineCount && !crafted; ++turn)
            {
                std::size_t const index = (state.CraftTurn + turn) % kCraftLineCount;
                if (nowMs < state.CraftRetryAtMs[index] || !HasLine(bot, kCraftLines[index]))
                    continue;
                BotAction craft;
                craft.bot_guid = bot.guid;
                craft.type = ActionType::kSkillUp;
                craft.spell_id = kCraftLines[index];
                actions.push_back(craft);
                state.CraftTurn = std::uint32_t(index + 1);
                state.CraftSent = std::uint32_t(index);
                state.CraftRetryAtMs[index] = nowMs + kSkillUpCastMs; // the answer decides the next try
                state.NextCastMs = nowMs + kGlobalCooldownMs;
                crafted = true;
            }
            if (crafted)
                continue;
        }

        // Gathering (SDK 0.18, off by default): a bot with Mining, Herbalism or Skinning asks the core to gather what is within 30 yards of it: a vein or herb its skill allows, or a
        // corpse it can skin. The core walks the bot there and casts; the next ask takes the loot.
        if (_options.Gathering && calm)
        {
            bool gathered = false;
            for (std::size_t index = 0; index < kGatherLineCount && !gathered; ++index)
            {
                if (nowMs < state.GatherRetryAtMs[index] || !HasLine(bot, kGatherLines[index]))
                    continue;
                BotAction gather;
                gather.bot_guid = bot.guid;
                gather.type = ActionType::kGather;
                gather.spell_id = kGatherLines[index];
                actions.push_back(gather);
                state.GatherSent = std::uint32_t(index);
                state.GatherRetryAtMs[index] = nowMs + kSkillUpCastMs; // the answer decides the next try
                state.NextCastMs = nowMs + kGlobalCooldownMs;
                gathered = true;
            }
            if (gathered)
                continue;
        }

        if (!hasTarget)
        {
            state.AutoShotTarget = 0; // the fight is over: Auto Shot stopped with it
            continue;
        }

        if (_options.TestSpell != 0)
        {
            // Test cast: on the snapshot tick timer, after the attack decision above.
            if (wanted.Type == ActionType::kAttackTarget &&
                (!state.LastTestCastTick || view.tick_number >= *state.LastTestCastTick + _options.CastEveryTicks))
            {
                BotAction cast;
                cast.bot_guid = bot.guid;
                cast.type = ActionType::kCastSpell;
                cast.spell_id = _options.TestSpell;
                cast.target_guid = targetRef;
                actions.push_back(cast);
                state.CastTarget[_options.TestSpell] = targetRef;
                state.LastTestCastTick = view.tick_number;
            }
            continue;
        }

        float const castDistance = targetUnit ? rotation::DistanceYards3d(bot, *targetUnit) : distance;

        // Class rotation: the first row that is off cooldown, in range and allowed by its rule. One cast per global cooldown.
        if (nowMs < state.NextCastMs)
            continue;
        // A cast or channel in progress finishes first: no cast can start meanwhile.
        if (IsCasting(bot))
            continue;
        rotation::Facts facts = rotation::FactsFor(bot, nearby, targetUnit, state.InFight ? nowMs - state.FightStartMs : 0);
        if (state.ComboTarget != targetRef)
        {
            state.ComboTarget = targetRef;
            state.ComboPoints = 0;
        }
        facts.ComboPoints = int32_t(state.ComboPoints);
        state.PetFought = true;
        // Auto Shot runs until the target changes or the bot moves; casting it again would not help (and may toggle it off).
        if (state.AutoShotTarget != 0 && (state.AutoShotTarget != targetRef || bot.moving != 0))
            state.AutoShotTarget = 0;

        // Spells the core reports as cooling down right now.
        std::unordered_set<std::uint32_t> cooling;
        for (forever_module::SpellCooldown const& cooldown : bot.spell_cooldowns)
            if (cooldown.spell_id != 0 && cooldown.remaining_ms != 0)
                cooling.insert(cooldown.spell_id);

        // Hunter: Mend Pet now and then in a long fight, when the pet is believed to be out.
        if (classId == kHunterClass && !state.PetMissing && bot.moving == 0 && nowMs >= state.PetMendAtMs && facts.FightMs >= kPetMendAfterMs &&
            facts.SelfPowerPct >= std::int32_t(kPetMendManaPct) && facts.SelfHpPct >= 50 &&
            (bot.level == 0 || bot.level >= rotation::LearnLevel(kMendPet)) && !cooling.count(kMendPet))
        {
            auto const retry = state.RetryAtMs.find(kMendPet);
            if (retry == state.RetryAtMs.end() || nowMs >= retry->second)
            {
                BotAction mend;
                mend.bot_guid = bot.guid;
                mend.type = ActionType::kCastSpell;
                mend.spell_id = kMendPet;
                mend.target_guid = 0;
                actions.push_back(mend);
                state.CastTarget[kMendPet] = 0;
                state.NextCastMs = nowMs + kGlobalCooldownMs;
                state.PetMendAtMs = nowMs + kPetMendEveryMs;
                continue;
            }
        }

        for (rotation::Row const& row : RowsFor(classId, spec))
        {
            if (row.Buff)
                continue; // upkeep spells are cast out of combat, below
            if (row.Spell == kAutoShot && state.AutoShotTarget == targetRef)
                continue; // already shooting this target
            if (row.OnTarget && row.MaxRange > kMeleeApproachYards + 1.0f && NoAmmo(bot, classId))
                continue; // a ranged attack of a hunter with no ammunition: refused by the core, so only the melee rows are used
            if (row.MaxLevel != 0 && bot.level > row.MaxLevel)
                continue; // a better spell has replaced it
            // Not learned yet: below the level a trainer teaches it (the rotation file's level, else the built-in table). (A level of 0 is unknown.)
            if (bot.level != 0 && (row.MinLevel != 0 ? row.MinLevel : rotation::LearnLevel(row.Spell)) > bot.level)
                continue;
            auto const retry = state.RetryAtMs.find(row.Spell);
            if (retry != state.RetryAtMs.end() && nowMs < retry->second)
                continue;
            if (cooling.count(row.Spell))
                continue;
            if (!CanPay(_options.SpellCosts.get(), row.Spell, bot))
                continue; // not enough mana, rage or energy for it now: the next row, not a refused cast
            if (row.OnTarget && castDistance < row.MinRange)
                continue;
            if (row.OnTarget && row.MaxRange > 0.0f && castDistance > (row.MaxRange > kRangeSlackYards * 2.0f ? row.MaxRange - kRangeSlackYards : row.MaxRange))
                continue;
            if (!rotation::Allowed(row.When, facts))
                continue;
            // Only one member of the party stops a cast: when a party member is already casting an interrupt, this bot uses its next row.
            if (row.When.Condition == rotation::Cond::TargetCasting && interruptTaken(bot, partyLeader))
                continue;
            // The effect is still up (on the target, or on the bot for a self buff): recasting only refreshes it.
            auto const dot = state.Dots.find(row.Spell);
            bool const sameTarget = dot != state.Dots.end() && dot->second.Target == (row.OnTarget ? targetRef : 0);
            if (sameTarget && nowMs < dot->second.UntilMs)
                continue;

            // A spell with a cast time cannot start while the bot moves: stop first (the cast follows on the next tick). The stop is
            // remembered as the bot's last movement order, so a later follow or walk is sent again.
            if (bot.moving != 0 && (KnownCastTimeSpell(row.Spell) || state.CastTimeSpells.count(row.Spell)))
            {
                BotAction stop;
                stop.bot_guid = bot.guid;
                stop.type = ActionType::kStop;
                actions.push_back(stop);
                state.Last = Decision{ ActionType::kStop, 0 };
                state.Approaching = false;
                break;
            }

            BotAction cast;
            cast.bot_guid = bot.guid;
            cast.type = ActionType::kCastSpell;
            cast.spell_id = row.Spell;
            cast.target_guid = row.OnTarget ? targetRef : 0;
            actions.push_back(cast);
            state.CastTarget[row.Spell] = cast.target_guid;
            state.NextCastMs = nowMs + kGlobalCooldownMs;
            break;
        }
    }

    for (auto it = _bots.begin(); it != _bots.end();)
        it = present.count(it->first) ? std::next(it) : _bots.erase(it);

    return actions;
}
