// SPDX-License-Identifier: GPL-2.0-or-later
#include "bot_policy_internal.h"

void BotPolicy::ReadResult(BotState& state, BotSnapshot const& bot, std::uint64_t nowMs)
{
    if (bot.last_action_sequence == 0 || bot.last_action_sequence == state.LastSequence)
        return;
    state.LastSequence = bot.last_action_sequence;

    // A long walk the core found no full route for is chained while it makes progress (NotePartialLeg); a walk that was applied ends the chain.
    bool const moveNoRoute = bot.last_action_type == uint8_t(ActionType::kMoveTo) && bot.last_action_status == BotActionStatus::kNoRoute;
    if (bot.last_action_type == uint8_t(ActionType::kMoveTo) && bot.last_action_status == BotActionStatus::kApplied)
        state.PartialActive = false;
    bool const chained = moveNoRoute && NotePartialLeg(state, bot, nowMs);

    // A LoS recovery walk whose navmesh route failed cannot clear the wall; leave that target
    // for a while instead of issuing the same impossible reposition every tick.
    if (state.LosRepositionTarget != 0 && bot.last_action_type == uint8_t(ActionType::kMoveTo) &&
        bot.last_action_status == BotActionStatus::kNoRoute)
    {
        AvoidUnit(state.AvoidUntilMs, state.AvoidCount, state.LosRepositionTarget, nowMs);
        state.LosRepositionTarget = 0;
        state.CloserUntilMs = 0;
        state.Last.reset();
    }

    // A town errand whose walk to the trainer has no route: the trip cannot be made, so it fails.
    if (state.Errand && moveNoRoute && !chained)
    {
        state.ErrandResult = ErrandOutcome::kFailed;
        state.Errand.reset();
        if (_options.OnEvent)
            _options.OnEvent("errand_failed: bot=" + std::to_string(bot.guid) + " reason=no_route");
    }
    // A town errand's train order: applied ends the trip; out of range, busy or moving is retried; any other refusal counts toward a failure.
    if (state.Errand && bot.last_action_type == uint8_t(ActionType::kTrain) && bot.last_action_status != BotActionStatus::kNone)
    {
        if (bot.last_action_status == BotActionStatus::kApplied)
        {
            state.ErrandResult = ErrandOutcome::kDone;
            state.Errand.reset();
            if (_options.OnEvent)
                _options.OnEvent("errand_done: bot=" + std::to_string(bot.guid));
        }
        else if (bot.last_action_status == BotActionStatus::kOutOfRange || bot.last_action_status == BotActionStatus::kBusy ||
                 bot.last_action_status == BotActionStatus::kMoving)
        {
            state.ErrandTrainSentMs = 0;
        }
        else if (++state.ErrandRefusals >= kErrandTrainRefusalMax)
        {
            state.ErrandResult = ErrandOutcome::kFailed;
            state.Errand.reset();
            if (_options.OnEvent)
                _options.OnEvent("errand_failed: bot=" + std::to_string(bot.guid) + " status=" + std::to_string(int(bot.last_action_status)));
        }
        else
        {
            state.ErrandTrainSentMs = 0;
        }
    }

    // A click or an item used on a door, a lever or a charge (dungeon events): the answer goes to the event that sent it.
    if ((bot.last_action_type == uint8_t(ActionType::kUseObject) || bot.last_action_type == uint8_t(ActionType::kGossip)) && state.Events)
    {
        bool const talk = bot.last_action_type == uint8_t(ActionType::kGossip);
        state.Events->OnUseResult(bot.last_action_status == BotActionStatus::kApplied ? dungeon_events::UseResult::Applied
                                  : bot.last_action_status == BotActionStatus::kOutOfRange ? dungeon_events::UseResult::OutOfRange
                                  : talk && bot.last_action_status == BotActionStatus::kRefused ? dungeon_events::UseResult::Unclear
                                  : talk && bot.last_action_status == BotActionStatus::kInvalidTarget ? dungeon_events::UseResult::GoneAway
                                                                                          : dungeon_events::UseResult::Failed, nowMs);
        if (_options.OnEvent && bot.last_action_status != BotActionStatus::kApplied)
            _options.OnEvent(std::string(bot.last_action_type == uint8_t(ActionType::kGossip) ? "gossip_refused" : "use_object_refused") + ": bot=" + std::to_string(bot.guid) +
                             " status=" + ActionStatusName(bot.last_action_status) +
                             (talk ? " spot_distance=" + std::to_string(int(std::hypot(bot.pos_x - state.GossipX, bot.pos_y - state.GossipY))) : std::string()));
        return;
    }

    // The core refuses a sit while in combat: the bot is not sitting, and the sit waits a second.
    if (bot.last_action_type == uint8_t(ActionType::kRest) && bot.last_action_status == BotActionStatus::kRefused)
    {
        state.Sitting = false;
        state.RestRetryAtMs = nowMs + kRetryMs;
        return;
    }

    // A flight refused (no flight master in reach, no path, not worth it, in combat): the bot walks for a while.
    if (bot.last_action_type == uint8_t(ActionType::kTaxi) && bot.last_action_status != BotActionStatus::kApplied &&
        bot.last_action_status != BotActionStatus::kNone)
    {
        state.TaxiBlockedUntilMs = nowMs + _options.Taxi.BlockedMs;
        state.TaxiWaitUntilMs = 0;
        if (state.TaxiFlight.OrderedMs != 0 && _options.OnEvent)
            _options.OnEvent(taxi::RefusedLine(bot.guid, bot.map_id, ActionStatusName(bot.last_action_status)));
        state.TaxiFlight = taxi::Flight{};
        return;
    }

    // Eating or drinking refused: busy or moving is a short wait, anything else (nothing to eat, supplies off in the core) a long one.
    if (bot.last_action_type == uint8_t(ActionType::kConsume) && bot.last_action_status != BotActionStatus::kApplied &&
        bot.last_action_status != BotActionStatus::kNone)
    {
        bool const brief = bot.last_action_status == BotActionStatus::kBusy || bot.last_action_status == BotActionStatus::kMoving;
        state.NextConsumeMs = nowMs + (brief ? 8000 : 120000);
        return;
    }

    // Auto gear refused (in combat, switched off in the core): tried again after a while.
    if (bot.last_action_type == uint8_t(ActionType::kAutoGear) && bot.last_action_status == BotActionStatus::kRefused)
    {
        state.GearedAtLevel = 0;
        state.GearRetryMs = nowMs + 60000;
        return;
    }

    // A move the core found no route for, or a quest-walk teleport the core refused: that start's giver is left alone for a while
    // (the planner picks the next start). Each result is read once, by its sequence. A refused teleport must not be sent again.
    // Diagnostics: a teleport the core did not apply (DungeonTeleport and the scenario's teleports) is logged with the reason the core gave, once per sequence.
    if (bot.last_action_type == uint8_t(ActionType::kTeleport) && bot.last_action_status != BotActionStatus::kApplied &&
        bot.last_action_status != BotActionStatus::kNone && bot.last_action_sequence != state.TeleportLoggedSequence && _options.OnEvent)
    {
        state.TeleportLoggedSequence = bot.last_action_sequence;
        _options.OnEvent("teleport_refused: bot=" + std::to_string(bot.guid) + " status=" + ActionStatusName(bot.last_action_status) + " pos=" +
                         std::to_string(int(bot.pos_x)) + "," + std::to_string(int(bot.pos_y)) + "," + std::to_string(int(bot.pos_z)) + " in_combat=" + std::to_string(bot.in_combat));
    }
    // Diagnostics: what a refusal for no route was for (the log otherwise says only "no route"), at most once in ten seconds for a bot.
    if (bot.last_action_status == BotActionStatus::kNoRoute && _options.OnEvent && nowMs - state.NoRouteLoggedMs >= 10000)
    {
        state.NoRouteLoggedMs = nowMs;
        std::string what = "action_type=" + std::to_string(int(bot.last_action_type));
        // The move the core refused is the last kMoveTo sent (LastMove); Last is the newest decision, often a stop sent after it.
        if (state.LastMove)
            what += " wanted=" + std::to_string(int(state.LastMove->Type)) + " target=" + std::to_string(state.LastMove->Target) + " target_entry=" +
                    std::to_string(state.LastMove->Target ? forever_module::UnitRefEntry(state.LastMove->Target) : 0) + " dest=" + std::to_string(int(state.LastMove->Dest.X)) + "," +
                    std::to_string(int(state.LastMove->Dest.Y)) + "," + std::to_string(int(state.LastMove->Dest.Z));
        // Triage (route_triage.h): what the bot was doing, so the refusals can be split by cause (cause=arrived|combat|to_unit|to_point|no_move).
        float const toDest = state.LastMove ? std::hypot(bot.pos_x - state.LastMove->Dest.X, bot.pos_y - state.LastMove->Dest.Y) : 0.0f;
        what += std::string(" cause=") + route_triage::Name(route_triage::Classify(bot.in_combat != 0, state.LastMove.has_value(),
                                                                                  state.LastMove && state.LastMove->Target != 0, toDest));
        if (state.Last)
            what += " last_sent=" + std::to_string(int(state.Last->Type));
        _options.OnEvent("no_route: bot=" + std::to_string(bot.guid) + " " + what + " pos=" + std::to_string(int(bot.pos_x)) + "," + std::to_string(int(bot.pos_y)) + "," +
                         std::to_string(int(bot.pos_z)) + " in_combat=" + std::to_string(bot.in_combat) + " route_index=" + std::to_string(state.RouteIndex));
    }
    bool const noRoute = moveNoRoute && !chained;
    bool const teleportRefused = bot.last_action_type == uint8_t(ActionType::kTeleport) &&
                                 bot.last_action_status != BotActionStatus::kApplied && bot.last_action_status != BotActionStatus::kNone;
    // A dungeon leader counts these against its current waypoint (kRouteNoRouteSkip). A move to the very place the bot already stands at is refused for lack of a route too (a
    // path of no length), which is an arrival, not a missing route: it is not counted.
    if (noRoute && bot.last_action_sequence != state.NoRouteSequence &&
        !(state.LastMove && std::hypot(bot.pos_x - state.LastMove->Dest.X, bot.pos_y - state.LastMove->Dest.Y) < 6.0f))
        ++state.RouteNoRoute;
    if ((noRoute || teleportRefused) && bot.last_action_sequence != state.NoRouteSequence)
    {
        state.NoRouteSequence = bot.last_action_sequence;
        if (noRoute)
            NoteMoveRefused(state, bot, nowMs);
        state.QuestTeleportSent = false;
        QuestsFor(bot).NoteNoRoute(state.Quest, nowMs, &bot);
        if (teleportRefused && _options.OnEvent)
            _options.OnEvent("quest_walk_blocked: bot=" + std::to_string(bot.guid) + " reason=teleport_refused status=" +
                             ActionStatusName(bot.last_action_status));
    }

    // A quest action refused (not eligible, bags full, no NPC in reach): the quest is left alone for a while.
    if (bot.last_action_type == uint8_t(ActionType::kQuest) && bot.last_action_status == BotActionStatus::kRefused)
    {
        // Each answer once (the snapshot repeats the last one every tick). Three bots sent a refused quest action every three seconds for two hours (5,400 of the 6,000
        // refusals): a run of refusals, whichever quests they were for, now pauses the quest engine for ten minutes and the bot grinds meanwhile.
        if (bot.last_action_sequence == state.QuestSeenSeq)
            return;
        state.QuestSeenSeq = bot.last_action_sequence;
        // The core does not echo the quest in the snapshot (last_action_spell is 0 for a quest action), so the quest is the one the planner sent last.
        // The planner knows the kind too: a hand-in or reward is its PendingHandIn, anything else is an accept or an abandon.
        if (_options.OnEvent && state.Quest.LastActionQuest != 0)
        {
            std::string const kind = state.Quest.PendingHandIn == state.Quest.LastActionQuest ? "hand_in" : "accept_or_abandon";
            _options.OnEvent("quest_refused: bot=" + std::to_string(bot.guid) + " quest=" + std::to_string(state.Quest.LastActionQuest) + " kind=" + kind +
                             " bag_free=" + std::to_string(bot.bag_free_slots) + " bag_room=" + (bot.bag_free_slots > 0 ? "yes" : "no"));
        }
        if (nowMs - state.QuestWindowMs > kQuestWindowMs)
        {
            state.QuestWindowMs = nowMs;
            state.QuestRefusals = 0;
        }
        if (++state.QuestRefusals >= kQuestRefusalsPause)
        {
            state.QuestPausedUntilMs = nowMs + kQuestPauseMs;
            state.QuestRefusals = 0;
            if (_options.OnEvent)
                _options.OnEvent("quest_paused: bot=" + std::to_string(bot.guid) + " reason=refused_run minutes=" + std::to_string(kQuestPauseMs / 60000));
        }
        if (QuestsFor(bot).NoteRefused(state.Quest, nowMs))
        {
            // A hand-in the core refused: the reward did not fit in the bags. A bot with quest work is never idle, so the vendor visit that
            // empties the bags (sell, equip) is not left to the idle bot's turn.
            state.ServiceUrgent = true;
            state.NextServiceMs = 0;
        }
        return;
    }

    // A kTransport order refused (the core's Bot.Actions.Transport off, the leader on no transport, a bot in combat, too far from the leader): the next one waits.
    // Out of range and busy are short waits (the bot is walking closer); any other refusal backs off for TransportRideBackoffMs.
    if (bot.last_action_type == uint8_t(ActionType::kTransport) && bot.last_action_status != BotActionStatus::kApplied && bot.last_action_status != BotActionStatus::kNone)
    {
        bool const brief = bot.last_action_status == BotActionStatus::kOutOfRange || bot.last_action_status == BotActionStatus::kBusy;
        state.TransportNextMs = nowMs + (brief ? _options.TransportRide.EveryMs : _options.TransportRide.BackoffMs);
        if (_options.OnEvent)
            _options.OnEvent("transport_refused: bot=" + std::to_string(bot.guid) + " status=" + ActionStatusName(bot.last_action_status) + " map=" + std::to_string(bot.map_id) +
                             " transport=" + std::to_string(bot.transport_guid));
        return;
    }

    // A party action refused (no invite waiting, a full party): the party waits a moment before the next one.
    if (bot.last_action_type == uint8_t(ActionType::kGroup) && bot.last_action_status == BotActionStatus::kRefused)
    {
        state.PartyRetryAtMs = nowMs + kPartyRetryMs;
        return;
    }

    // Mount / dismount: record whether the request succeeded so the policy does not keep repeating it.
    // The answer is for the request that was sent (MountSentUp), whatever spell the core reports, and each answer is read once. A bot with no mount learned
    // (every bot below the riding level) was refused every 15 seconds: 19,000 refusals in two hours on 300 bots. Mount refusals now pause for two minutes, then
    // four, up to an hour; an answer for a dismount of a bot that was not mounted only clears the flag.
    if (bot.last_action_type == uint8_t(ActionType::kMount))
    {
        if (bot.last_action_sequence != state.MountSeenSeq)
        {
            state.MountSeenSeq = bot.last_action_sequence;
            if (bot.last_action_status == BotActionStatus::kApplied)
            {
                state.Mounted = state.MountSentUp || bot.last_action_spell == 1; // (a bot already mounted when the policy began has sent nothing: the spell tells)
                if (state.Mounted)
                    state.MountFails = 0;
            }
            else
            {
                state.Mounted = false;
                if (state.MountSentUp)
                {
                    state.MountFails = std::min<std::uint32_t>(state.MountFails + 1, 12);
                    std::uint64_t wait = kMountFailBaseMs;
                    for (std::uint32_t i = 1; i < state.MountFails && wait < kMountFailMaxMs; ++i)
                        wait *= 2;
                    state.MountRetryMs = nowMs + std::min(wait, kMountFailMaxMs);
                }
                else
                    state.MountRetryMs = nowMs + 15000;
            }
        }
        return;
    }

    // The death actions: a refusal is logged, and the action waits before it is sent again.
    if (bot.last_action_type == uint8_t(ActionType::kReleaseSpirit) || bot.last_action_type == uint8_t(ActionType::kResurrect) ||
        bot.last_action_type == uint8_t(ActionType::kReclaimCorpse))
    {
        bool const ok = bot.last_action_status == BotActionStatus::kApplied;
        char const* const what = bot.last_action_type == uint8_t(ActionType::kReleaseSpirit) ? "release"
                                 : bot.last_action_type == uint8_t(ActionType::kResurrect) ? "resurrect_accept" : "reclaim";
        bool logIt = !ok;
        if (bot.last_action_type == uint8_t(ActionType::kResurrect))
        {
            // An answer to a resurrect offer that is refused again and again must not hold the bot: after a few it is left alone for a minute.
            if (ok)
                state.AcceptRefusals = 0;
            else
            {
                logIt = state.AcceptRefusals == 0; // one line per run of refusals
                if (++state.AcceptRefusals >= kAcceptRefusalsMax)
                {
                    state.AcceptIgnoreUntilMs = nowMs + kAcceptIgnoreMs;
                    state.AcceptIgnoreFrom = bot.resurrect_from; // a different resurrector is still answered
                    state.AcceptRefusals = 0;
                }
            }
        }
        if (logIt && _options.OnEvent)
            _options.OnEvent(std::string("death_action_refused: bot=") + std::to_string(bot.guid) + " action=" + what +
                             " status=" + ActionStatusName(bot.last_action_status));
        if (ok && bot.last_action_type == uint8_t(ActionType::kReclaimCorpse))
            state.Reclaimed = true;
        return;
    }

    // A melee attack refused or with no valid target: that unit is avoided for kAvoidMs, so the bot picks another.
    if (bot.last_action_type == uint8_t(ActionType::kAttackTarget) &&
        (bot.last_action_status == BotActionStatus::kRefused || bot.last_action_status == BotActionStatus::kInvalidTarget))
    {
        if (state.Last && state.Last->Type == ActionType::kAttackTarget && state.Last->Target != 0)
            AvoidUnit(state.AvoidUntilMs, state.AvoidCount, state.Last->Target, nowMs);
        return;
    }

    // A craft (SDK 0.17): started, the next one comes when the cast is done; busy or moving, soon; kNeedsFocus (SDK 0.18): the bot walks to the nearest anvil, forge or
    // fire and asks again there; any other answer (no recipe, no materials, a skill too high for the recipes) means there is nothing to make now: that skill waits five
    // minutes, varied per bot. The answer is for the line that was sent (CraftSent), whatever spell the core reports.
    if (bot.last_action_type == uint8_t(ActionType::kSkillUp))
    {
        std::size_t const index = std::min<std::size_t>(state.CraftSent, kCraftLineCount - 1);
        auto const waitRefused = [&](std::uint64_t base)
        {
            state.CraftRetryAtMs[index] = nowMs + bot_random::Vary(base, std::min<std::uint32_t>(_options.RandomPct * 2, 90),
                                                                   bot_random::Draw(_options.RandomSeed, bot.guid, bot_random::kSaltIdle, 0x400000000ull + nowMs / 1000));
        };
        if (bot.last_action_status == BotActionStatus::kApplied)
            state.CraftRetryAtMs[index] = nowMs + kSkillUpNextMs;
        else if (bot.last_action_status == BotActionStatus::kBusy || bot.last_action_status == BotActionStatus::kMoving)
            state.CraftRetryAtMs[index] = nowMs + kSkillUpBusyMs;
        else if (bot.last_action_status == BotActionStatus::kNeedsFocus)
        {
            // Where the focus is: the nearest object on this map, if the table is loaded and it is not too far; the walk is begun by the idle logic (CraftWalking).
            std::uint32_t const focus = crafting::FocusOfSkill(kCraftLines[index]);
            std::optional<std::size_t> object;
            if (_options.Foci && focus != 0)
                object = crafting::NearestFocus(*_options.Foci, focus, bot.map_id, bot.pos_x, bot.pos_y);
            if (object && std::hypot(bot.pos_x - (*_options.Foci)[*object].X, bot.pos_y - (*_options.Foci)[*object].Y) <= kCraftWalkMaxYards)
            {
                state.CraftWalking = true;
                state.CraftWalkLine = std::uint32_t(index);
                state.CraftWalkIndex = std::uint32_t(*object);
                state.CraftWalkStartMs = nowMs;
                state.CraftRetryAtMs[index] = nowMs + kCraftWalkTimeoutMs; // held while the walk runs; its end sets the time
                if (_options.OnEvent)
                    _options.OnEvent("craft_walk: bot=" + std::to_string(bot.guid) + " skill=" + std::to_string(kCraftLines[index]) + " focus=" + std::to_string(focus) +
                                     " object=" + (*_options.Foci)[*object].Name);
            }
            else
                waitRefused(kCraftWalkFailMs);
        }
        else
            waitRefused(kSkillUpRefusedMs);
        return;
    }

    // A gather (SDK 0.18): started, the next ask comes when the cast is done (and takes the loot); walking to the node, soon; nothing near: a minute and a half.
    if (bot.last_action_type == uint8_t(ActionType::kGather))
    {
        std::size_t const index = std::min<std::size_t>(state.GatherSent, kGatherLineCount - 1);
        if (bot.last_action_status == BotActionStatus::kApplied)
        {
            state.GatherRetryAtMs[index] = nowMs + kSkillUpNextMs;
            state.GatherFails[index] = 0;
            state.GatherWalking = false;
        }
        else if (bot.last_action_status == BotActionStatus::kBusy || bot.last_action_status == BotActionStatus::kMoving)
            state.GatherRetryAtMs[index] = nowMs + kSkillUpBusyMs;
        else
        {
            // "nothing to gather within 30 yards" is the normal answer where the bot stands, and the module does not look for nodes: the pause doubles with every
            // answer in a row (90 s up to 48 minutes), and starts again after a gather that worked.
            // GatherWalk: nothing in reach, and the table has a node of this skill on the bot's map that is not too far: the bot walks to it (the idle logic does the walk).
            if (bot.last_action_status == BotActionStatus::kRefused && _options.GatherWalk && _options.GatherNodes)
            {
                std::optional<std::size_t> const node = gather::NearestNode(*_options.GatherNodes, kGatherLines[index], bot.map_id, bot.pos_x, bot.pos_y);
                if (node && std::hypot(bot.pos_x - (*_options.GatherNodes)[*node].X, bot.pos_y - (*_options.GatherNodes)[*node].Y) <= kGatherWalkMaxYards)
                {
                    state.GatherWalking = true;
                    state.GatherWalkLine = std::uint32_t(index);
                    state.GatherWalkIndex = std::uint32_t(*node);
                    state.GatherWalkStartMs = nowMs;
                    state.GatherRetryAtMs[index] = nowMs + kGatherWalkTimeoutMs; // held while the walk runs; its end sets the time
                    if (_options.OnEvent)
                        _options.OnEvent("gather_walk: bot=" + std::to_string(bot.guid) + " skill=" + std::to_string(kGatherLines[index]) + " node=" +
                                         std::to_string((*_options.GatherNodes)[*node].Entry));
                    return;
                }
            }
            std::uint64_t base = bot.last_action_status == BotActionStatus::kRefused ? kGatherNothingMs : kGatherRefusedMs;
            if (bot.last_action_status == BotActionStatus::kRefused)
            {
                if (bot.last_action_sequence != state.GatherSeenSeq)
                {
                    state.GatherSeenSeq = bot.last_action_sequence;
                    state.GatherFails[index] = std::min<std::uint32_t>(state.GatherFails[index] + 1, kGatherFailDoublings + 1);
                }
                base <<= std::min<std::uint32_t>(state.GatherFails[index] == 0 ? 0 : state.GatherFails[index] - 1, kGatherFailDoublings);
            }
            state.GatherRetryAtMs[index] = nowMs + bot_random::Vary(base, std::min<std::uint32_t>(_options.RandomPct * 2, 90),
                                                                    bot_random::Draw(_options.RandomSeed, bot.guid, bot_random::kSaltIdle, 0x500000000ull + nowMs / 1000));
        }
        return;
    }

    ReadCastResult(state, bot, nowMs);
}

