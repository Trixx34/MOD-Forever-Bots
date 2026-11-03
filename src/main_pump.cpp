// SPDX-License-Identifier: GPL-2.0-or-later
// forever-bots: the Tier 2 pump thread (PumpTier2), split out of main.cpp.
#include "main_internal.h"

void main_detail::PumpTier2(forever_module::Tier2Session& session, std::atomic<bool> const& running, PolicyOptions options,
                         std::string const& telemetryPath, BotSettings const& settings, PumpExtras extras)
{
    BotPolicy policy(options);
    std::uint64_t dungeonGeneration = extras.Dungeons ? extras.Dungeons->Generation.load() : 0;
    // One director per party of the scenario (ScenarioParties): the bots, in guid order, are cut into parties of the roster's size, the same cut the policy makes, and a
    // director sees only its own party's bots. With several parties each result and log line carries the party number ("dm#2").
    // With several dungeons the parties go in list order (the first dungeon's first) and are numbered per dungeon: "dm#1".."dm#10", "rfc#1".."rfc#10".
    std::vector<ScenarioDirector> directors;
    std::vector<std::string> directorNames;
    std::vector<std::size_t> dungeonOf;                     // director (party) index -> index in extras.Scenarios
    std::vector<ScenarioPhase> lastPhases;
    std::unordered_map<std::uint64_t, std::size_t> partyOf; // bot guid -> party index (recomputed each tick)
    std::size_t const partySize = extras.Scenarios.empty() ? 1 : std::max<std::size_t>(1, extras.Scenarios.front().Plan.RosterSize);
    std::uint32_t scenarioParties = 0;
    for (PumpExtras::ScenarioDungeon const& dungeon : extras.Scenarios)
        scenarioParties += std::max<std::uint32_t>(1, dungeon.Parties);
    for (std::size_t d = 0; d < extras.Scenarios.size(); ++d)
        for (std::uint32_t k = 0; k < std::max<std::uint32_t>(1, extras.Scenarios[d].Parties); ++k)
        {
            ScenarioPlan plan = extras.Scenarios[d].Plan;
            if (scenarioParties > 1)
                plan.Name += "#" + std::to_string(k + 1);
            directorNames.push_back(plan.Name);
            dungeonOf.push_back(d);
            directors.emplace_back(std::move(plan));
            lastPhases.push_back(ScenarioPhase::Done);
        }
    std::vector<bool> phaseLogged(directors.size(), false);
    bool sawSnapshot = false;
    std::uint64_t lastTick = 0;
    std::uint64_t presenceFedMs = 0;
    std::uint64_t auctionFedMs = 0;
    std::uint64_t errandFedMs = 0;
    // Last action sequence seen per bot, so each result is reported once.
    std::unordered_map<std::uint64_t, std::uint32_t> seenSequence;
    std::unordered_map<std::uint64_t, std::uint64_t> serviceNoticeMs; // bot -> when a service refusal was last logged
    std::unordered_set<std::uint32_t> unknownSpellsReported; // spell ids the core does not know, reported once each
    // Ambient chat (AmbientChat, off by default): the line pool is read once here; a missing or empty file turns it off.
    std::optional<ambient::Speaker> ambientSpeaker;
    std::unordered_map<std::uint64_t, std::uint32_t> ambientLevel; // bot guid -> the level last seen, for the level-up line
    std::uint64_t ambientMs = 0;                                    // when the last ambient line was scheduled
    ambient::DeathWatch ambientDeaths;                              // a bot that saw another bot die says its local_death line
    ambient::BossWatch ambientBosses;                               // a dungeon party that killed a boss says its boss_kill line
    std::size_t ambientCursor = 0;                                  // the bot the next ambient line is tried for
    std::uint32_t ambientTurn = 0;                                  // alternates General and Trade when both are on
    if (settings.AmbientChat)
    {
        std::string const path = BesideTheModule(settings.AmbientFile);
        std::ifstream file(path);
        std::size_t errors = 0;
        std::vector<ambient::LineDef> lines = ambient::ParseLines(file, errors);
        std::cerr << "forever-bots: ambient chat: " << lines.size() << " line(s) from " << path << ", " << errors << " bad\n";
        if (lines.empty())
            std::cerr << "forever-bots: ambient chat: no lines, off\n";
        else
        {
            ambient::Limits limits;
            limits.PerMinute = settings.AmbientPerMin;
            limits.BotCooldownMs = std::uint64_t(settings.AmbientBotCooldownSec) * 1000;
            ambientSpeaker.emplace(std::move(lines), limits);
        }
    }

    // Timing of the policy, reported once a minute so the cost at the current bot count is in the log.
    std::uint64_t reportedAt = nowMs();
    std::uint64_t ticks = 0;
    std::uint64_t decideTotalUs = 0;
    std::uint64_t decideMaxUs = 0;
    std::uint64_t actionsTotal = 0;
    std::map<std::tuple<std::uint64_t, unsigned, std::uint32_t>, std::uint64_t> actionStats; // ActionStats: actions sent this minute, by bot, type and spell
    std::uint64_t actionStatsAtMs = nowMs();
    std::size_t lastBotCount = 0;

    // The telemetry file is appended to, so a restart adds to the same record. It rotates at TelemetryMaxMB.
    std::unique_ptr<RotatingFile> telemetryFile;
    std::unique_ptr<BotTelemetry> telemetry;
    if (!telemetryPath.empty())
    {
        telemetryFile = std::make_unique<RotatingFile>(telemetryPath,
                                                       std::uint64_t(settings.TelemetryMaxMB) * 1024 * 1024,
                                                       settings.TelemetryKeep);
        if (telemetryFile->IsOpen())
        {
            if (telemetryFile->CurrentBytes() == 0)
                telemetryFile->WriteLine(BotTelemetry::Header());
            BotTelemetry::Options telemetryOptions;
            telemetryOptions.SummaryEveryMs = std::uint64_t(settings.TelemetrySummarySec) * 1000;
            telemetryOptions.StallAfterMs = std::uint64_t(settings.StallAfterMinutes) * 60000;
            telemetryOptions.ClassOf = options.ClassOf;
            telemetryOptions.RaceOf = options.RaceOf;
            telemetry = std::make_unique<BotTelemetry>(
                [file = telemetryFile.get()](std::string const& line) { file->WriteLine(line); }, telemetryOptions);
            std::cerr << "forever-bots: telemetry to " << telemetryPath << " (summary every " << settings.TelemetrySummarySec
                      << " s, rotated at " << settings.TelemetryMaxMB << " MB, keeping " << settings.TelemetryKeep << ")\n";
        }
        else
        {
            std::cerr << "forever-bots: cannot open telemetry file " << telemetryPath << "; telemetry is off\n";
        }
    }

    // The live picture for the dashboard's map (LiveFile): rewritten in place, so nothing piles up.
    BotLive live(BotLive::Options{ settings.LiveFile, settings.LiveEveryMs, 400 });
    if (live.Enabled())
        std::cerr << "forever-bots: live picture to " << settings.LiveFile << " every " << settings.LiveEveryMs << " ms\n";

    move_gate::MoveGate moveGate;
    while (running)
    {
        forever_module::WorldTickSnapshotView const& view = session.ReadLatestSnapshot();

        // Report what the core did with each action: a refusal is logged with its reason, once per action.
        for (forever_module::BotSnapshot const& bot : view.bots)
        {
            auto seen = seenSequence.find(bot.guid);
            if (bot.last_action_sequence == 0 || (seen != seenSequence.end() && seen->second == bot.last_action_sequence))
                continue;
            seenSequence[bot.guid] = bot.last_action_sequence;
            // A spell id the core does not know is a mistake in the rotation tables: say so once per id.
            if (bot.last_action_status == forever_module::BotActionStatus::kUnknownSpell && bot.last_action_spell != 0)
            {
                if (unknownSpellsReported.insert(bot.last_action_spell).second)
                    std::cerr << "forever-bots: spell " << bot.last_action_spell << " is not in the core's spell table; check the rotation id\n";
                continue;
            }
            if (bot.last_action_status != forever_module::BotActionStatus::kApplied)
            {
                // A vendor or trainer is refused whenever no NPC is in range, the normal state of an idle bot, and a craft or a gather whenever there is nothing to
                // make or gather. Those are logged at most once per bot every ten minutes, so 1500 bots do not flood the log.
                bool const service = bot.last_action_type == static_cast<std::uint8_t>(forever_module::ActionType::kVendor) ||
                                     bot.last_action_type == static_cast<std::uint8_t>(forever_module::ActionType::kTrain) ||
                                     bot.last_action_type == static_cast<std::uint8_t>(forever_module::ActionType::kSkillUp) ||
                                     bot.last_action_type == static_cast<std::uint8_t>(forever_module::ActionType::kGather) ||
                                     bot.last_action_type == static_cast<std::uint8_t>(forever_module::ActionType::kCraft);
                if (service)
                {
                    std::uint64_t const now = nowMs();
                    auto const last = serviceNoticeMs.find(bot.guid);
                    if (last != serviceNoticeMs.end() && now - last->second < kServiceNoticeMs)
                        continue;
                    serviceNoticeMs[bot.guid] = now;
                }
                std::cerr << "forever-bots: bot " << bot.guid << " " << ActionTypeName(static_cast<forever_module::ActionType>(bot.last_action_type))
                          << (bot.last_action_spell ? " spell " + std::to_string(bot.last_action_spell) : std::string())
                          << " not done: " << ActionStatusName(bot.last_action_status) << "\n";
            }
        }

        if (!sawSnapshot || view.tick_number != lastTick)
        {
            sawSnapshot = true;
            lastTick = view.tick_number;

            if (telemetry)
                telemetry->Observe(view, nowMs());
            if (live.Enabled())
                live.Maybe(view, nowMs(), std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::system_clock::now().time_since_epoch()).count()));

            // One publish per snapshot tick: the core applies the newest action set it sees for each tick.
            auto const started = std::chrono::steady_clock::now();
            // The config was delivered again (a core reload): read the dungeon files again, and let "tank lead" use the result.
            if (extras.Dungeons && extras.Dungeons->Generation.load() != dungeonGeneration)
            {
                dungeonGeneration = extras.Dungeons->Generation.load();
                DungeonSources sources;
                {
                    std::lock_guard<std::mutex> lock(extras.Dungeons->Lock);
                    sources = extras.Dungeons->Sources;
                }
                std::vector<std::string> reloadWarnings;
                DungeonSet const set = LoadDungeonSet(sources, reloadWarnings);
                for (std::string const& warning : reloadWarnings)
                    std::cerr << "forever-bots: dungeons: " << warning << "\n";
                for (std::string const& line : set.Log)
                    std::cerr << "forever-bots: " << line << "\n";
                policy.SetDungeons(DungeonRoutes(set.Dungeons));
                std::cerr << "forever-bots: dungeon files reloaded: tank lead knows " << set.Dungeons.size() << " dungeon(s)\n";
            }
            // .autogear commands: the bots led by the player who typed it are dressed at this tick.
            if (extras.Commands)
            {
                std::vector<std::uint64_t> issuers;
                {
                    std::lock_guard<std::mutex> lock(extras.Commands->Lock);
                    issuers.swap(extras.Commands->AutoGear);
                }
                std::vector<std::uint64_t> gearBots;
                {
                    std::lock_guard<std::mutex> lock(extras.Commands->Lock);
                    gearBots.swap(extras.Commands->GearBots);
                }
                for (std::uint64_t bot : gearBots)
                    policy.RequestBagGear(bot);
                for (std::uint64_t issuer : issuers)
                {
                    std::uint64_t const ref = forever_module::MakeUnitRef(2, 0, issuer & forever_module::kUnitRefCounterMask);
                    for (forever_module::BotSnapshot const& bot : view.bots)
                        if (bot.group_leader_guid == ref)
                            policy.RequestAutoGear(bot.guid);
                }
            }
            // Chat commands: applied before this tick's decisions; the lines for the speaker go back through the Run() thread.
            if (extras.Commands)
            {
                std::vector<CommandQueue::ErrandItem> errands;
                {
                    std::lock_guard<std::mutex> lock(extras.Commands->Lock);
                    errands.swap(extras.Commands->Errands);
                }
                for (CommandQueue::ErrandItem const& item : errands)
                {
                    if (item.Cancel)
                        policy.CancelErrand(item.Guid);
                    else
                        policy.StartErrand(item.Guid, item.Order);
                }
            }
            if (extras.Commands)
            {
                std::vector<CommandQueue::ChatItem> chat;
                {
                    std::lock_guard<std::mutex> lock(extras.Commands->Lock);
                    chat.swap(extras.Commands->Chat);
                }
                for (CommandQueue::ChatItem const& item : chat)
                {
                    std::vector<ChatAck> acks;
                    std::vector<std::string> const lines = policy.ApplyChat(view, item.Context, item.Command, nowMs(), &acks);
                    std::lock_guard<std::mutex> lock(extras.Commands->Lock);
                    for (std::string const& line : lines)
                        extras.Commands->Replies.emplace_back(item.Context.Issuer, line);
                    for (ChatAck& ack : acks)
                        if (extras.Commands->Says.size() < 64)
                            extras.Commands->Says.push_back(std::move(ack));
                }
            }
            // Ambient chat: a bot that gained a level gets its level line; otherwise one bot a second, alive and out of combat, may say an
            // ambient line. The Speaker decides which line and whether the rate allows it. The lines go to the Run() thread to send.
            if (ambientSpeaker && extras.Commands)
            {
                std::uint64_t const at = nowMs();
                auto const contextOf = [&options](forever_module::BotSnapshot const& bot, std::string event)
                {
                    ambient::Context ctx;
                    ctx.Level = bot.level;
                    ctx.Zone = ambient::ZoneName(bot.map_id);
                    std::uint32_t classId = options.ClassId;
                    if (options.ClassOf)
                        if (std::uint32_t const own = options.ClassOf(bot.guid))
                            classId = own;
                    ctx.Class = ambient::ClassName(classId);
                    ctx.Event = std::move(event);
                    return ctx;
                };
                auto const channelFor = [&settings](std::uint32_t turn) -> ambient::Channel
                {
                    if (settings.AmbientChannels == 3)
                        return turn % 2 ? ambient::Channel::Trade : ambient::Channel::General;
                    return settings.AmbientChannels == 2 ? ambient::Channel::Trade : ambient::Channel::General;
                };
                std::vector<CommandQueue::AmbientLine> picks;
                for (forever_module::BotSnapshot const& bot : view.bots)
                {
                    auto const seen = ambientLevel.find(bot.guid);
                    bool const gained = seen != ambientLevel.end() && bot.level > seen->second;
                    ambientLevel[bot.guid] = bot.level;
                    if (!gained || bot.life_state != 0)
                        continue;
                    ambient::Channel const chan = channelFor(ambientTurn++);
                    if (auto line = ambientSpeaker->Pick(bot.guid, chan, contextOf(bot, "level"), at))
                        picks.push_back({ bot.guid, chan == ambient::Channel::Trade, std::move(*line) });
                }
                {
                    // World events: a death seen by a nearby bot, and a boss killed by a dungeon party. One line at most per event; the Speaker keeps its rate limits.
                    std::vector<ambient::LifeSample> samples;
                    samples.reserve(view.bots.size());
                    std::unordered_map<std::uint64_t, forever_module::BotSnapshot const*> byGuid;
                    for (forever_module::BotSnapshot const& bot : view.bots)
                    {
                        samples.push_back({ bot.guid, bot.map_id, bot.life_state, bot.in_combat != 0 });
                        byGuid[bot.guid] = &bot;
                    }
                    for (std::uint64_t guid : ambientDeaths.Observe(samples))
                    {
                        ambient::Channel const chan = channelFor(ambientTurn++);
                        if (auto line = ambientSpeaker->Pick(guid, chan, contextOf(*byGuid[guid], ambient::kEventLocalDeath), at))
                            picks.push_back({ guid, chan == ambient::Channel::Trade, std::move(*line) });
                    }
                    for (std::size_t party = 0; party < directors.size(); ++party)
                    {
                        if (!ambientBosses.Grew(party, directors[party].KilledBossEntries().size()))
                            continue;
                        for (forever_module::BotSnapshot const& bot : view.bots)
                        {
                            auto const member = partyOf.find(bot.guid);
                            if (member == partyOf.end() || member->second != party || bot.life_state != 0)
                                continue;
                            ambient::Channel const chan = channelFor(ambientTurn++);
                            if (auto line = ambientSpeaker->Pick(bot.guid, chan, contextOf(bot, ambient::kEventBossKill), at))
                            {
                                picks.push_back({ bot.guid, chan == ambient::Channel::Trade, std::move(*line) });
                                break; // one bot of the party speaks
                            }
                        }
                    }
                }
                if (!view.bots.empty() && at - ambientMs >= 1000)
                {
                    ambientMs = at;
                    forever_module::BotSnapshot const& bot = view.bots[ambientCursor++ % view.bots.size()];
                    if (bot.life_state == 0 && bot.in_combat == 0)
                    {
                        ambient::Channel const chan = channelFor(ambientTurn++);
                        if (auto line = ambientSpeaker->Pick(bot.guid, chan, contextOf(bot, ""), at))
                            picks.push_back({ bot.guid, chan == ambient::Channel::Trade, std::move(*line) });
                    }
                }
                if (!picks.empty())
                {
                    std::lock_guard<std::mutex> lock(extras.Commands->Lock);
                    for (CommandQueue::AmbientLine& line : picks)
                        if (extras.Commands->Ambient.size() < 64)
                            extras.Commands->Ambient.push_back(std::move(line));
                }
            }
            {
                std::vector<std::uint64_t> guids;
                guids.reserve(view.bots.size());
                for (forever_module::BotSnapshot const& bot : view.bots)
                    guids.push_back(bot.guid);
                std::sort(guids.begin(), guids.end());
                partyOf.clear();
                for (std::size_t i = 0; i < guids.size(); ++i)
                    partyOf[guids[i]] = std::min(i / partySize, directors.size() - 1);
                policy.SetRouteActiveFor([&directors, &partyOf](std::uint64_t guid)
                {
                    auto const at = partyOf.find(guid);
                    return at != partyOf.end() && directors[at->second].RouteActive();
                });
                policy.SetTamingFor([&directors, &partyOf](std::uint64_t guid)
                {
                    auto const at = partyOf.find(guid);
                    return at != partyOf.end() && directors[at->second].Taming();
                });
                policy.SetStagingHoldFor([&directors, &partyOf](std::uint64_t guid)
                {
                    auto const at = partyOf.find(guid);
                    return at != partyOf.end() && directors[at->second].Staging();
                });
                policy.SetKilledBossesFor([&directors, &partyOf](std::uint64_t guid) -> std::set<std::uint32_t> const&
                {
                    static std::set<std::uint32_t> const none;
                    auto const at = partyOf.find(guid);
                    return at == partyOf.end() ? none : directors[at->second].KilledBossEntries();
                });
                // Several dungeons: a bot walks, fights and goes back in by the route of its own party's dungeon, from the staging place on (the portal it walks
                // back to is that dungeon's, not the first one's).
                if (extras.Scenarios.size() > 1)
                    policy.SetScenarioRouteFor([&extras, &dungeonOf, &partyOf](std::uint64_t guid) -> PolicyOptions::ScenarioRoute const*
                    {
                        auto const at = partyOf.find(guid);
                        return at == partyOf.end() ? nullptr : &extras.Scenarios[dungeonOf[at->second]].Route;
                    });
            }
            std::vector<forever_module::BotAction> actions = policy.Decide(view, nowMs());
            // Talent respecs (TalentRespec): one kRespecTalents per bot, spec in spell_id and role in dest_x; the core does the reset and the spend.
            for (BotPolicy::TalentOrder const& order : policy.TakeRespecOrders())
            {
                if (actions.size() >= session.MaxActions())
                    break;
                forever_module::BotAction respec;
                respec.bot_guid = order.Bot;
                respec.type = forever_module::ActionType::kRespecTalents;
                respec.spell_id = order.Spec;
                respec.dest_x = static_cast<float>(order.Role);
                actions.push_back(respec);
            }
            if (extras.Commands)
            {
                // Whispers from the Run() thread: the policy decides the answers (and the refusals it queued); busy bots are shared back for trade requests.
                std::vector<CommandQueue::WhisperItem> whispers;
                {
                    std::lock_guard<std::mutex> lock(extras.Commands->Lock);
                    whispers.swap(extras.Commands->Whispers);
                }
                // Dungeon finder queue events (LfgQueueFill): the policy picks an idle bot for a missing role.
                std::vector<lfg_queue_fill::Queue> lfgQueues;
                {
                    std::lock_guard<std::mutex> lock(extras.Commands->Lock);
                    lfgQueues.swap(extras.Commands->LfgQueues);
                }
                std::vector<advisor::GuildLevel> guildLevels;
                {
                    std::lock_guard<std::mutex> lock(extras.Commands->Lock);
                    guildLevels.swap(extras.Commands->GuildLevels);
                }
                std::vector<std::uint64_t> declines;
                {
                    std::lock_guard<std::mutex> lock(extras.Commands->Lock);
                    declines.swap(extras.Commands->QuestDeclines);
                }
                // A declined invite: the player's unit reference (type 2, a player) keeps QuestInvite from inviting it again for two hours.
                for (std::uint64_t const decliner : declines)
                    policy.NoteQuestInviteDeclined(forever_module::MakeUnitRef(2, 0, decliner), nowMs());
                for (advisor::GuildLevel const& level : guildLevels)
                    policy.OnGuildMemberLevel(view, level.Guild, level.Member, level.Level, nowMs());
                for (lfg_queue_fill::Queue const& queue : lfgQueues)
                    policy.OnLfgQueue(view, queue, nowMs());
                std::vector<BotPolicy::WhisperReply> answers;
                for (CommandQueue::WhisperItem const& item : whispers)
                {
                    std::vector<BotPolicy::WhisperReply> made = policy.ApplyWhisper(view, item.Player, item.Recipient, item.Text, nowMs());
                    answers.insert(answers.end(), made.begin(), made.end());
                }
                std::vector<BotPolicy::WhisperReply> queued = policy.TakeWhisperReplies();
                answers.insert(answers.end(), queued.begin(), queued.end());
                std::vector<std::uint64_t> const busy = policy.BusyBots(view);
                {
                    std::lock_guard<std::mutex> lock(extras.Commands->Lock);
                    extras.Commands->BusyBots.clear();
                    extras.Commands->BusyBots.insert(busy.begin(), busy.end());
                    for (BotPolicy::WhisperReply& answer : answers)
                        if (extras.Commands->WhisperReplies.size() < 64)
                            extras.Commands->WhisperReplies.push_back(std::move(answer));
                }
                std::vector<BotPolicy::TalentOrder> talentOrders = policy.TakeTalentOrders();
                std::lock_guard<std::mutex> lock(extras.Commands->Lock);
                for (std::uint64_t const bot : extras.Commands->TalentRefused)
                    policy.TalentOrderFailed(bot, nowMs());
                extras.Commands->TalentRefused.clear();
                // Duels, battlegrounds, guild and gifts: what the core answered goes to the policy; the new orders go to the Run() thread to send.
                for (social::Outcome const& outcome : extras.Commands->SocialOutcomes)
                    policy.NoteSocialOutcome(outcome, nowMs());
                extras.Commands->SocialOutcomes.clear();
                for (auto const& [bot, player] : extras.Commands->GiftThanks)
                    policy.NoteGiftRecipient(bot, player);
                extras.Commands->GiftThanks.clear();
                for (advisor::Order& order : policy.TakeAdvisorOrders())
                    if (extras.Commands->Advisor.size() < 64)
                        extras.Commands->Advisor.push_back(std::move(order));
                for (social::Order const& order : policy.TakeSocialOrders())
                    if (extras.Commands->Social.size() < 64)
                        extras.Commands->Social.push_back(order);
                for (BotPolicy::TalentOrder const& order : talentOrders)
                    if (extras.Commands->Talents.size() < 256)
                        extras.Commands->Talents.push_back(order);
                // LFG fill offers: whispers to the human leader (ChatAck::Player set), sent with the chat lines below.
                std::vector<ChatAck> offers = policy.TakeWhispers();
                for (ChatAck& offer : offers)
                    if (extras.Commands->Says.size() < 64)
                        extras.Commands->Says.push_back(std::move(offer));
            }
            if (extras.Presence && nowMs() - presenceFedMs >= kPresenceFeedMs)
            {
                // Ordinary bots only: alts and party members are never logged off and are not in the list.
                presenceFedMs = nowMs();
                std::vector<presence::Online> list;
                list.reserve(view.bots.size());
                for (forever_module::BotSnapshot const& bot : view.bots)
                {
                    if ((extras.Alts && extras.Alts->Has(bot.guid)) || bot.group_role != 0)
                        continue;
                    list.push_back({ bot.guid, policy.SafeToLogOut(bot), bot.level });
                }
                std::lock_guard<std::mutex> lock(extras.Presence->Lock);
                for (presence::Online const& online : list)
                    extras.Presence->Levels[online.Guid] = online.Level;
                extras.Presence->Bots.swap(list);
                extras.Presence->AtMs = presenceFedMs;
            }
            if (extras.Errand && nowMs() - errandFedMs >= kPresenceFeedMs)
            {
                errandFedMs = nowMs();
                std::vector<ErrandBot> list;
                list.reserve(view.bots.size());
                std::vector<std::pair<std::uint64_t, ErrandOutcome>> outcomes;
                for (forever_module::BotSnapshot const& bot : view.bots)
                {
                    if (ErrandOutcome const outcome = policy.TakeErrandOutcome(bot.guid); outcome != ErrandOutcome::kNone)
                        outcomes.emplace_back(bot.guid, outcome);
                    if ((extras.Alts && extras.Alts->Has(bot.guid)) || bot.life_state != 0)
                        continue;
                    ErrandBot entry;
                    entry.Guid = bot.guid;
                    entry.Available = bot.in_combat == 0 && bot.in_flight == 0;
                    entry.Called = bot.group_role != 0 || bot.group_leader_guid != 0;
                    int const slot = errand::TrainingDueSlot(bot.profession_skill, bot.profession_value, bot.profession_max);
                    entry.TrainDue = slot >= 0;
                    entry.Skill = slot >= 0 ? bot.profession_skill[slot] : 0;
                    entry.Side = options.SideOf ? options.SideOf(bot.guid) : 0;
                    entry.Map = bot.map_id;
                    entry.X = bot.pos_x;
                    entry.Y = bot.pos_y;
                    entry.Z = bot.pos_z;
                    entry.AuctioneerValid = bot.auctioneer_valid != 0;
                    entry.AuctioneerX = bot.auctioneer_x;
                    entry.AuctioneerY = bot.auctioneer_y;
                    entry.AuctioneerZ = bot.auctioneer_z;
                    list.push_back(entry);
                }
                std::lock_guard<std::mutex> lock(extras.Errand->Lock);
                extras.Errand->Bots.swap(list);
                extras.Errand->AtMs = errandFedMs;
                for (auto& outcome : outcomes)
                    if (extras.Errand->Outcomes.size() < 256)
                        extras.Errand->Outcomes.push_back(outcome);
            }
            if (extras.Auction && nowMs() - auctionFedMs >= kPresenceFeedMs)
            {
                auctionFedMs = nowMs();
                std::vector<AuctionBot> list;
                list.reserve(view.bots.size());
                for (forever_module::BotSnapshot const& bot : view.bots)
                {
                    if ((extras.Alts && extras.Alts->Has(bot.guid)) || bot.group_role != 0)
                        continue;
                    list.push_back({ bot.guid, bot.bag_free_slots, bot.auctioneer_guid, bot.life_state == 0 && bot.in_combat == 0, bot.copper, bot.level });
                }
                std::lock_guard<std::mutex> lock(extras.Auction->Lock);
                extras.Auction->Bots.swap(list);
                extras.Auction->AtMs = auctionFedMs;
            }
            for (std::size_t k = 0; k < directors.size(); ++k)
            {
                // The director of party k sees the bots of its party only.
                forever_module::WorldTickSnapshotView const partyView = SubsetView(view, [&partyOf, k](std::uint64_t guid)
                {
                    auto const at = partyOf.find(guid);
                    return at != partyOf.end() && at->second == k;
                });
                ScenarioDirector& director = directors[k];
                std::string const& name = directorNames[k];
                std::size_t const before = actions.size();
                director.Decide(partyView, nowMs(), actions);

                // The scenario's progress, in the module's log: each phase as it starts, and every teleport the director sends.
                if (!phaseLogged[k] || director.Phase() != lastPhases[k])
                {
                    static char const* const names[] = { "waiting for the roster", "dressing", "taming", "grouping", "entering", "running", "done" };
                    std::cerr << "forever-bots: scenario " << name << ": phase " << names[int(director.Phase())] << " ("
                              << partyView.bots.size() << " bots in view)\n";
                    lastPhases[k] = director.Phase();
                    phaseLogged[k] = true;
                }
                for (std::size_t i = before; i < actions.size(); ++i)
                    if (actions[i].type == forever_module::ActionType::kTeleport)
                        std::cerr << "forever-bots: scenario " << name << ": teleport bot " << actions[i].bot_guid << " to map "
                                  << actions[i].spell_id << " (" << actions[i].dest_x << ", " << actions[i].dest_y << ", " << actions[i].dest_z << ")\n";
                for (std::string const& boss : director.TakeNewBossKills())
                    std::cerr << "forever-bots: scenario " << name << ": boss killed: " << boss << "\n";
                if (auto const result = director.TakeResult())
                {
                    std::string const line = FormatScenarioResult(*result);
                    std::cerr << "forever-bots: " << line << "\n";
                    if (!extras.ScenarioResultFile.empty())
                    {
                        std::ofstream out(extras.ScenarioResultFile, std::ios::app);
                        if (out)
                            out << line << "\n";
                    }
                }
            }
            auto const decideUs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                                 std::chrono::steady_clock::now() - started)
                                                                 .count());
            ++ticks;
            decideTotalUs += decideUs;
            decideMaxUs = std::max(decideMaxUs, decideUs);
            if (extras.Sell)
                AppendSales(*extras.Sell, settings, view, actions, session.MaxActions(), nowMs(), extras.Alts);
            // Every source's moves pass the gate: a move to a place the core just found no route to is not sent again before the pause is over.
            for (forever_module::BotSnapshot const& bot : view.bots)
                moveGate.Observe(bot.guid, bot.last_action_type, bot.last_action_status, bot.last_action_sequence, nowMs());
            moveGate.Filter(actions, nowMs());
            actionsTotal += actions.size();
            if (settings.ActionStats)
            {
                for (forever_module::BotAction const& action : actions)
                    ++actionStats[std::make_tuple(action.bot_guid, static_cast<unsigned>(action.type), action.spell_id)];
                if (nowMs() - actionStatsAtMs >= 60000)
                {
                    std::vector<std::pair<std::uint64_t, std::tuple<std::uint64_t, unsigned, std::uint32_t>>> top;
                    for (auto const& [key, count] : actionStats)
                        top.emplace_back(count, key);
                    std::sort(top.begin(), top.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
                    for (std::size_t i = 0; i < top.size() && i < 10; ++i)
                        std::cerr << "forever-bots: actions_top: bot=" << std::get<0>(top[i].second) << " type=" << std::get<1>(top[i].second)
                                  << " spell=" << std::get<2>(top[i].second) << " count=" << top[i].first << "\n";
                    actionStats.clear();
                    actionStatsAtMs = nowMs();
                }
            }
            lastBotCount = view.bots.size();

            std::string error;
            if (!session.PublishActions(std::move(actions), error))
                std::cerr << "forever-bots: could not publish actions: " << error << "\n";
        }

        std::uint64_t const now = nowMs();
        if (now - reportedAt >= kPumpReportMs && ticks > 0)
        {
            std::cerr << "forever-bots: pump: " << lastBotCount << " bots, decide avg " << decideTotalUs / ticks << " us, max "
                      << decideMaxUs << " us over " << ticks << " ticks, " << ActionsPerTick(actionsTotal, ticks) << " actions per tick\n";
            reportedAt = now;
            ticks = 0;
            decideTotalUs = 0;
            decideMaxUs = 0;
            actionsTotal = 0;
        }
        std::this_thread::sleep_for(kPumpInterval);
    }
}
