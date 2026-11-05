// SPDX-License-Identifier: GPL-2.0-or-later
#include "bot_policy_internal.h"

std::vector<std::size_t> BotPolicy::GrindSpotsForBot(BotState& state, BotSnapshot const& bot, std::uint64_t nowMs)
{
    std::uint8_t const side = _options.SideOf ? _options.SideOf(bot.guid) : std::uint8_t(0);
    std::vector<std::size_t> const blocked = _grindBlocks.Blocked(nowMs);
    std::vector<std::size_t> spots = GrindSpotsFor(*_options.Grind, bot.map_id, bot.level, side, bot.pos_x, bot.pos_y, state.GrindVisited, blocked);
    // A block keeps a spot out only while another one is left for the level: with none left, the blocked spots are used again rather than leaving the bot idle.
    if (spots.empty() && !blocked.empty())
    {
        spots = GrindSpotsFor(*_options.Grind, bot.map_id, bot.level, side, bot.pos_x, bot.pos_y, state.GrindVisited, {});
        if (!spots.empty() && _options.OnEvent)
            _options.OnEvent("grind_blocks_lifted: bot=" + std::to_string(bot.guid) + " level=" + std::to_string(bot.level) + " map=" + std::to_string(bot.map_id) +
                             " blocked=" + std::to_string(blocked.size()));
    }
    if (spots.empty() && _options.OnEvent)
        _options.OnEvent("grind_none: bot=" + std::to_string(bot.guid) + " level=" + std::to_string(bot.level) + " map=" + std::to_string(bot.map_id) +
                         " blocked=" + std::to_string(blocked.size()));
    return spots;
}

std::size_t BotPolicy::BeginTankLead(BotState& state, BotSnapshot const& bot, PolicyOptions::DungeonRoute const& dungeon)
{
    state.TankLead = true;
    state.Events.reset();
    _tankKilled[bot.map_id].clear();
    // On from the point nearest to where the tank stands, not from the entrance: it can be told in the middle of a dungeon.
    std::size_t nearest = 0;
    float nearestDistance = 1e30f;
    for (std::size_t i = 0; i < dungeon.Route.size(); ++i)
    {
        float const distance = DistanceTo(bot, dungeon.Route[i]);
        if (distance < nearestDistance)
        {
            nearestDistance = distance;
            nearest = i;
        }
    }
    state.RouteIndex = nearest;
    state.RouteNoRoute = 0;
    state.RouteBossSinceMs = 0;
    state.Last.reset();
    return nearest;
}

namespace {

constexpr std::uint32_t kLevelDungeonMinMembers = 3;      // a bot party smaller than this does not go into a dungeon on its own
constexpr std::uint64_t kLevelHoldMs = 4000;              // after a teleport order a bot takes no other order for this long
constexpr std::uint64_t kLevelEnterRetryMs = 8000;        // the teleport into the dungeon is sent again to a bot that is not there yet
constexpr std::uint64_t kLevelEnterTimeoutMs = 40000;     // the party is not in by then: the attempt is given up (and counted)
constexpr std::uint64_t kLevelRunMaxMs = 45 * 60 * 1000;  // a run that is not over by then is ended (an attempt that did not clear it)
constexpr std::uint64_t kLfgJoinWaitMs = 60000;           // LFG: how long the speaker has to invite the bots that were brought
constexpr std::uint64_t kLfgOrderMaxMs = 10 * 60 * 1000;  // LFG: an order nobody finished is dropped after this

// The dungeons of the options that a party can run (a route and an entry), for level_dungeon.h, with the map each is on.
struct KnownDungeons
{
    std::vector<level_dungeon::Dungeon> List;
    std::vector<PolicyOptions::DungeonRoute const*> Routes;
};

KnownDungeons KnownDungeonsOf(PolicyOptions const& options)
{
    KnownDungeons known;
    for (auto const& [map, route] : options.DungeonsByMap)
    {
        if (route.Id.empty() || route.Route.empty() || !route.HasEntry)
            continue;
        known.List.push_back(level_dungeon::Dungeon{ route.Id, route.Name, route.MinLevel, route.MaxLevel, map });
        known.Routes.push_back(&route);
    }
    return known;
}

} // namespace