// The answer to a cast (ActionType::kCastSpell): the Deadmines cost log, the resurrect and pet spells, the refusals by status, and the upkeep buffs' back-off.
// Split from ReadResult (which has already taken the sequence): a spell with no id, or any other action type, is not read here.
void BotPolicy::NoteMoveRefused(BotState& state, BotSnapshot const& bot, std::uint64_t nowMs)
{
    state.WalkBlockedAtMs = nowMs;
    // The same refused move must not be sent again every tick (it was 26 times a second for 220 bots): the place is blocked for a pause that
    // doubles with every refusal in a row, 5 s up to 2 minutes.
    if (state.LastMove)
    {
        bool const same = state.NoRouteUntilMs != 0 && std::hypot(state.NoRouteDest.X - state.LastMove->Dest.X, state.NoRouteDest.Y - state.LastMove->Dest.Y) < kNoRouteSameYards;
        state.NoRouteStreak = same ? std::min<std::uint32_t>(state.NoRouteStreak + 1, 6) : 1;
        state.NoRouteDest = state.LastMove->Dest;
        state.NoRouteUntilMs = nowMs + (kNoRoutePauseMs << (state.NoRouteStreak - 1));
        // The kite tries one point after another, and only the last refusal blocked: with two points it went back and forth between them (2026-10-30: 91,000 refusals in
        // 15 minutes). Every refused point is kept for a while, so the circle is walked through and the bot then stands.
        std::erase_if(state.RefusedPlaces, [nowMs](auto const& place) { return place.second <= nowMs; });
        if (state.RefusedPlaces.size() >= kRefusedPlacesKept)
            state.RefusedPlaces.erase(state.RefusedPlaces.begin());
        state.RefusedPlaces.emplace_back(state.LastMove->Dest, nowMs + kRefusedPlaceMs);
        // A walk to a grind spot the core has no route to: reported to the shared list, which blocks the spot for every bot once enough bots agree.
        if (state.Wandering && state.GrindSpotIndex != std::size_t(-1) && _options.Grind && state.GrindSpotIndex < _options.Grind->Spots.size() &&
            std::hypot(state.GrindTarget.X - state.LastMove->Dest.X, state.GrindTarget.Y - state.LastMove->Dest.Y) < 1.0f &&
            _grindBlocks.Report(state.GrindSpotIndex, bot.guid, nowMs) && _options.OnEvent)
            _options.OnEvent("grind_spot_blocked: spot=" + std::to_string(state.GrindSpotIndex) + " minutes=" + std::to_string(kGrindBlockMs / 60000));
    }

}

