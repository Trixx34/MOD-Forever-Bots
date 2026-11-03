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

BusyKind BotPolicy::BusyOf(BotSnapshot const& bot) const
{
    auto const found = _bots.find(bot.guid);
    bool const travelling = found != _bots.end() && (found->second.Traveling || found->second.SupplyTrip);
    bool const inDungeon = _options.DungeonsByMap.count(bot.map_id) != 0;
    return BusyReason(bot.in_combat != 0, bot.life_state != 0, inDungeon, travelling);
}

BotActivity BotPolicy::ActivityOf(BotSnapshot const& bot) const
{
    if (bot.in_combat != 0)
        return BotActivity::Fighting;
    if (_options.DungeonsByMap.count(bot.map_id) != 0)
        return BotActivity::Dungeon;
    auto const found = _bots.find(bot.guid);
    if (found == _bots.end())
        return BotActivity::Grinding;
    BotState const& state = found->second;
    if (state.ChatFollow != 0)
        return BotActivity::Following;
    if (state.ChatStay)
        return BotActivity::Holding;
    if (state.Traveling || state.SupplyTrip)
        return BotActivity::Travelling;
    return BotActivity::Grinding;
}

std::vector<BotPolicy::WhisperReply> BotPolicy::ApplyWhisper(WorldTickSnapshotView const& view, std::uint64_t player, std::uint64_t bot,
                                                             std::string const& text, std::uint64_t nowMs)
{
    if (!_options.WhisperReplies || _bots.count(player) != 0) // our own bots never get an answer (nor is a bot's whisper to a bot answered)
        return {};
    BotSnapshot const* snapshot = nullptr;
    for (BotSnapshot const& b : view.bots)
        if (b.guid == bot)
        {
            snapshot = &b;
            break;
        }
    if (snapshot == nullptr)
        return {};
    WhisperIntent const intent = ClassifyWhisper(text);
    if (intent == WhisperIntent::Other || !_whispers.Allow(player, bot, nowMs))
        return {};
    BusyKind const busy = BusyOf(*snapshot);
    std::string line = intent == WhisperIntent::NeedGroup && busy != BusyKind::None ? BusyLine(bot, busy) : ReplyLine(intent, bot, ActivityOf(*snapshot));
    return { WhisperReply{ bot, player, ClampReply(std::move(line)) } };
}

std::vector<BotPolicy::WhisperReply> BotPolicy::TakeWhisperReplies()
{
    std::vector<WhisperReply> out;
    out.swap(_whisperReplies);
    return out;
}