void BotPolicy::DriveLevelDungeons(WorldTickSnapshotView const& view, std::unordered_map<std::uint64_t, std::size_t> const& indexOf,
                                   std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> const& membersOf, std::uint64_t nowMs,
                                   std::vector<BotAction>& actions)
{
    auto const event = [&](std::string const& text)
    {
        if (_options.OnEvent)
            _options.OnEvent(text);
    };
    auto const teleport = [&](BotSnapshot const& bot, std::uint32_t map, Point const& to)
    {
        BotAction action;
        action.bot_guid = bot.guid;
        action.type = ActionType::kTeleport;
        action.spell_id = map;
        action.dest_x = to.X;
        action.dest_y = to.Y;
        action.dest_z = to.Z;
        actions.push_back(action);
        BotState& state = StateOf(bot.guid);
        state.Last.reset();
        state.GotoActive = false;
        state.LevelHoldUntilMs = nowMs + kLevelHoldMs;
    };
    auto const snapshotOf = [&](std::uint64_t guid) -> BotSnapshot const*
    {
        auto const at = indexOf.find(guid);
        return at == indexOf.end() ? nullptr : &view.bots[at->second];
    };

    // ---- Level dungeons: one run per bot party ----
    if (_options.LevelDungeons)
    {
        KnownDungeons const known = KnownDungeonsOf(_options);
        for (auto const& partyEntry : membersOf)
        {
            std::uint64_t const leaderGuid = partyEntry.first;
            std::vector<std::uint64_t> const& members = partyEntry.second;
            BotSnapshot const* const leader = snapshotOf(leaderGuid);
            if (!leader)
                continue;
            auto run = _levelRuns.find(leaderGuid);
            if (run == _levelRuns.end())
            {
                // Not in a run: due? Never in combat, never while a member is dead or away, never inside a dungeon, never while the tank leads on a chat order.
                if (known.List.empty() || members.empty() || members.size() + 1 < kLevelDungeonMinMembers || leader->life_state != 0 || leader->in_combat != 0 ||
                    leader->group_role != 2 || _options.DungeonsByMap.count(leader->map_id) != 0 || StateOf(leaderGuid).TankLead)
                    continue;
                std::uint32_t level = leader->level;
                bool ready = level != 0;
                std::vector<BotSnapshot const*> party{ leader };
                for (std::uint64_t member : members)
                {
                    BotSnapshot const* const other = snapshotOf(member);
                    if (!other || other->life_state != 0 || other->in_combat != 0 || other->in_flight != 0 || other->level == 0 ||
                        other->group_leader_guid != PlayerRef(leaderGuid) || _options.DungeonsByMap.count(other->map_id) != 0)
                    {
                        ready = false;
                        break;
                    }
                    level = std::min(level, other->level);
                    party.push_back(other);
                }
                if (!ready)
                    continue;
                level_dungeon::Book& book = _levelBooks[leaderGuid];
                int const due = level_dungeon::Due(known.List, level, book, _options.LevelDungeonRuns);
                if (due < 0)
                    continue;
                PolicyOptions::DungeonRoute const& dungeon = *known.Routes[std::size_t(due)];
                level_dungeon::NoteStarted(book, dungeon.Id, level);
                LevelDungeonRun started;
                started.Map = known.List[std::size_t(due)].MapId;
                started.Id = dungeon.Id;
                started.StartMs = nowMs;
                started.RetryMs = nowMs + kLevelEnterRetryMs;
                started.OriginMap = leader->map_id;
                started.Origin = Point{ leader->pos_x, leader->pos_y, leader->pos_z };
                started.PartyLevel = level;
                for (BotSnapshot const* member : party)
                    teleport(*member, started.Map, dungeon.Entry);
                _levelRuns[leaderGuid] = started;
                event("level_dungeon_start: leader=" + std::to_string(leaderGuid) + " dungeon=" + dungeon.Id + " level=" + std::to_string(level) +
                      " run=" + std::to_string(book.Dungeons[dungeon.Id].Runs) + " members=" + std::to_string(party.size()));
                continue;
            }

            // In a run.
            LevelDungeonRun& current = run->second;
            auto const route = _options.DungeonsByMap.find(current.Map);
            BotState& leaderState = StateOf(leaderGuid);
            auto const sendHome = [&]()
            {
                if (leader->life_state == 0 && leader->map_id == current.Map)
                    teleport(*leader, current.OriginMap, current.Origin);
                for (std::uint64_t member : members)
                    if (BotSnapshot const* other = snapshotOf(member); other && other->life_state == 0 && other->map_id == current.Map)
                        teleport(*other, current.OriginMap, current.Origin);
            };
            if (route == _options.DungeonsByMap.end())
            {
                leaderState.TankLead = false;
                leaderState.LevelRun = false;
                sendHome();
                event("level_dungeon_end: leader=" + std::to_string(leaderGuid) + " dungeon=" + current.Id + " result=route_gone");
                _levelRuns.erase(run);
                continue;
            }
            if (current.Phase_ == LevelDungeonRun::Phase::Entering)
            {
                if (leader->map_id == current.Map && leader->life_state == 0)
                {
                    leaderState.ChatFollow = 0;
                    leaderState.ChatStay = false;
                    leaderState.LevelRun = true;
                    std::size_t const nearest = BeginTankLead(leaderState, *leader, route->second);
                    current.Phase_ = LevelDungeonRun::Phase::Running;
                    current.StartMs = nowMs;
                    event("level_dungeon_in: leader=" + std::to_string(leaderGuid) + " dungeon=" + current.Id + " point=" + std::to_string(nearest + 1) + "/" +
                          std::to_string(route->second.Route.size()));
                }
                else if (nowMs - current.StartMs > kLevelEnterTimeoutMs)
                {
                    sendHome();
                    event("level_dungeon_end: leader=" + std::to_string(leaderGuid) + " dungeon=" + current.Id + " result=not_entered");
                    _levelRuns.erase(run);
                }
                else if (nowMs >= current.RetryMs)
                {
                    current.RetryMs = nowMs + kLevelEnterRetryMs;
                    if (leader->map_id != current.Map && leader->life_state == 0)
                        teleport(*leader, current.Map, route->second.Entry);
                    for (std::uint64_t member : members)
                        if (BotSnapshot const* other = snapshotOf(member); other && other->life_state == 0 && other->map_id != current.Map)
                            teleport(*other, current.Map, route->second.Entry);
                }
                continue;
            }
            // Running: over when the route is walked (cleared), the lead was ended (stop, lead off, left the map) or the time is up.
            bool const cleared = leaderState.TankLead && leaderState.RouteIndex >= route->second.Route.size();
            bool const ended = !leaderState.TankLead || leader->map_id != current.Map;
            bool const late = nowMs - current.StartMs > kLevelRunMaxMs;
            if (cleared || ended || late)
            {
                if (cleared)
                    level_dungeon::NoteCleared(_levelBooks[leaderGuid], current.Id);
                leaderState.TankLead = false;
                leaderState.LevelRun = false;
                leaderState.Events.reset();
                sendHome();
                event("level_dungeon_end: leader=" + std::to_string(leaderGuid) + " dungeon=" + current.Id + " result=" + (cleared ? "cleared" : (late ? "timeout" : "ended")) +
                      " level=" + std::to_string(leader->level));
                _levelRuns.erase(run);
            }
        }
        // A run whose party is gone (bots logged off, party redrawn) is dropped; its tank flag too.
        for (auto run = _levelRuns.begin(); run != _levelRuns.end();)
        {
            if (membersOf.count(run->first) == 0 || snapshotOf(run->first) == nullptr)
            {
                if (auto const state = _bots.find(run->first); state != _bots.end())
                    state->second.LevelRun = false;
                run = _levelRuns.erase(run);
            }
            else
                ++run;
        }
    }

    // ---- LFG: the group of the speaker goes into the dungeon once the asked bots have joined ----
    for (auto order = _lfgOrders.begin(); order != _lfgOrders.end();)
    {
        std::vector<BotSnapshot const*> group;
        for (BotSnapshot const& bot : view.bots)
            if (bot.group_leader_guid == order->first)
                group.push_back(&bot);
        LfgOrder& lfg = order->second;
        auto const route = _options.DungeonsByMap.find(lfg.Map);
        bool drop = route == _options.DungeonsByMap.end() || nowMs - lfg.StartMs > kLfgOrderMaxMs;
        if (!drop && !lfg.Entered)
        {
            std::size_t joined = 0;
            for (BotSnapshot const* bot : group)
                joined += std::find(lfg.Asked.begin(), lfg.Asked.end(), bot->guid) != lfg.Asked.end() ? 1 : 0;
            bool const waitedEnough = nowMs - lfg.StartMs > kLfgJoinWaitMs;
            bool const ready = lfg.Asked.empty() ? !group.empty() : (joined == lfg.Asked.size() || (joined > 0 && waitedEnough));
            if (ready)
            {
                for (BotSnapshot const* bot : group)
                    if (bot->life_state == 0 && bot->in_combat == 0)
                        teleport(*bot, lfg.Map, route->second.Entry);
                lfg.Entered = true;
                lfg.RetryMs = nowMs;
                event("lfg_enter: speaker=" + std::to_string(order->first) + " dungeon=" + lfg.Id + " bots=" + std::to_string(group.size()));
            }
            else if (waitedEnough && joined == 0 && lfg.Asked.size() > 0 && nowMs - lfg.StartMs > 2 * kLfgJoinWaitMs)
                drop = true; // nobody invited them
        }
        else if (!drop)
        {
            // Inside: the first tank-class bot of the group in the dungeon leads. No such bot (a human tank): the order is over.
            bool led = false;
            for (BotSnapshot const* bot : group)
            {
                BotState& state = StateOf(bot->guid);
                if (bot->map_id != lfg.Map || bot->life_state != 0 || state.TankLead)
                    continue;
                if (state.Role_ == BotState::Role::Tank || (state.Role_ == BotState::Role::Auto && ClassFor(_options, bot->guid) == kWarriorClass))
                {
                    state.ChatFollow = 0;
                    state.ChatStay = false;
                    state.LevelRun = false;
                    BeginTankLead(state, *bot, route->second);
                    event("lfg_lead: bot=" + std::to_string(bot->guid) + " dungeon=" + lfg.Id);
                    led = true;
                    break;
                }
            }
            if (led || nowMs - lfg.RetryMs > 30000)
                drop = true;
        }
        order = drop ? _lfgOrders.erase(order) : std::next(order);
    }
}