// A kNoRoute answer for a walk of kPartialMinYards or more. The core starts the move anyway and the bot walks as far as the navmesh path reaches, and a long path is
// capped, so the answer often means "a first leg only" (2026-11-09/10 sim: 62,000 refusals for walks over 300 yards, 39,000 over 1,000, each ending the walk). The
// first such answer starts a chain and gives nothing up; PartialChainTick judges each leg.
bool BotPolicy::NotePartialLeg(BotState& state, BotSnapshot const& bot, std::uint64_t nowMs)
{
    // A dungeon or raid route has its own handling of refusals (skip, rewind, teleport): it is left as it is.
    if (!state.LastMove || state.LastMove->Target != 0 || IsRouteActive(bot.guid))
        return false;
    float const toDest = std::hypot(bot.pos_x - state.LastMove->Dest.X, bot.pos_y - state.LastMove->Dest.Y);
    if (toDest < kPartialMinYards)
    {
        state.PartialActive = false;
        return false;
    }
    bool const same = state.PartialActive && std::hypot(state.PartialDest.X - state.LastMove->Dest.X, state.PartialDest.Y - state.LastMove->Dest.Y) < kNoRouteSameYards;
    if (!same)
    {
        state.PartialActive = true;
        state.PartialDest = state.LastMove->Dest;
        state.PartialLastYards = toDest;
        state.PartialLegs = 0;
    }
    else if (state.PartialLegs >= kPartialLegsMax)
    {
        state.PartialActive = false;
        return false;
    }
    state.PartialSettleMs = nowMs + kPartialSettleMs;
    state.PartialLegStartMs = nowMs;
    return true;
}