std::vector<std::uint64_t> BotPolicy::BusyBots(WorldTickSnapshotView const& view) const
{
    std::vector<std::uint64_t> busy;
    if (!_options.WhisperReplies)
        return busy;
    for (BotSnapshot const& bot : view.bots)
        if (BusyOf(bot) != BusyKind::None)
            busy.push_back(bot.guid);
    return busy;
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

std::vector<BotPolicy::TalentOrder> BotPolicy::TakeRespecOrders()
{
    std::vector<TalentOrder> orders;
    orders.swap(_respecOrders);
    return orders;
}

std::vector<social::Order> BotPolicy::TakeSocialOrders()
{
    std::vector<social::Order> orders;
    orders.swap(_socialOrders);
    return orders;
}

void BotPolicy::NoteDuelChallenge(std::uint64_t bot, std::uint64_t player)
{
    if (_options.Duels.Enabled)
        StateOf(bot).DuelChallenger = player;
}

void BotPolicy::NoteGiftRecipient(std::uint64_t bot, std::uint64_t player)
{
    if (_options.MailGifts.Enabled)
        StateOf(bot).GiftRecipient = player;
}

void BotPolicy::NoteGuildLeaveRequest(std::uint64_t bot)
{
    if (_options.GuildJoin.Enabled)
        StateOf(bot).GuildLeaveRequested = true;
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
    route.Summons = options.Summons;
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
    // Parties (M8): the lowest-guid bots, in guid order, cut into parties of PartySize. Computed once per tick.
    std::unordered_map<std::uint64_t, std::size_t> indexOf; // bot guid -> index in view.bots
    for (std::size_t i = 0; i < view.bots.size(); ++i)
        indexOf[view.bots[i].guid] = i;

    std::unordered_map<std::uint64_t, std::uint64_t> leaderOf;                // bot guid -> its party's leader
    std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> membersOf;  // leader guid -> the other members
    BuildParties(view, indexOf, leaderOf, membersOf);

    // Level dungeons and LFG: sends parties into their dungeon and back, and starts the tank lead once they are in (it sets TankLead, which the loop below reads).
    if (_options.LevelDungeons || !_lfgOrders.empty())
        DriveLevelDungeons(view, indexOf, membersOf, nowMs, actions);

    // LFG fill: free bots are offered to human parties that lack a tank or a healer (whispers come from TakeWhispers).
    if (_options.LfgFill)
        DriveLfgFill(view, indexOf, nowMs, actions);

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

    DecideTick tick{ view, nowMs, actions, present, indexOf, leaderOf, membersOf, tankLed };

    // ==== Per-bot decision loop ====
    // Order within one bot's turn: read its last action's result, handle ground-effect avoidance and line-of-sight
    // recovery (both can `continue` before any other logic runs), then dead-bot handling, then (while alive) party
    // role/routing/follow, targeting and pulling, crowd control, pet taming, loot/fight-watchdog bookkeeping, and the
    // economy/social actions (crafting, vendor, chat orders) further below.
    for (std::size_t i = 0; i < view.bots.size(); ++i)
        DecideBot(tick, i);

    for (auto it = _bots.begin(); it != _bots.end();)
        it = present.count(it->first) ? std::next(it) : _bots.erase(it);

    return actions;
}

// One bot's turn: the steps below, in this order. A step that answers TurnFlow::kSkip ends the turn (it was a `continue` of Decide's loop).
void BotPolicy::DecideBot(DecideTick& tick, std::size_t i)
{
    BotSnapshot const& bot = tick.view.bots[i];
    tick.present.insert(bot.guid);
    BotTurn turn{ i, bot, StateOf(bot.guid), tick.nowMs };
    turn.actionsAtStart = tick.actions.size();
    if (TurnBegin(tick, turn) == TurnFlow::kSkip)
        return;
    if (TurnGroundEffects(tick, turn) == TurnFlow::kSkip)
        return;
    TurnTargetMemory(tick, turn);
    if (TurnDead(tick, turn) == TurnFlow::kSkip)
        return;
    TurnRevivedAndRoles(tick, turn);
    TurnTalentsAndGear(tick, turn);
    TurnQuestsAndHold(tick, turn);
    TurnTargeting(tick, turn);
    if (TurnCrowdControl(tick, turn) == TurnFlow::kSkip)
        return;
    if (TurnTaming(tick, turn) == TurnFlow::kSkip)
        return;
    TurnFightWatch(tick, turn);
    TurnMoveSetup(tick, turn);
    TurnMoveChoice(tick, turn);
    TurnIdle(tick, turn);
    TurnShopping(tick, turn);
    TurnWander(tick, turn);
    TurnRestAndConsume(tick, turn);
    TurnTrainAndParty(tick, turn);
    TurnSocial(tick, turn);
    TurnLootTaxiMount(tick, turn);
    TurnSendMove(tick, turn);
    if (TurnForcedHeal(tick, turn) == TurnFlow::kSkip)
        return;
    if (TurnPartySupport(tick, turn) == TurnFlow::kSkip)
        return;
    if (TurnHunterPet(tick, turn) == TurnFlow::kSkip)
        return;
    TurnUpkeepBuffs(tick, turn);
    TurnCraftGather(tick, turn);
    if (TurnRotation(tick, turn) == TurnFlow::kSkip)
        return;
}

bool BotPolicy::DecideTick::IsOurBot(std::uint64_t ref) const
{
    return ref != 0 && indexOf.count(ref & forever_module::kUnitRefCounterMask) != 0;
}

bool BotPolicy::BotTurn::Avoided(std::uint64_t ref) const
{
    auto const until = state.AvoidUntilMs.find(ref);
    return until != state.AvoidUntilMs.end() && nowMs < until->second;
}

std::function<bool(std::uint64_t)> BotPolicy::BotTurn::AvoidedFn() const
{
    return [this](std::uint64_t ref) { return Avoided(ref); };
}

// The route is on per bot when parallel parties are run (each has its own director), else for all.
bool BotPolicy::IsRouteActive(std::uint64_t guid) const
{
    return _routeActiveFn ? _routeActiveFn(guid) : _routeActive;
}

std::set<std::uint32_t> const& BotPolicy::KilledBossesOf(std::uint64_t guid) const
{
    return _killedBossesFn ? _killedBossesFn(guid) : _killedBosses;
}

bool BotPolicy::IsRezzer(BotSnapshot const& member) const
{
    return member.life_state == 0 && ResurrectSpellFor(ClassFor(_options, member.guid), member.level) != 0;
}

bool BotPolicy::PartyFighting(DecideTick const& tick, std::uint64_t leaderGuid) const
{
    bool fighting = false;
    tick.ForParty(leaderGuid, [&](BotSnapshot const& member) { fighting = fighting || (member.life_state == 0 && member.in_combat != 0); });
    return fighting;
}

// The distinct live hostiles this party's members are already fighting (their own threat targets, deduped). kMaxDungeonEngaged caps how far
// the pack-focus widening below may reach for a hostile nobody has engaged yet (see kMaxDungeonEngaged).
std::set<std::uint64_t> BotPolicy::PartyEngagedTargets(DecideTick const& tick, std::uint64_t leaderGuid) const
{
    std::set<std::uint64_t> targets;
    tick.ForParty(leaderGuid, [&](BotSnapshot const& member)
    {
        if (member.life_state == 0 && member.in_combat != 0 && member.threat_target_guid != 0)
            targets.insert(member.threat_target_guid);
    });
    return targets;
}

// Whether another member of this bot's party is already stopping a cast. The snapshot says who casts what, and a member with
// casting_spell set is mid-cast (cast_remaining_ms is 0 for a spell in flight, which the core still counts as casting).
// Only the party is looked at, so the cost does not grow with the bot count.
bool BotPolicy::InterruptTaken(DecideTick const& tick, BotSnapshot const& bot, std::uint64_t leaderGuid) const
{
    if (leaderGuid == 0)
        return false; // not in a party of this module's: nobody to coordinate with
    bool taken = false;
    tick.ForParty(leaderGuid, [&](BotSnapshot const& member)
    {
        if (member.guid == bot.guid || member.life_state != 0 || member.casting_spell == 0)
            return;
        if (IsInterruptSpell(member.casting_spell))
            taken = true;
    });
    return taken;
}

// Whether the party waits for a dead member: 0 no, 1 for a resurrect, 2 for a ghost on its way back. Never in a fight, never
// for a ghost whose run failed, and every wait is capped.
std::uint8_t BotPolicy::DeadWait(DecideTick const& tick, std::uint64_t leaderGuid) const
{
    std::uint64_t const nowMs = tick.nowMs;
    if (PartyFighting(tick, leaderGuid))
        return 0;
    bool rezzer = false;
    tick.ForParty(leaderGuid, [&](BotSnapshot const& member) { rezzer = rezzer || IsRezzer(member); });
    std::uint8_t kind = 0;
    tick.ForParty(leaderGuid, [&](BotSnapshot const& member)
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
}

// How long this bot idles before its next walk: kWanderIdleMs varied by twice RandomPct (+-50 % at 25), drawn once per walk, so the bots do not all
// set off at the same moment.
std::uint64_t BotPolicy::WanderIdleFor(BotTurn& turn) const
{
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    if (_options.RandomPct == 0)
        return kWanderIdleMs;
    if (state.WanderIdleMs == 0)
        state.WanderIdleMs = bot_random::Vary(kWanderIdleMs, std::min<std::uint32_t>(_options.RandomPct * 2, 90),
                                              bot_random::Draw(_options.RandomSeed, bot.guid, bot_random::kSaltIdle, state.WanderCycles));
    return state.WanderIdleMs;
}