std::vector<ChatAck> BotPolicy::TakeWhispers()
{
    std::vector<ChatAck> out;
    out.swap(_lfgFillWhispers);
    return out;
}

void BotPolicy::DriveLfgFill(WorldTickSnapshotView const& view, std::unordered_map<std::uint64_t, std::size_t> const& indexOf, std::uint64_t nowMs,
                             std::vector<BotAction>& actions)
{
    auto const event = [&](std::string const& text)
    {
        if (_options.OnEvent)
            _options.OnEvent(text);
    };
    auto const isOurBot = [&indexOf](std::uint64_t ref) { return ref != 0 && indexOf.count(ref & forever_module::kUnitRefCounterMask) != 0; };
    // A bot's role: the one a chat order gave it, else its class (an unknown class counts as a damage dealer).
    auto const roleOf = [this](std::uint64_t guid) -> level_dungeon::Role
    {
        if (auto const state = _bots.find(guid); state != _bots.end())
        {
            switch (state->second.Role_)
            {
                case BotState::Role::Tank: return level_dungeon::Role::Tank;
                case BotState::Role::Healer: return level_dungeon::Role::Healer;
                case BotState::Role::Dps: return level_dungeon::Role::Dps;
                default: break;
            }
        }
        std::uint32_t const classId = ClassFor(_options, guid);
        return classId == 0 ? level_dungeon::Role::Dps : level_dungeon::RoleOfClass(classId);
    };
    auto const sideOf = [this](std::uint64_t guid) -> std::uint8_t
    {
        if (_options.PartySideOf)
            return _options.PartySideOf(guid);
        return _options.SideOf ? _options.SideOf(guid) : std::uint8_t(0);
    };
    // A bot a levelling order or an LFG order has, or a bot with a chat order or a dungeon run, is not free.
    auto const busy = [this, nowMs](std::uint64_t guid) -> bool
    {
        auto const state = _bots.find(guid);
        if (state == _bots.end())
            return false;
        BotState const& s = state->second;
        return s.Traveling || s.SupplyTrip || s.ChatStay || s.ChatFollow != 0 || s.LevelRun || s.TankLead || s.LevelHoldUntilMs > nowMs;
    };
    std::set<std::uint64_t> summoned; // bots an LFG order of a speaker has asked for
    for (auto const& order : _lfgOrders)
        summoned.insert(order.second.Asked.begin(), order.second.Asked.end());

    // The human parties: their members, by the leader's player reference (a leader that is not one of this module's bots).
    std::map<std::uint64_t, std::vector<BotSnapshot const*>> parties;
    for (BotSnapshot const& bot : view.bots)
        if (bot.group_role != 0 && bot.group_leader_guid != 0 && !isOurBot(bot.group_leader_guid))
            parties[bot.group_leader_guid].push_back(&bot);

    for (auto const& [leaderRef, members] : parties)
    {
        // A speaker's LFG order fills its own group. A party with a member in a fight, dead, or in a dungeon is left alone.
        if (_lfgOrders.count(leaderRef) != 0 || members.empty())
            continue;
        bool calm = true;
        std::uint32_t partyLevel = 0, tanks = 0, healers = 0;
        for (BotSnapshot const* member : members)
        {
            if (member->life_state != 0 || member->in_combat != 0 || _options.DungeonsByMap.count(member->map_id) != 0)
            {
                calm = false;
                break;
            }
            partyLevel = std::max(partyLevel, member->level);
            switch (roleOf(member->guid))
            {
                case level_dungeon::Role::Tank: ++tanks; break;
                case level_dungeon::Role::Healer: ++healers; break;
                default: break;
            }
        }
        auto const lastParty = _lfgFillPartyMs.find(leaderRef);
        if (!calm || !lfg_fill::PartyDue(lastParty == _lfgFillPartyMs.end() ? 0 : lastParty->second, nowMs))
            continue;

        // The free bots: alone, out of combat and in the world, of the party's side, in its level band, not offered to a party lately.
        std::vector<lfg_fill::Candidate> candidates;
        for (BotSnapshot const& bot : view.bots)
        {
            if (bot.group_role != 0 || bot.group_invite_from != 0 || bot.life_state != 0 || bot.in_combat != 0 || bot.in_flight != 0 || bot.level == 0)
                continue;
            if ((_options.IsAlt && _options.IsAlt(bot.guid)) || _options.DungeonsByMap.count(bot.map_id) != 0 || busy(bot.guid) || summoned.count(bot.guid) != 0)
                continue;
            if (sideOf(bot.guid) != sideOf(members.front()->guid))
                continue;
            auto const lastBot = _lfgFillBotMs.find(bot.guid);
            if (!lfg_fill::BotDue(lastBot == _lfgFillBotMs.end() ? 0 : lastBot->second, nowMs))
                continue;
            candidates.push_back(lfg_fill::Candidate{ bot.guid, bot.level, roleOf(bot.guid) });
        }

        std::vector<lfg_fill::Offer> const offers = lfg_fill::PickOffers(std::uint32_t(members.size() + 1), tanks, healers, partyLevel, candidates);
        if (offers.empty())
            continue;
        // The bots go next to the party's first member (the leader is a human and its place is not in the snapshot).
        BotSnapshot const& anchor = *members.front();
        std::uint64_t const playerCounter = leaderRef & forever_module::kUnitRefCounterMask;
        std::string roles;
        for (lfg_fill::Offer const& offer : offers)
        {
            BotSnapshot const& bot = view.bots[indexOf.at(offer.Bot)];
            BotAction action;
            action.bot_guid = bot.guid;
            action.type = ActionType::kTeleport;
            action.spell_id = anchor.map_id;
            action.dest_x = anchor.pos_x;
            action.dest_y = anchor.pos_y;
            action.dest_z = anchor.pos_z;
            actions.push_back(action);
            BotState& state = StateOf(bot.guid);
            state.Last.reset();
            state.GotoActive = false;
            state.LevelHoldUntilMs = nowMs + kLevelHoldMs;
            _lfgFillBotMs[bot.guid] = nowMs;
            ChatAck whisper;
            whisper.Bot = bot.guid;
            whisper.Text = lfg_fill::OfferText(offer.RoleOf);
            whisper.Player = playerCounter;
            _lfgFillWhispers.push_back(whisper);
            roles += (roles.empty() ? "" : ",") + std::string(level_dungeon::RoleName(offer.RoleOf));
        }
        _lfgFillPartyMs[leaderRef] = nowMs;
        event("lfg_fill: party=" + std::to_string(playerCounter) + " members=" + std::to_string(members.size() + 1) + " offers=" + std::to_string(offers.size()) +
              " roles=" + roles);
    }
}