void BotPolicy::PartialChainTick(BotState& state, BotSnapshot const& bot, std::uint64_t nowMs)
{
    if (!state.PartialActive)
        return;
    // The chain is for the move that was sent last; when the bot has been given something else to do, it is over (nothing is blocked).
    bool const stillThatWalk = state.Last && state.Last->Type == ActionType::kMoveTo && state.LastMove &&
                               std::hypot(state.LastMove->Dest.X - state.PartialDest.X, state.LastMove->Dest.Y - state.PartialDest.Y) < 1.0f &&
                               std::hypot(state.Last->Dest.X - state.PartialDest.X, state.Last->Dest.Y - state.PartialDest.Y) < 1.0f;
    if (!stillThatWalk || bot.life_state != 0 || nowMs - state.PartialLegStartMs > kPartialLegMaxMs)
    {
        state.PartialActive = false;
        return;
    }
    if (nowMs < state.PartialSettleMs || bot.moving != 0 || bot.in_combat != 0)
        return;
    float const toDest = std::hypot(bot.pos_x - state.PartialDest.X, bot.pos_y - state.PartialDest.Y);
    if (toDest <= kPartialArrivedYards)
    {
        state.PartialActive = false;
        return;
    }
    if (state.PartialLastYards - toDest >= kPartialProgressYards)
    {
        // The leg brought the bot nearer: the same move is sent again from here.
        ++state.PartialLegs;
        if (_options.OnEvent)
            _options.OnEvent("walk_chained: bot=" + std::to_string(bot.guid) + " leg=" + std::to_string(state.PartialLegs) + " to_dest=" + std::to_string(int(toDest)) +
                             " gained=" + std::to_string(int(state.PartialLastYards - toDest)) + " pos=" + std::to_string(int(bot.pos_x)) + "," + std::to_string(int(bot.pos_y)));
        state.PartialLastYards = toDest;
        state.PartialSettleMs = nowMs + kPartialSettleMs;
        state.PartialLegStartMs = nowMs;
        state.Last.reset(); // the move is the same, so it would not be sent again otherwise
        return;
    }
    // The leg went nowhere: the route is really missing. The walk is refused now, as it would have been at the first answer.
    state.PartialActive = false;
    if (_options.OnEvent)
        _options.OnEvent("walk_chain_ended: bot=" + std::to_string(bot.guid) + " legs=" + std::to_string(state.PartialLegs) + " to_dest=" + std::to_string(int(toDest)));
    ++state.RouteNoRoute;
    NoteMoveRefused(state, bot, nowMs);
    state.QuestTeleportSent = false;
    QuestsFor(bot).NoteNoRoute(state.Quest, nowMs, &bot);
}

