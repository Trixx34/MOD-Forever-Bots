// SPDX-License-Identifier: GPL-2.0-or-later
// BotPolicy::Decide, a dead bot's turn (TurnDead, run by DecideBot; bot_policy.cpp): the death and ghost lines, the resurrect answer, the release,
// the revives and the corpse run. Every path through it ends the bot's turn.
#include "bot_policy_internal.h"

BotPolicy::TurnFlow BotPolicy::TurnDead(DecideTick& tick, BotTurn& turn)
{
    BotSnapshot const& bot = turn.bot;

    // ==== Dead-bot handling (corpse, resurrect, ghost run) ====
    // A dead bot (a corpse, or a ghost: it has health 1, so only life_state tells) gets its own orders and nothing else. State
    // changes are logged once.
    if (bot.life_state != 0)
    {
        DeadRecord(tick, turn);
        if (DeadAnswerResurrect(tick, turn) == TurnFlow::kSkip)
            return TurnFlow::kSkip;
        if (DeadCorpse(tick, turn) == TurnFlow::kSkip)
            return TurnFlow::kSkip;
        if (DeadHeldByWarder(tick, turn) == TurnFlow::kSkip)
            return TurnFlow::kSkip;
        if (DeadReviveByLeader(tick, turn) == TurnFlow::kSkip)
            return TurnFlow::kSkip;
        DeadGhostRun(tick, turn);
        return TurnFlow::kSkip;
    }
    return TurnFlow::kNext;
}

// A dead bot's event line, when there is a log.
void BotPolicy::LogEvent(std::string const& text) const
{
    if (_options.OnEvent)
        _options.OnEvent(text);
}

// A dead bot's move or stop, sent only when it differs from the last one and is not a place the core just found no route to.
void BotPolicy::DeadEmit(DecideTick& tick, BotTurn& turn, Decision const& wantedDead)
{
    std::uint64_t nowMs = tick.nowMs;
    std::vector<BotAction>& actions = tick.actions;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
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
    if (wantedDead.Type == ActionType::kMoveTo)
        state.LastMove = wantedDead;
}

// The corpse run is given up: the ghost stands, and why is logged.
void BotPolicy::DeadRunFailed(DecideTick& tick, BotTurn& turn, char const* why)
{
    std::uint64_t nowMs = tick.nowMs;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    state.RunFailed = true;
    DeadEmit(tick, turn, Decision{ ActionType::kStop, 0 });
    LogEvent(std::string("corpse_run_failed: bot=") + std::to_string(bot.guid) + " reason=" + why + " map=" + std::to_string(bot.map_id) +
          " corpse_map=" + std::to_string(bot.corpse_map) + " run_ms=" + std::to_string(state.RunStarted ? nowMs - state.RunStartMs : 0));
}

// Just died, or became a ghost: the living state is dropped and the change is logged.
void BotPolicy::DeadRecord(DecideTick& tick, BotTurn& turn)
{
    std::uint64_t nowMs = tick.nowMs;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto& nearby = turn.nearby;
    std::uint8_t const was = state.Life;
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
        LogEvent("died: bot=" + std::to_string(bot.guid) + " life=" + std::to_string(bot.life_state) + " map=" + std::to_string(bot.map_id) +
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
        LogEvent("ghost: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) + " corpse_map=" + std::to_string(bot.corpse_map) +
              " corpse_valid=" + std::to_string(bot.corpse_valid) + " reclaim_wait_ms=" + std::to_string(bot.reclaim_wait_ms));
    }
    state.Life = bot.life_state;
}

BotPolicy::TurnFlow BotPolicy::DeadAnswerResurrect(DecideTick& tick, BotTurn& turn)
{
    std::uint64_t nowMs = tick.nowMs;
    std::vector<BotAction>& actions = tick.actions;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
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
                LogEvent("resurrect_accept: bot=" + std::to_string(bot.guid) + " from=" + std::to_string(bot.resurrect_from) +
                      " life=" + std::to_string(bot.life_state) + " reason=offer_pending");
            }
        }
        return TurnFlow::kSkip;
    }
    return TurnFlow::kNext;
}

// A corpse: the leader's revive (Recover tactic), or the release once no resurrect is coming.
BotPolicy::TurnFlow BotPolicy::DeadCorpse(DecideTick& tick, BotTurn& turn)
{
    WorldTickSnapshotView const& view = tick.view;
    std::uint64_t nowMs = tick.nowMs;
    std::vector<BotAction>& actions = tick.actions;
    auto const& indexOf = tick.indexOf;
    auto const& tankLed = tick.tankLed;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto const& deadParty = turn.deadParty;
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
                    LogEvent("revive_corpse: bot=" + std::to_string(bot.guid) + " leader=" + std::to_string(leader->guid) + " attempt=" + std::to_string(state.ReviveAttempts) +
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
                LogEvent("release_held: bot=" + std::to_string(bot.guid) + " reason=combat would_release=" + reason);
            }
            return TurnFlow::kSkip;
        }
        if (!death_pause::ReleaseAllowed(_options.DeathPause, _options.RandomSeed, bot.guid, state.DeathMs, nowMs))
            return TurnFlow::kSkip; // DeathPause: a player takes a moment before pressing release
        BotAction release;
        release.bot_guid = bot.guid;
        release.type = ActionType::kReleaseSpirit;
        actions.push_back(release);
        state.ReleaseSent = true;
        state.ReleaseSentMs = nowMs;
        LogEvent(std::string("release: bot=") + std::to_string(bot.guid) + " reason=" + reason + " waited_ms=" + std::to_string(waited) +
              " rezzer_up=" + (rezzer ? "yes" : "no") + " alive_members=" + (anyAlive ? "yes" : "no"));
        return TurnFlow::kSkip;
    }
    return TurnFlow::kNext;
}

