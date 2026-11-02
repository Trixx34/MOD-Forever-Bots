// SPDX-License-Identifier: GPL-2.0-or-later
// BotPolicy::Decide, a bot's turn: quests, the pull hold, targets, crowd control, taming, the fight watchdog and the movement set-up (the steps DecideBot runs in order; bot_policy.cpp).
#include "bot_policy_internal.h"

void BotPolicy::TurnQuestsAndHold(DecideTick& tick, BotTurn& turn)
{
    WorldTickSnapshotView const& view = tick.view;
    std::uint64_t nowMs = tick.nowMs;
    auto const& indexOf = tick.indexOf;
    auto& membersOf = tick.membersOf;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto const& stagingHold = turn.stagingHold;
    auto const& lowHealth = turn.lowHealth;
    auto const& classId = turn.classId;
    auto const& partyLeader = turn.partyLeader;
    auto const& partyLeaderBot = turn.partyLeaderBot;
    auto const& partyMember = turn.partyMember;
    auto const& routeOn = turn.routeOn;
    PolicyOptions::ScenarioRoute const& scenario = *turn.scenario;
    auto const& returningToDungeon = turn.returningToDungeon;
    auto const& deadWaitKind = turn.deadWaitKind;

    // Quests (M5): the planner's step for this bot. A party member follows its leader instead, and a hurt or fighting bot
    // leaves quests alone until it is done.
    auto& questStep = turn.questStep;
    turn.routeBusy = false;
    auto& routeBusy = turn.routeBusy;
    bool const questsAllowed = _quests.Enabled() && !partyMember && !stagingHold && !(IsRouteActive(bot.guid) && scenario.RouteMap != 0) /* a dungeon run: the leader walks the route, not to a quest giver */ && !lowHealth && bot.in_combat == 0 && !state.ChatStay && state.ChatFollow == 0 && nowMs >= state.QuestPausedUntilMs;
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
    auto const& noEntries = turn.noEntries;
    turn.huntEntries = &(questStep.Kind == QuestStepKind::Hunt ? questStep.Entries : noEntries);
    std::vector<std::uint32_t> const& huntEntries = *turn.huntEntries;

    // ==== Target acquisition and pulling ====
    // The target is the threat target (even when it is out of view), otherwise the closest live hostile. A member with
    // no threat of its own fights its leader's target, when that is in view.
    // Dungeon pull control (the route is active): the leader pulls only when its party is ready, the others never pull and join a
    // fight once the tank has held it for a moment. A bot already in combat defends itself whatever the rule says.
    turn.dungeonHold = false;
    auto& dungeonHold = turn.dungeonHold;
    turn.tankHeadStart = false;
    auto& tankHeadStart = turn.tankHeadStart;
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
}

void BotPolicy::TurnTargeting(DecideTick& tick, BotTurn& turn)
{
    WorldTickSnapshotView const& view = tick.view;
    std::uint64_t nowMs = tick.nowMs;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto& nearby = turn.nearby;
    auto const& classId = turn.classId;
    auto const& partyLeader = turn.partyLeader;
    auto const& partyMember = turn.partyMember;
    auto const& routeOn = turn.routeOn;
    PolicyOptions::ScenarioRoute const& scenario = *turn.scenario;
    auto const& returningToDungeon = turn.returningToDungeon;
    auto& leaderSnap = turn.leaderSnap;
    auto& tankHeadStart = turn.tankHeadStart;

    turn.targetUnit = nullptr;
    auto& targetUnit = turn.targetUnit;
    turn.targetRef = bot.threat_target_guid;
    auto& targetRef = turn.targetRef;
    if (targetRef != 0 && turn.Avoided(targetRef))
        targetRef = 0; // it refused us: the bot does not fight it for now
    if (returningToDungeon)
        targetRef = 0; // nothing on the way back is worth a fight: a bot that stopped to clear the road died outside the dungeon
    // Chat orders: a passive or staying bot starts no fight of its own; a pull order points the bot at the speaker's target.
    bool groupFighting = false;
    if (bot.group_leader_guid != 0)
        for (BotSnapshot const& other : view.bots)
            groupFighting = groupFighting || (other.group_leader_guid == bot.group_leader_guid && other.in_combat != 0 && other.life_state == 0);
    if (partyLeader != 0 && PartyFighting(tick, partyLeader))
        groupFighting = true;
    turn.noSelfPull = (state.Stance_ == BotState::Stance::Passive || state.ChatStay ||
                             (state.Stance_ == BotState::Stance::Defensive && !groupFighting && state.Role_ != BotState::Role::Tank)) &&
                            bot.in_combat == 0;
    auto const& noSelfPull = turn.noSelfPull;
    if (state.Focus != 0 && (bot.in_combat != 0 || groupFighting))
    {
        if (FindLiveMob(nearby, state.Focus))
        {
            state.FocusMissingMs = 0;
            if (!turn.Avoided(state.Focus))
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
        else if (targetRef == 0 && !turn.Avoided(state.ForcedTarget))
            targetRef = state.ForcedTarget;
    }
    if (targetRef == 0 && partyMember && leaderSnap && leaderSnap->threat_target_guid != 0 && !tankHeadStart && !noSelfPull &&
        !turn.Avoided(leaderSnap->threat_target_guid))
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
            bool const interruptNow = _options.TacticInterrupt && canInterrupt && !InterruptTaken(tick, bot, partyLeader);
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
                std::set<std::uint64_t> const engaged = PartyEngagedTargets(tick, partyLeader);
                bool const atEngagementCap = engaged.size() >= kMaxDungeonEngaged;
                auto const reachable = [&](NearbyUnit const& unit)
                {
                    bool const specialTier = (interruptNow && unit.casting != 0) || isSummonerToKill(unit) || rankOf(unit) != 1000;
                    return specialTier || engaged.count(unit.guid) != 0 || !atEngagementCap;
                };
                for (NearbyUnit const& unit : nearby)
                    if (unit.is_hostile && unit.health != 0 && IsLiveMob(unit) && !turn.Avoided(unit.guid) && unit.guid != assist->guid &&
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
}

BotPolicy::TurnFlow BotPolicy::TurnCrowdControl(DecideTick& tick, BotTurn& turn)
{
    std::uint64_t nowMs = tick.nowMs;
    std::vector<BotAction>& actions = tick.actions;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto& nearby = turn.nearby;
    auto const& classId = turn.classId;
    auto const& partyLeader = turn.partyLeader;
    auto const& partyMember = turn.partyMember;
    auto const& routeOn = turn.routeOn;
    PolicyOptions::ScenarioRoute const& scenario = *turn.scenario;
    auto& leaderSnap = turn.leaderSnap;
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
                    tick.ForParty(partyLeader, [&](BotSnapshot const& member)
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
                            std::set<std::uint64_t> const engaged = PartyEngagedTargets(tick, partyLeader);
                            for (NearbyUnit const& unit : nearby)
                                if (unit.is_hostile && unit.health != 0 && IsLiveMob(unit) && !turn.Avoided(unit.guid) && unit.elite == 0 &&
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
                    return TurnFlow::kSkip;
                }
            }
        }
    }
    return TurnFlow::kNext;
}