void BotPolicy::ReadCastResult(BotState& state, BotSnapshot const& bot, std::uint64_t nowMs)
{
    if (bot.last_action_type != uint8_t(ActionType::kCastSpell) || bot.last_action_spell == 0)
        return;

    std::uint32_t const spell = bot.last_action_spell;
    if (ScenarioOf(bot.guid).RouteMap == 36 && _options.OnEvent)
    {
        auto const cast = state.CastTarget.find(spell);
        std::uint64_t const target = cast == state.CastTarget.end() ? 0 : cast->second;
        // The level and what the highest rank of this spell at that level costs (the file's flat cost, -1 when unpriced): a cast whose real power drop
        // is the rank 1 cost at level 23 shows the bot never learned the higher ranks.
        std::int64_t wantCost = -1;
        if (_options.SpellCosts)
            if (auto const pay = _options.SpellCosts->CostAtLevel(spell, bot.level))
                wantCost = pay->Cost;
        _options.OnEvent("cast_result: time_ms=" + std::to_string(nowMs) + " bot=" + std::to_string(bot.guid) + " spell=" + std::to_string(spell) +
                         " level=" + std::to_string(bot.level) + " want_cost=" + std::to_string(wantCost) +
                         " target=" + std::to_string(target) + " status=" + ActionStatusName(bot.last_action_status) +
                         " hp=" + std::to_string(bot.health) + "/" + std::to_string(bot.max_health) +
                         " power=" + std::to_string(bot.power) + "/" + std::to_string(bot.max_power));
    }
    if (IsResurrectSpell(spell))
    {
        // A resurrect: the dead member is left alone while the cast runs, or after a refusal for a while; a run of refusals is logged.
        auto const cast = state.CastTarget.find(spell);
        std::uint64_t const target = cast != state.CastTarget.end() ? cast->second : 0;
        if (bot.last_action_status == BotActionStatus::kApplied)
        {
            state.RezRetryAtMs[target] = nowMs + kRezCastMs;
            state.RezRefusals[target] = 0;
        }
        else
        {
            state.RezRetryAtMs[target] = nowMs + kRezRetryMs;
            if (++state.RezRefusals[target] == kRezRefusalsLogged && _options.OnEvent)
                _options.OnEvent("rez_refused: rezzer=" + std::to_string(bot.guid) + " spell=" + std::to_string(spell) + " target=" +
                                 std::to_string(target) + " status=" + ActionStatusName(bot.last_action_status));
        }
        return;
    }
    if (spell == kCallPet || spell == kRevivePet || spell == kMendPet)
    {
        // The snapshot has no pet, so the results of the pet casts are the only evidence of its state.
        switch (bot.last_action_status)
        {
            case BotActionStatus::kApplied:
                state.TamePetFirst = false;
                state.PetMissing = false;
                state.PetDead = false;
                state.PetProbe = false;
                state.PetFailures = 0;
                state.PetRetryAtMs = 0;
                if (spell == kMendPet)
                    state.AutoShotTarget = 0; // the channel stops Auto Shot: it is cast again afterwards
                break;
            case BotActionStatus::kRefused:
            case BotActionStatus::kInvalidTarget:
            case BotActionStatus::kNoLineOfSight:
                if (spell == kCallPet)
                {
                    // Call Pet is refused for a dead pet (or for no pet at all, which Revive Pet's refusal then shows).
                    state.PetDead = true;
                    state.PetMissing = true;
                }
                else if (spell == kRevivePet)
                {
                    state.PetDead = false; // maybe it was alive after all, or there is no pet: Call Pet is tried again after the backoff
                    state.PetMissing = true;
                    state.TamePetFirst = false; // neither Call Pet nor Revive Pet finds a stored pet: the refusal of Tame Beast had another cause
                    ++state.PetFailures;
                    std::uint64_t wait = kPetBackoffMs;
                    for (std::uint32_t i = 1; i < state.PetFailures && wait < kPetBackoffMaxMs; ++i)
                        wait *= 2;
                    state.PetRetryAtMs = nowMs + std::min(wait, kPetBackoffMaxMs);
                }
                else
                {
                    state.PetMissing = true; // Mend Pet has no pet to heal
                    state.PetProbe = false;
                }
                break;
            case BotActionStatus::kNotKnown:
            case BotActionStatus::kUnknownSpell:
                state.TamePetFirst = false; // the spell is not learned: nothing to call or revive
                break;
            default:
                break; // busy, moving, cooldown, no power, out of range: the usual retry below, the pet state is unchanged
        }
    }
    if (spell == kAutoShot && bot.last_action_status == BotActionStatus::kApplied)
    {
        auto const shot = state.CastTarget.find(spell);
        if (shot != state.CastTarget.end())
            state.AutoShotTarget = shot->second;
    }
    if (bot.last_action_status == BotActionStatus::kInvalidTarget)
    {
        // The cast had no valid target: that unit is avoided too, so the next cast goes to another one.
        auto const cast = state.CastTarget.find(spell);
        if (cast != state.CastTarget.end() && cast->second != 0)
            AvoidUnit(state.AvoidUntilMs, state.AvoidCount, cast->second, nowMs);
    }
    if (bot.last_action_status == BotActionStatus::kNoLineOfSight)
    {
        auto const cast = state.CastTarget.find(spell);
        // A healer's heal on a party member that it cannot see (319 refusals on the 2026-10-29 Onyxia run, with the healers holding
        // still at the Warder stand-off): it is allowed to move for a few seconds to find the member again (TurnMoveSetup's
        // healLosPoint, TurnMoveChoice's "heal_los" branch).
        bool const healOnAlly = cast != state.CastTarget.end() && cast->second != 0 && forever_module::UnitRefType(cast->second) == kPlayerType &&
                                state.Role_ != BotState::Role::Dps && HealSpellFor(ClassFor(_options, bot.guid), bot.level) != 0;
        if (healOnAlly)
        {
            state.HealLosTarget = cast->second;
            state.HealLosUntilMs = nowMs + kHealLosMoveMs;
        }
        else
            // Walls between the bot and its own attack target: close in rather than cast through them. Not set for the heal-on-ally
            // case above: CloserUntilMs feeds MoveToTarget's approach distance, which is the bot's own attack target, not the ally it
            // was trying to heal; setting it there used to pull a line-of-sight-blocked healer toward the enemy it was fighting
            // instead of toward the member it could not see (2026-11-10 report, "a healer's recovery walks toward the enemy").
            state.CloserUntilMs = nowMs + kCloserAfterLosMs;
        if (cast != state.CastTarget.end() && cast->second != 0 &&
            ClassFor(_options, bot.guid) == 5 && (spell == 585 || spell == 589 || spell == 8092))
        {
            state.LosRepositionTarget = cast->second;
            state.Last.reset();
            if (_options.OnEvent)
                _options.OnEvent("healer_dps_los_refused: healer=" + std::to_string(bot.guid) + " spell=" +
                                 std::to_string(spell) + " target=" + std::to_string(cast->second));
        }
        if (++state.LosRefusals >= kLosRefusalsAvoid && cast != state.CastTarget.end() && cast->second != 0)
        {
            AvoidUnit(state.AvoidUntilMs, state.AvoidCount, cast->second, nowMs); // still no sight after closing in: another target
            state.LosRefusals = 0;
            state.LosRepositionTarget = 0;
        }
    }
    else if (bot.last_action_status != BotActionStatus::kBusy && bot.last_action_status != BotActionStatus::kMoving)
        state.LosRefusals = 0;
    if (bot.last_action_status == BotActionStatus::kOutOfRange)
        state.RangeCloserUntilMs = nowMs + kCloserAfterRangeMs; // the snapshot's distance was short of the core's: stand closer for a while
    if (bot.last_action_status == BotActionStatus::kApplied)
    {
        // The target of the cast as sent: 0 for a spell on the bot itself (a self buff such as a seal).
        auto const cast = state.CastTarget.find(spell);
        if (cast != state.CastTarget.end())
        {
            std::uint64_t const duration = DurationMsOf(ClassFor(_options, bot.guid), spell, SpecFor(bot.guid, ClassFor(_options, bot.guid)));
            if (duration != 0)
            {
                state.Dots[spell] = Dot{ cast->second, nowMs + duration };
                state.BuffUntilMs[{ spell, cast->second }] = nowMs + duration;
            }

            if (std::uint32_t const gained = ComboPointsGained(spell); gained != 0)
            {
                // A builder on a new target starts the count again.
                state.ComboPoints = (state.ComboTarget == cast->second) ? std::min(state.ComboPoints + gained, kMaxComboPoints) : gained;
                state.ComboTarget = cast->second;
            }
        }
        if (spell == kEviscerate || spell == kRupture || spell == kSliceAndDice || spell == kFerociousBite)
            state.ComboPoints = 0; // a finisher spends every combo point
    }

    // An upkeep buff (Battle Shout, Rockbiter Weapon, Lightning Shield...) is cast out of combat, nobody waits for it, and it was retried as fast as a fight
    // spell: about 20,000 refused casts in 20 minutes on 300 bots. Refused buffs wait longer (see kBuffPowerRetryMs and kBuffRefusedRetryMs).
    bool buff = false;
    bool const castFailed = bot.last_action_status != BotActionStatus::kApplied && bot.last_action_status != BotActionStatus::kNone &&
                            bot.last_action_status != BotActionStatus::kNotOnline;
    if (castFailed)
        for (rotation::Row const& row : RowsFor(ClassFor(_options, bot.guid), SpecFor(bot.guid, ClassFor(_options, bot.guid))))
            buff = buff || (row.Buff && row.Spell == spell);
    if (buff)
    {
        // The buff claim is made when the cast is sent (so a party mate deciding a moment later does not cast it too), for the whole length of the buff.
        // A cast the core refused never gave the buff: the claim goes, or nobody (this bot included) would try it again until the buff would have ended.
        auto const cast = state.CastTarget.find(spell);
        std::uint64_t const recipient = cast != state.CastTarget.end() ? cast->second : 0;
        _buffClaims.erase({ spell, recipient });
        if (recipient == 0)
            _buffClaims.erase({ spell, PlayerRef(bot.guid) });
    }

    // A spell that keeps failing for a reason that is not a fight's business (an upkeep buff on a target that is not valid, in range or in sight, a buff on cooldown, a
    // spell the bot has not learned) waits twice as long after each answer in a row: 5 s up to ten minutes (a spell not known: a minute up to ten). Heard once per
    // answer, since the snapshot repeats the last one. Mark of the Wild, Thorns, Arcane Intellect and Power Word: Fortitude were refused 30,000 times in two hours.
    auto const failWait = [&](std::uint64_t base) -> std::uint64_t
    {
        std::uint32_t& streak = state.SpellFails[spell];
        if (bot.last_action_sequence != state.SpellFailSeq)
        {
            state.SpellFailSeq = bot.last_action_sequence;
            streak = std::min<std::uint32_t>(streak + 1, 12);
        }
        std::uint64_t wait = base;
        for (std::uint32_t i = 1; i < streak && wait < kSpellFailMaxMs; ++i)
            wait *= 2;
        return std::min(wait, kSpellFailMaxMs);
    };
    bool const wasApplied = bot.last_action_status == BotActionStatus::kApplied;
    if (wasApplied)
        state.SpellFails.erase(spell);
    switch (bot.last_action_status)
    {
        case BotActionStatus::kNotKnown:
        case BotActionStatus::kUnknownSpell:
            state.RetryAtMs[spell] = nowMs + failWait(kNotKnownRetryMs);
            if (spell == kTameBeast)
                state.TameNotKnown = true;
            break;
        case BotActionStatus::kOnCooldown:
            state.RetryAtMs[spell] = nowMs + (buff ? failWait(kBuffFailBaseMs) : kCooldownRetryMs);
            state.NextCastMs = std::max(state.NextCastMs, nowMs + kHeldCastMs); // usually the global cooldown: no other spell meanwhile
            break;
        case BotActionStatus::kBusy:
            state.RetryAtMs[spell] = nowMs + kBusyRetryMs; // another cast is in progress: soon, not after the long retry
            state.NextCastMs = std::max(state.NextCastMs, nowMs + kBusyRetryMs * 2); // and no other spell is tried meanwhile
            break;
        case BotActionStatus::kMoving:
            state.CastTimeSpells.insert(spell);            // remembered: the bot stops before this spell from now on
            state.RetryAtMs[spell] = nowMs + kBusyRetryMs;
            break;
        case BotActionStatus::kNotEnoughPower:
            // Out of combat a warrior has no rage at all and a caster is waiting to drink, so the buff has to wait.
            state.RetryAtMs[spell] = nowMs + (buff ? kBuffPowerRetryMs : kPowerRetryMs);
            break;
        case BotActionStatus::kRefused:
            state.RetryAtMs[spell] = nowMs + (buff ? failWait(kBuffRefusedRetryMs) : kRetryMs);
            // A silent refusal (result 32) is the global cooldown too. A refused pet cast is the answer to "is there a pet": no hold, the
            // follow-up (Revive Pet after Call Pet) goes at once.
            if (spell != kCallPet && spell != kRevivePet && spell != kMendPet)
                state.NextCastMs = std::max(state.NextCastMs, nowMs + kHeldCastMs);
            break;
        case BotActionStatus::kOutOfRange:
        case BotActionStatus::kNoLineOfSight:
        case BotActionStatus::kInvalidTarget:
            state.RetryAtMs[spell] = nowMs + (buff ? failWait(kBuffFailBaseMs) : kRetryMs);
            break;
        case BotActionStatus::kApplied:
        case BotActionStatus::kNone:
        case BotActionStatus::kNotOnline:
            break;
    }
}