BotPolicy::TurnFlow BotPolicy::DeadHeldByWarder(DecideTick& tick, BotTurn& turn)
{
    WorldTickSnapshotView const& view = tick.view;
    std::uint64_t nowMs = tick.nowMs;
    auto const& indexOf = tick.indexOf;
    auto const& tankLed = tick.tankLed;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto const& deadParty = turn.deadParty;
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
    return TurnFlow::kNext;
}

BotPolicy::TurnFlow BotPolicy::DeadReviveByLeader(DecideTick& tick, BotTurn& turn)
{
    WorldTickSnapshotView const& view = tick.view;
    std::uint64_t nowMs = tick.nowMs;
    std::vector<BotAction>& actions = tick.actions;
    auto const& indexOf = tick.indexOf;
    auto const& tankLed = tick.tankLed;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto const& deadParty = turn.deadParty;
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
                LogEvent("revive_out_of_combat: bot=" + std::to_string(bot.guid) + " leader=" + std::to_string(leader->guid) +
                      " attempt=" + std::to_string(state.ReviveAttempts) + " map=" + std::to_string(bot.map_id));
            }
            return TurnFlow::kSkip;
        }
    }
    return TurnFlow::kNext;
}

void BotPolicy::DeadGhostRun(DecideTick& tick, BotTurn& turn)
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
    // A ghost: back to the corpse. A corpse on another map (an instance, a ghost is released outside) is reached through the portal.
    if (state.RunFailed)
        return;
    if (!bot.corpse_valid)
    {
        DeadRunFailed(tick, turn, "no_corpse");
        return;
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
            LogEvent("party_teleport: bot=" + std::to_string(bot.guid) + " leader=" + std::to_string(leader.guid) + " map=" + std::to_string(bot.map_id) +
                  " leader_map=" + std::to_string(leader.map_id) + " waited_ms=" + std::to_string(nowMs - state.DeathMs));
        }
        return;
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
            LogEvent("dungeon_entry_teleport: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) + " corpse_map=" + std::to_string(bot.corpse_map) +
                  " waited_ms=" + std::to_string(nowMs - state.EntrySinceMs));
        }
        return; // at the entrance it waits for the leader's revive (the revive above); the corpse run is not walked
    }
    if (instanceParty && entryOf != _options.DungeonsByMap.end() && entryOf->second.HasEntry && !state.RunStarted)
        LogEvent("dungeon_entry_wait_expired: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) + " waited_ms=" + std::to_string(nowMs - state.EntrySinceMs));
    if (!state.RunStarted)
    {
        state.RunStarted = true;
        state.RunStartMs = nowMs;
        LogEvent("corpse_run_start: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) + " corpse_map=" +
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
        LogEvent("corpse_run_teleport: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) + " corpse_map=" + std::to_string(bot.corpse_map) +
              " reason=run_limit");
        return;
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
            LogEvent("corpse_run_teleport: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) + " corpse_map=" + std::to_string(bot.corpse_map) +
                  " reason=timeout");
            return;
        }
        DeadRunFailed(tick, turn, "timeout");
        return;
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
            LogEvent("corpse_map_reached: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) + " dist=" + std::to_string(int(dist)));
        }
        std::uint32_t const done = dist >= state.RunStartDist ? 0u : std::uint32_t((1.0f - dist / state.RunStartDist) * 100.0f);
        for (std::uint32_t mark : { 25u, 50u, 75u })
            if (done >= mark && state.RunProgress < mark)
            {
                state.RunProgress = mark;
                LogEvent("corpse_run_progress: bot=" + std::to_string(bot.guid) + " pct=" + std::to_string(mark) + " dist=" + std::to_string(int(dist)));
            }
        if (dist <= kReclaimYards)
        {
            DeadEmit(tick, turn, Decision{ ActionType::kStop, 0 });
            bool danger = false;
            for (NearbyUnit const& unit : nearby)
                danger = danger || (IsLiveMob(unit) && rotation::DistanceYards(bot, unit) <= kReclaimDangerYards);
            if (!danger)
                state.ReclaimDangerSinceMs = 0;
            else if (state.ReclaimDangerSinceMs == 0)
            {
                state.ReclaimDangerSinceMs = nowMs;
                LogEvent("reclaim_held: bot=" + std::to_string(bot.guid) + " reason=hostile_near_corpse dist=" + std::to_string(int(dist)));
            }
            bool const holdForDanger = danger && nowMs - state.ReclaimDangerSinceMs < kReclaimDangerWaitMs;
            if (bot.reclaim_wait_ms == 0 && nowMs >= state.ReclaimRetryMs && !holdForDanger)
            {
                BotAction reclaim;
                reclaim.bot_guid = bot.guid;
                reclaim.type = ActionType::kReclaimCorpse;
                actions.push_back(reclaim);
                state.ReclaimRetryMs = nowMs + kReclaimRetryMs;
                LogEvent("reclaim: bot=" + std::to_string(bot.guid) + " dist=" + std::to_string(int(dist)) + " run_ms=" + std::to_string(nowMs - state.RunStartMs));
            }
        }
        else
            DeadEmit(tick, turn, Decision{ ActionType::kMoveTo, 0, corpse });
    }
    else if (PolicyOptions::ScenarioRoute const& own = ScenarioOf(bot.guid); own.HasPortal && bot.map_id == own.PortalMap && bot.corpse_map == own.PortalInsideMap)
        DeadEmit(tick, turn, Decision{ ActionType::kMoveTo, 0, own.PortalPoint });
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
        LogEvent("corpse_teleport: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) + " corpse_map=" + std::to_string(bot.corpse_map) +
              " run_ms=" + std::to_string(nowMs - state.RunStartMs));
    }
}