BotPolicy::TurnFlow BotPolicy::TurnTaming(DecideTick& tick, BotTurn& turn)
{
    std::uint64_t nowMs = tick.nowMs;
    std::vector<BotAction>& actions = tick.actions;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto const& stagingHold = turn.stagingHold;
    auto& nearby = turn.nearby;
    auto const& lowHealth = turn.lowHealth;
    auto const& classId = turn.classId;
    auto const& routeOn = turn.routeOn;
    std::vector<std::uint32_t> const& huntEntries = *turn.huntEntries;
    auto& dungeonHold = turn.dungeonHold;
    auto& targetUnit = turn.targetUnit;
    auto& targetRef = turn.targetRef;
    auto const& noSelfPull = turn.noSelfPull;
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
                 : PullTarget(bot, nearby, turn.AvoidedFn(), bot.in_combat != 0, huntEntries, routeOn))
    {
        targetUnit = hostile;
        targetRef = hostile->guid;
    }
    turn.hasTarget = targetRef != 0 && !lowHealth;
    auto& hasTarget = turn.hasTarget;

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
                return TurnFlow::kSkip; // still channelling: stand still, send nothing
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
                return TurnFlow::kSkip;
            }
            else if (bot.moving != 0)
            {
                BotAction stop;
                stop.bot_guid = bot.guid;
                stop.type = ActionType::kStop;
                actions.push_back(stop);
                state.Last = Decision{ ActionType::kStop, 0 };
                state.Approaching = false;
                return TurnFlow::kSkip;
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
                return TurnFlow::kSkip;
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
            return TurnFlow::kSkip;
        }
    }
    return TurnFlow::kNext;
}

void BotPolicy::TurnFightWatch(DecideTick& tick, BotTurn& turn)
{
    std::uint64_t nowMs = tick.nowMs;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto& targetUnit = turn.targetUnit;
    auto& targetRef = turn.targetRef;
    auto& hasTarget = turn.hasTarget;

    // ==== Loot tracking and fight watchdog ====
    // The target of this fight is looted once it is dead: remember it, and forget the ones too old to still be on the ground.
    if (hasTarget && forever_module::UnitRefType(targetRef) == 8)
        state.FoughtAtMs[targetRef] = nowMs;
    for (auto it = state.FoughtAtMs.begin(); it != state.FoughtAtMs.end();)
        it = nowMs - it->second > kLootWindowMs ? state.FoughtAtMs.erase(it) : std::next(it);
    for (auto it = state.LootedUntilMs.begin(); it != state.LootedUntilMs.end();)
        it = nowMs >= it->second ? state.LootedUntilMs.erase(it) : std::next(it);
    turn.distance = targetUnit ? rotation::DistanceYards(bot, *targetUnit) : bot.target_distance;
    auto const& distance = turn.distance;

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
}

void BotPolicy::TurnMoveSetup(DecideTick& tick, BotTurn& turn)
{
    WorldTickSnapshotView const& view = tick.view;
    std::uint64_t nowMs = tick.nowMs;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto& nearby = turn.nearby;
    auto const& partyMember = turn.partyMember;

    auto& wanted = turn.wanted;
    turn.moveWhy = ""; // which part of the policy chose a kMoveTo (logged for a party in a dungeon run, move_why)
    auto& moveWhy = turn.moveWhy;
    turn.kiteRetry = -1;       // the AoeKitePoint retry that picked a kite move: 0 is the entry retreat (with an entry), more is the circle (move_why)
    auto& kiteRetry = turn.kiteRetry;
    // An Onyxian Warder close to a party member (not its leader, the tank): the nearest one in view that is alive, and where a healer may step to see a member it could not heal.
    // Not tied to the bot's own in_combat: the 2026-10-29 pull left the non-tank bots out of combat for 55 s, standing 5 yards from the Warder.
    turn.aoeWarder = nullptr;
    auto& aoeWarder = turn.aoeWarder;
    auto& healLosPoint = turn.healLosPoint;
    auto& kiteEntry = turn.kiteEntry;   // the entry to retreat toward (AoeKitePoint)
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
}