std::vector<std::string> BotPolicy::ApplyLfg(WorldTickSnapshotView const& view, ChatContext const& chat, ChatCommand const& command, std::uint64_t nowMs)
{
    KnownDungeons const known = KnownDungeonsOf(_options);
    if (known.List.empty())
        return { "LFG: no dungeon is known to the module." };
    int const found = level_dungeon::FindDungeon(known.List, command.Arg);
    if (found < 0)
        return { "LFG: I do not know \"" + command.Arg + "\". Known dungeons: " + level_dungeon::KeyList(known.List) + "." };
    level_dungeon::Dungeon const& dungeon = known.List[std::size_t(found)];
    std::uint64_t const issuerRef = PlayerRef(chat.Issuer);

    // The group so far: the speaker and the bots of its group. The speaker's own role is not known, so it counts as one member and no role.
    std::uint32_t members = 0, groupSize = 0, tanks = 0, healers = 0;
    for (BotSnapshot const& bot : view.bots)
    {
        if (bot.group_leader_guid != issuerRef)
            continue;
        ++members;
        groupSize = std::max(groupSize, bot.group_size);
        level_dungeon::Role const role = level_dungeon::RoleOfClass(ClassFor(_options, bot.guid));
        tanks += role == level_dungeon::Role::Tank ? 1 : 0;
        healers += role == level_dungeon::Role::Healer ? 1 : 0;
    }
    // the speaker plus its bots, or the party size the core reports when that is bigger
    std::uint32_t const size = std::max(members + 1, groupSize);
    std::vector<level_dungeon::Role> const wanted = level_dungeon::MissingRoles(size, tanks, healers);

    // The free bots: not in a group or invited, not in a bot party of the module's, alive and quiet, in the dungeon's level range, outside a dungeon.
    std::unordered_map<std::uint64_t, std::size_t> indexOf;
    for (std::size_t i = 0; i < view.bots.size(); ++i)
        indexOf[view.bots[i].guid] = i;
    std::unordered_map<std::uint64_t, std::uint64_t> leaderOf;
    std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> membersOf;
    BuildParties(view, indexOf, leaderOf, membersOf);
    std::vector<BotSnapshot const*> freeBots;
    for (BotSnapshot const& bot : view.bots)
    {
        if (bot.group_role != 0 || bot.group_leader_guid != 0 || bot.group_invite_from != 0 || bot.life_state != 0 || bot.in_combat != 0 || bot.in_flight != 0 ||
            bot.level < dungeon.MinLevel || bot.level > dungeon.MaxLevel || leaderOf.count(bot.guid) != 0 || _options.DungeonsByMap.count(bot.map_id) != 0 ||
            (_options.IsAlt && _options.IsAlt(bot.guid)))
            continue;
        freeBots.push_back(&bot);
    }
    // Near the speaker first (same map, then distance), then by guid so the pick is stable.
    std::sort(freeBots.begin(), freeBots.end(), [&](BotSnapshot const* a, BotSnapshot const* b)
    {
        auto const key = [&](BotSnapshot const* bot)
        {
            return std::make_pair(bot->map_id == chat.MapId ? DistanceTo(*bot, chat.Pos) : 1e30f, bot->guid);
        };
        return key(a) < key(b);
    });

    LfgOrder order;
    order.Map = dungeon.MapId;
    order.Id = dungeon.Id;
    order.StartMs = nowMs;
    order.RetryMs = nowMs;
    std::vector<level_dungeon::Role> filled, unfilled;
    std::uint32_t index = 0;
    for (level_dungeon::Role role : wanted)
    {
        auto pick = std::find_if(freeBots.begin(), freeBots.end(), [&](BotSnapshot const* bot) { return level_dungeon::RoleOfClass(ClassFor(_options, bot->guid)) == role; });
        if (pick == freeBots.end())
        {
            unfilled.push_back(role);
            continue;
        }
        BotSnapshot const& bot = **pick;
        // The bot walks (teleports) next to the speaker, who invites it; it accepts an invite from a player (AcceptInvites, ChatCommands).
        BotState& state = StateOf(bot.guid);
        forever_module::BotAction summon;
        summon.bot_guid = bot.guid;
        summon.type = ActionType::kSummon;
        summon.target_guid = issuerRef;
        summon.dest_x = float(index++);
        state.OrderAction = summon;
        state.GotoActive = false;
        state.Last.reset();
        order.Asked.push_back(bot.guid);
        filled.push_back(role);
        freeBots.erase(pick);
    }
    _lfgOrders[issuerRef] = order;

    std::string line = "LFG " + dungeon.Id + ": group of " + std::to_string(size);
    if (!filled.empty())
        line += ", filling " + level_dungeon::RoleList(filled);
    else if (wanted.empty())
        line += ", nothing missing";
    if (!unfilled.empty())
        line += "; no free " + level_dungeon::RoleList(unfilled) + " of level " + std::to_string(dungeon.MinLevel) + "-" + std::to_string(dungeon.MaxLevel);
    if (!filled.empty())
        line += ". Invite the bots next to you.";
    return { line };
}

role_profile::Profile BotPolicy::ProfileOfBot(std::uint64_t guid) const
{
    role_profile::PlayedRole role = role_profile::PlayedRole::Auto;
    if (auto const state = _bots.find(guid); state != _bots.end())
    {
        switch (state->second.Role_)
        {
            case BotState::Role::Tank: role = role_profile::PlayedRole::Tank; break;
            case BotState::Role::Healer: role = role_profile::PlayedRole::Healer; break;
            case BotState::Role::Dps: role = role_profile::PlayedRole::Dps; break;
            default: break;
        }
    }
    return role_profile::ProfileFor(ClassFor(_options, guid), role);
}
