// SPDX-License-Identifier: GPL-2.0-or-later
// forever-bots: asks the core for bots, then drives the ones it controls from Tier 2 snapshots.
//
// Usage: forever-bots <tier1-endpoint>. The core passes the endpoint when it spawns the module.
//
// Two loops run. Run() (the Tier 1 loop) handles control replies, revocations and the spawn ramp on this thread. A pump
// thread reads each new Tier 2 snapshot and publishes the bot actions BotPolicy decides on. The pump touches only the
// Tier 2 region and the ClassTable (locked), so the two loops share no other state.
#include "main_internal.h"

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: forever-bots <tier1-endpoint>\n";
        return 1;
    }

    forever_module::ModuleManifest manifest;
    manifest.module_name = "forever-bots";
    manifest.module_version = FOREVER_MODULE_VERSION; // from module.json, see CMakeLists.txt
    manifest.tier2_participates = true;
    manifest.requested_max_bots = kMaxBotsCeiling;
    manifest.requested_max_nearby_units = kRequestedNearbyUnits;
    manifest.requested_max_actions = kRequestedActions;
    manifest.tier1_hooks = { "CommandScript", "PlayerScript.OnChat", "PlayerScript.OnBotTrade", "PlayerScript.OnLootItem" };

    forever_module::ModuleClient client(std::move(manifest));
    if (!client.Connect(argv[1]) || !client.Handshake())
    {
        std::cerr << "forever-bots: could not connect to the core: " << client.rejection_reason() << "\n";
        return 1;
    }

    // Tier 2 is granted at handshake: there is a region only if the core accepted it.
    bool const tier2Granted = !client.tier2_region_name().empty();
    std::uint32_t const grantedBots = client.negotiated_bounds().max_bots;

    // Config is delivered right after the handshake, so the first callback has already run when OnConfig returns. A later
    // re-delivery (a core reload) updates the ramp, the random picks and the cap, on the Run() thread. The telemetry file
    // and the test spell take effect at restart.
    // Ordering note: settings/roster/alts/commands below are captured by reference into the OnConfig/OnControlRevoked/
    // OnRequest callbacks registered further down this function. Each callback's captures must outlive the ModuleClient
    // (they do: client, settings, roster and the shared_ptr registries are all declared in this same main() frame and
    // destructed in reverse order after client, so there is no dangling-reference risk), but it does mean a callback must
    // not be registered before the state it captures is constructed. Keep new captured state declared above its callback.
    BotSettings settings;
    bool configured = false;
    BotRoster roster(0); // the cap is set once the settings are known
    auto const dungeonBox = std::make_shared<DungeonReloadBox>();
    client.OnConfig([&settings, &configured, &roster, tier2Granted, grantedBots, dungeonBox](
                        std::unordered_map<std::string, std::string> const& config)
    {
        std::vector<std::string> warnings;
        // A dungeon file fills the scenario keys the config leaves out (dungeon_data.h).
        BotSettings const parsed = ParseBotSettings(MergeDungeonFile(config, warnings), warnings);
        for (std::string const& warning : warnings)
            std::cerr << "forever-bots: config: " << warning << "\n";

        if (!configured)
        {
            settings = parsed;
            configured = true;
            return;
        }

        {
            // the dungeon files are read again by the pump (doc/dungeon-strategy.md, "Reloading")
            std::lock_guard<std::mutex> lock(dungeonBox->Lock);
            dungeonBox->Sources = SourcesFromConfig(config);
        }
        dungeonBox->Generation.fetch_add(1);
        settings.MaxBots = parsed.MaxBots;
        settings.Race = parsed.Race;
        settings.Class = parsed.Class;
        settings.Ramp = parsed.Ramp;
        roster.SetMaxBots(EffectiveMaxBots(parsed.MaxBots, grantedBots, tier2Granted));
        std::cerr << "forever-bots: config reloaded: MaxBots " << roster.MaxBots() << ", ramp " << settings.Ramp.Initial << " +"
                  << settings.Ramp.Step << " every " << settings.Ramp.StepMinutes << " min to " << settings.Ramp.Cap
                  << " (owned " << roster.Owned().size() << ")\n";
    });

    // The bots we may control: MaxBots from config, capped at what the core granted for Tier 2. Without a Tier 2 region
    // there is nothing to drive, so no bots are requested. The core enforces the same bound (ModuleBotOwnership).
    std::uint32_t const maxBots = EffectiveMaxBots(settings.MaxBots, grantedBots, tier2Granted);
    if (maxBots != settings.MaxBots)
    {
        std::cerr << "forever-bots: MaxBots " << settings.MaxBots << " limited to " << maxBots << " (Tier 2 grant "
                  << grantedBots << (tier2Granted ? "" : ", no Tier 2 region") << ")\n";
    }
    roster.SetMaxBots(maxBots);

    pick::ClassTable classes;
    pick::ClassTable races;
    RunContext run{ client, roster, classes, races, settings, pick::Stream(settings.RandomSeed), nowMs(), LocalSecondsOfDay() };
    std::cerr << "forever-bots: random seed " << settings.RandomSeed << ", race " << settings.Race << ", class "
              << settings.Class << " (0 = random), ramp " << settings.Ramp.Initial << " +" << settings.Ramp.Step << " every "
              << settings.Ramp.StepMinutes << " min to " << settings.Ramp.Cap << ", stop at " << settings.Ramp.StopSecOfDay / 3600
              << ":" << (settings.Ramp.StopSecOfDay / 60) % 60 << " local\n";

    auto alts = std::make_shared<AltRegistry>();
    client.OnControlRevoked([&roster, alts](std::uint64_t botGuid, std::string const& reason)
    {
        {
            std::lock_guard<std::mutex> lock(alts->Lock);
            auto const it = alts->ByGuid.find(botGuid);
            if (it != alts->ByGuid.end())
            {
                alts->ByName.erase(it->second);
                alts->ByGuid.erase(it);
            }
        }
        if (roster.OnRevoked(botGuid, reason))
            std::cerr << "forever-bots: control of bot " << botGuid << " revoked: " << reason << "\n";
    });

    // .autogear: a player in a party types it (as a chat command) and the bots that player leads are dressed for their level.
    auto commands = std::make_shared<CommandQueue>();
    client.OnRequest("CommandScript", [commands, alts, &run, &settings](forever_module::EventRequest const& request) -> forever_module::EventReply
    {
        forever_module::EventReply reply;
        std::string text = request.command().command_text();
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        // .bots <verb>: the chat commands (chat_commands.h) as a dot command, for the bots of the player's group. goto here, pull and heal need
        // the speaker's position or target, which a dot command does not carry: those are for party chat.
        if (text == "bots" || text.rfind("bots ", 0) == 0)
        {
            std::string const rest = text.size() > 5 ? text.substr(5) : std::string();
            auto const setReply = [&reply](bool ok, std::string const& message)
            {
                reply.mutable_command()->set_success(ok);
                reply.mutable_command()->set_result_text(message);
            };
            // .bots alt add <name> | alt remove <name> | alt list, .bots spawn <race> <class> [level], .bots list. The answer comes back as a message
            // to the player when the core has decided.
            std::uint64_t const issuerCounter = request.command().issuer_guid() & forever_module::kUnitRefCounterMask;
            auto const commandWords = [&rest]
            {
                std::vector<std::string> out;
                std::istringstream in(rest);
                std::string word;
                while (in >> word)
                    out.push_back(word);
                return out;
            }();
            auto const tellIssuer = [&run, issuerCounter](std::string const& text) { run.Client.SendPlayerMessage(issuerCounter, text); };
            // alt/spawn/list/autogear below are always available regardless of ChatOrders/ChatControl: they manage which
            // characters play as bots at all (fleet administration), not in-fight orders to bots already playing, so they
            // are intentionally not gated the same way ParseChatCommand's verbs are a few lines down.
            if (!commandWords.empty() && commandWords[0] == "alt")
            {
                if (commandWords.size() == 2 && commandWords[1] == "list")
                {
                    std::lock_guard<std::mutex> lock(alts->Lock);
                    std::string names;
                    for (auto const& [name, guid] : alts->ByName)
                        names += (names.empty() ? "" : ", ") + name;
                    setReply(true, names.empty() ? "No alts are playing as bots." : "Alts as bots: " + names);
                    return reply;
                }
                if (commandWords.size() >= 3 && commandWords[1] == "add")
                {
                    std::string const name = JoinWords(commandWords, 2);
                    // An alt does not use up the bot cap, so it is not refused for it (BotRoster::OnAltGranted).
                    forever_module::SpawnParams params;
                    params.set_alt_name(name);
                    params.set_owner_guid(issuerCounter);
                    run.Client.RequestControl(0, params, [&run, alts, tellIssuer, name](forever_module::ControlResponse const& response)
                    {
                        if (!response.granted())
                        {
                            tellIssuer("Cannot make " + name + " a bot: " + response.reason());
                            return;
                        }
                        std::uint64_t const guid = response.bot_guid();
                        run.Roster.OnAltGranted(guid);
                        if (response.class_id() != 0)
                            run.Classes.Set(guid, response.class_id());
                        if (response.race() != 0)
                            run.Races.Set(guid, response.race());
                        {
                            std::lock_guard<std::mutex> lock(alts->Lock);
                            alts->ByName[LowerCopy(name)] = guid;
                            alts->ByGuid[guid] = LowerCopy(name);
                        }
                        std::cerr << "forever-bots: alt " << name << " is bot " << guid << "\n";
                        tellIssuer(name + " is logging in as a bot. Invite it to your group to command it.");
                    });
                    setReply(true, "Asking the server to log " + name + " in as a bot.");
                    return reply;
                }
                if (commandWords.size() >= 3 && commandWords[1] == "remove")
                {
                    std::string const name = JoinWords(commandWords, 2);
                    std::uint64_t guid = 0;
                    {
                        std::lock_guard<std::mutex> lock(alts->Lock);
                        auto const it = alts->ByName.find(LowerCopy(name));
                        if (it != alts->ByName.end())
                        {
                            guid = it->second;
                            alts->ByGuid.erase(guid);
                            alts->ByName.erase(it);
                        }
                    }
                    if (guid == 0)
                    {
                        setReply(false, name + " is not an alt playing as a bot.");
                        return reply;
                    }
                    run.Roster.OnRevoked(guid, "alt removed");
                    run.Client.ReleaseControl(guid); // the core logs an alt out when the module lets it go
                    setReply(true, name + " is being logged out.");
                    return reply;
                }
                setReply(false, "Usage: .bots alt add <name> | alt remove <name> | alt list");
                return reply;
            }
            if (!commandWords.empty() && commandWords[0] == "spawn")
            {
                std::uint32_t race = 0, cls = 0, level = 0;
                auto const number = [](std::string const& text, std::uint32_t& out)
                {
                    return std::from_chars(text.data(), text.data() + text.size(), out).ec == std::errc{};
                };
                if (commandWords.size() < 3 || commandWords.size() > 4 || !number(commandWords[1], race) || !number(commandWords[2], cls) || (commandWords.size() == 4 && !number(commandWords[3], level)) ||
                    race == 0 || cls == 0)
                {
                    setReply(false, "Usage: .bots spawn <race id> <class id> [level]");
                    return reply;
                }
                if (!run.Roster.CanRequest())
                {
                    setReply(false, "The module cannot take another bot (its bot limit is reached).");
                    return reply;
                }
                forever_module::SpawnParams params = ParamsFor(RaceClass{ race, cls });
                if (level != 0)
                    params.set_level(level);
                run.Roster.NoteRequestSent(0);
                run.Client.RequestControl(0, params, [&run, tellIssuer, race, cls](forever_module::ControlResponse const& response)
                {
                    if (!response.granted())
                    {
                        run.Roster.OnDenied(0);
                        tellIssuer("Spawn refused: " + response.reason());
                        return;
                    }
                    std::uint64_t const guid = response.bot_guid();
                    run.Roster.OnGranted(0, guid);
                    run.Classes.Set(guid, cls);
                    run.Races.Set(guid, race);
                    run.Client.StoreSet(bot_store::KeyFor(guid), std::to_string(race) + " " + std::to_string(cls));
                    tellIssuer("Spawned bot " + std::to_string(guid) + " (race " + std::to_string(race) + ", class " + std::to_string(cls) + ").");
                });
                setReply(true, "Spawning a bot.");
                return reply;
            }
            if (commandWords.size() == 1 && commandWords[0] == "list")
            {
                setReply(true, "This module controls " + std::to_string(run.Roster.Owned().size()) + " bot(s), " + std::to_string(run.Roster.PendingCount()) +
                              " request(s) pending (limit " + std::to_string(run.Roster.MaxBots()) + ").");
                return reply;
            }
            if (rest == "autogear")
            {
                {
                    std::lock_guard<std::mutex> lock(commands->Lock);
                    commands->AutoGear.push_back(request.command().issuer_guid());
                }
                setReply(true, "Your party's bots are being dressed for their level.");
                return reply;
            }
            if (!settings.ChatCommands)
            {
                setReply(false, "Bot commands are off (ChatCommands = 0 in the module's config).");
                return reply;
            }
            if (rest == "help")
            {
                std::string text = "Bot commands ([selector] verb [args], selector optional):";
                for (std::string const& line : ChatHelpLines(settings.ChatOrders, settings.ChatControl))
                    text += "\n" + line;
                text += "\nAlso: .bots list | autogear | alt add|remove|list <name> | spawn <race> <class> [level].";
                setReply(true, text);
                return reply;
            }
            auto const parsed = ParseChatCommand(rest, settings.ChatOrders, settings.ChatControl);
            if (!parsed)
            {
                setReply(false, "Usage: .bots [all|tank|healer|dps|<class>] follow [off] | stay [off] | release | status | autogear" +
                               std::string(settings.ChatOrders ? " | stop | passive | aggressive | summon | revive | mount | dismount" : "") +
                               std::string(settings.ChatControl ? " | role <tank|healer|dps|auto> | stance <aggressive|defensive|passive> | distance <yards>|default" : "") +
                               ". goto here, pull, heal, focus and train: say them in party chat.");
                return reply;
            }
            if (parsed->Verb == ChatVerb::GotoHere || parsed->Verb == ChatVerb::Pull || parsed->Verb == ChatVerb::Heal ||
                parsed->Verb == ChatVerb::Focus || parsed->Verb == ChatVerb::Train)
            {
                setReply(false, std::string(ChatVerbName(parsed->Verb)) + " needs your position or target: say it in party chat.");
                return reply;
            }
            CommandQueue::ChatItem item;
            item.Command = *parsed;
            item.Context.Issuer = request.command().issuer_guid() & forever_module::kUnitRefCounterMask;
            item.Context.Orders = settings.ChatOrders;
            item.Context.Control = settings.ChatControl;
            // The party-chat path below (OnNotify, PlayerChatEvent) sets these from the same settings; the dot
            // command skipped them, so a bot that took a ".bots" order never spoke its ChatSay confirmation line.
            item.Context.Say = settings.ChatSay;
            item.Context.SayMax = settings.ChatSayMax;
            {
                std::lock_guard<std::mutex> lock(commands->Lock);
                if (commands->Chat.size() < 64)
                    commands->Chat.push_back(std::move(item));
            }
            setReply(true, "Sent to your group's bots.");
            return reply;
        }
        if (text != "autogear")
        {
            reply.mutable_command()->set_success(false); // not ours: no text, so the core tries the next module
            return reply;
        }
        {
            std::lock_guard<std::mutex> lock(commands->Lock);
            commands->AutoGear.push_back(request.command().issuer_guid());
        }
        reply.mutable_command()->set_success(true);
        reply.mutable_command()->set_result_text("Your party's bots are being dressed for their level.");
        return reply;
    });

    // Bot trade (trade_policy.h). Everything below runs on the client's thread (notify callbacks, timers and action replies), so the TradeBook
    // needs no lock. The core only sends the events of bots this module controls, and only when Bot.Trade.Enabled is on there.
    auto trades = std::make_shared<trade::TradeBook>();
    auto runTrade = std::make_shared<std::function<void(std::vector<trade::Step> const&)>>();
    *runTrade = [&client, trades, runTrade, commands](std::vector<trade::Step> const& steps)
    {
        using forever_module::ActionReply;
        using forever_module::ModuleClient;
        for (trade::Step const& step : steps)
        {
            switch (step.What)
            {
                case trade::Step::Kind::Begin:
                    client.BotTrade(step.Bot, ModuleClient::BotTradeOp::kBegin);
                    break;
                case trade::Step::Kind::ListBags:
                {
                    std::uint64_t const bot = step.Bot;
                    client.BotTrade(bot, ModuleClient::BotTradeOp::kListBags, 0, 0, 0, [trades, runTrade, bot](ActionReply const& reply)
                    {
                        std::vector<trade::BagItem> items;
                        if (reply.success())
                            for (auto const& item : reply.bag_items())
                                items.push_back({ item.item_guid(), item.entry(), item.count(), item.name() });
                        (*runTrade)(trades->OnBagList(bot, std::move(items), nowMs()));
                    });
                    break;
                }
                case trade::Step::Kind::Whisper:
                    client.BotSpeak(step.Bot, ModuleClient::SpeakKind::kWhisper, step.Partner, step.Text);
                    break;
                case trade::Step::Kind::SetItem:
                {
                    std::uint64_t const bot = step.Bot;
                    std::string const name = step.Text;
                    client.BotTrade(bot, ModuleClient::BotTradeOp::kSetItem, step.Slot, step.ItemGuid, 0, [trades, runTrade, bot, name](ActionReply const& reply)
                    {
                        if (!reply.success())
                            (*runTrade)(trades->OnItemRefused(bot, name));
                    });
                    break;
                }
                case trade::Step::Kind::Accept:
                    client.BotTrade(step.Bot, ModuleClient::BotTradeOp::kAccept);
                    break;
                case trade::Step::Kind::Cancel:
                    client.BotTrade(step.Bot, ModuleClient::BotTradeOp::kCancel);
                    break;
                case trade::Step::Kind::Regear:
                {
                    // the pump thread asks the policy for a bag-only gear pass: the core wears what the bot carries when it is better
                    std::lock_guard<std::mutex> lock(commands->Lock);
                    commands->GearBots.push_back(step.Bot);
                    break;
                }
            }
        }
    };
    client.OnNotify("PlayerScript.OnBotTrade", [trades, runTrade](forever_module::EventNotify const& notify)
    {
        if (notify.event_payload_case() != forever_module::EventNotify::kBotTrade)
            return;
        forever_module::BotTradeEvent const& event = notify.bot_trade();
        if (event.status() > static_cast<std::uint32_t>(trade::Status::Completed))
            return;
        (*runTrade)(trades->OnEvent(event.bot_guid(), event.partner_guid(), static_cast<trade::Status>(event.status()), nowMs()));
    });
    // A trade nobody is working on is cancelled.
    client.AddTimer(std::chrono::seconds(5), [trades, runTrade] { (*runTrade)(trades->OnIdle(nowMs())); });

    // Chat commands (chat_commands.h): a group leader's party or raid line, or a whisper to a bot. Parsed here, applied on the pump thread.
    client.OnNotify("PlayerScript.OnChat", [commands, &settings, trades, runTrade](forever_module::EventNotify const& notify)
    {
        if (notify.event_payload_case() == forever_module::EventNotify::kPlayerChat)
        {
            // A whisper to a bot that is trading with the speaker is about the trade (what it asks for), not a chat order.
            forever_module::PlayerChatEvent const& whisper = notify.player_chat();
            if (whisper.channel() == 3 && trades->PartnerOf(whisper.recipient_guid()) == whisper.player_guid())
            {
                (*runTrade)(trades->OnWhisper(whisper.player_guid(), whisper.recipient_guid(), whisper.text(), nowMs()));
                return;
            }
        }
        if (!settings.ChatCommands || notify.event_payload_case() != forever_module::EventNotify::kPlayerChat)
            return;
        forever_module::PlayerChatEvent const& chat = notify.player_chat();
        auto const parsed = ParseChatCommand(chat.text(), settings.ChatOrders, settings.ChatControl);
        if (!parsed)
            return; // ordinary chat
        CommandQueue::ChatItem item;
        item.Command = *parsed;
        item.Context.Issuer = chat.player_guid();
        item.Context.Recipient = chat.channel() == 3 ? chat.recipient_guid() : 0;
        item.Context.MapId = chat.map_id();
        item.Context.Pos = Point{ chat.pos_x(), chat.pos_y(), chat.pos_z() };
        item.Context.TargetRef = chat.target_ref();
        item.Context.Say = settings.ChatSay;
        item.Context.SayMax = settings.ChatSayMax;
        item.Context.Orders = settings.ChatOrders;
        item.Context.Control = settings.ChatControl;
        std::lock_guard<std::mutex> lock(commands->Lock);
        if (commands->Chat.size() < 64)
            commands->Chat.push_back(std::move(item));
    });
    // The lines the pump thread has for speakers are sent from this thread (client calls belong to it).
    client.AddTimer(std::chrono::milliseconds(500), [commands, &client]
    {
        std::vector<std::pair<std::uint64_t, std::string>> replies;
        std::vector<ChatAck> says;
        std::vector<BotPolicy::TalentOrder> talents;
        {
            std::lock_guard<std::mutex> lock(commands->Lock);
            replies.swap(commands->Replies);
            says.swap(commands->Says);
            talents.swap(commands->Talents);
        }
        for (BotPolicy::TalentOrder const& order : talents)
            client.SpendBotTalents(order.Bot, static_cast<forever_module::ModuleClient::TalentRole>(order.Role), static_cast<forever_module::TalentSpec>(order.Spec), [commands, bot = order.Bot](forever_module::ActionReply const& reply)
            {
                if (!reply.success())
                {
                    std::lock_guard<std::mutex> lock(commands->Lock);
                    commands->TalentRefused.push_back(bot);
                }
            });
        for (auto const& [player, text] : replies)
            client.SendPlayerMessage(player, text);
        for (ChatAck const& say : says)
            client.BotSpeak(say.Bot, forever_module::ModuleClient::SpeakKind::kSay, 0, say.Text);
    });

    // Bots from last run are rows in the module store (bot/<guid>). Each row's value is "race class", read before the
    // bot is requested so the rotation knows its class. A revoke keeps the row, since the core revokes every bot at
    // shutdown; only "character not found" removes one. Spawning for the rest is the ramp's job.
    auto reclaim = [&run](std::uint64_t guid)
    {
        run.Roster.NoteRequestSent(guid);
        run.Client.StoreGet(bot_store::KeyFor(guid), [&run, guid](forever_module::ActionReply const& stored)
        {
            RaceClass combo{};
            if (stored.found() && ParseRaceClass(stored.value(), combo))
                run.Classes.Set(guid, combo.Class), run.Races.Set(guid, combo.Race);
            else
                std::cerr << "forever-bots: bot " << guid << " has no saved class; its rotation is off until it is set\n";

            run.Client.RequestControl(guid, {}, [&run, guid](forever_module::ControlResponse const& reply)
            {
                if (reply.granted())
                {
                    run.Roster.OnGranted(guid, reply.bot_guid());
                    std::cerr << "forever-bots: reclaimed bot " << guid << "\n";
                    return;
                }
                run.Roster.OnDenied(guid);
                if (reply.reason() == "character not found")
                    run.Client.StoreDelete(bot_store::KeyFor(guid));
                std::cerr << "forever-bots: could not reclaim bot " << guid << ": " << reply.reason() << "\n";
            });
        });
    };

    bool const scenarioOn = !settings.Scenarios.empty();
    std::size_t scenarioSpawned = 0;
    if (scenarioOn)
    {
        roster.SetMaxBots(std::min<std::uint32_t>(ScenarioBotCount(settings.Scenarios), maxBots));
        for (ScenarioSettings const& scenario : settings.Scenarios)
            std::cerr << "forever-bots: scenario " << scenario.Name << ": " << scenario.Parties << " part" << (scenario.Parties == 1 ? "y" : "ies") << " of " << scenario.Roster.size() << " bots at level "
                      << scenario.Level << ", route of " << scenario.Route.size() << " point(s), " << scenario.DurationSec << " s" << (scenario.Raid ? " (raid, 8 subgroups)" : "") << "\n";
        if (settings.Scenarios.size() > 1)
            std::cerr << "forever-bots: scenario of " << settings.Scenarios.size() << " dungeons: " << ScenarioPartyCount(settings.Scenarios) << " parties, "
                      << ScenarioBotCount(settings.Scenarios) << " bots\n";
        client.AddTimer(std::chrono::milliseconds(3000), [&run, &scenarioSpawned] { ScenarioSpawnTick(run, scenarioSpawned); });
    }

    if (!scenarioOn)
    client.StoreList(bot_store::KeyPrefix, [&run, &reclaim](forever_module::ActionReply const& reply)
    {
        if (!reply.success())
        {
            std::cerr << "forever-bots: could not read saved bots (" << reply.reason() << "); spawning without reclaim\n";
            return;
        }
        for (int i = 0; i < reply.keys_size(); ++i)
        {
            std::uint64_t guid = 0;
            if (bot_store::GuidFromKey(reply.keys(i), guid) && run.Roster.CanRequest())
                reclaim(guid);
        }
    });

    if (!scenarioOn)
        client.AddTimer(kRampInterval, [&run] { RampTick(run); });

    // The policy's class lookup: a bot's own class, or the configured class (0 = none) for a bot the table does not know.
    PolicyOptions policyOptions;
    policyOptions.TestSpell = settings.TestSpell;
    policyOptions.ClassId = settings.Class;
    policyOptions.ServiceEveryMs = std::uint64_t(settings.ServiceEverySec) * 1000;
    policyOptions.TrainEveryMs = std::uint64_t(settings.TrainEverySec) * 1000;
    policyOptions.Supplies = settings.Supplies;
    policyOptions.DungeonTeleport = settings.DungeonTeleport;
    policyOptions.DungeonReviveOutOfCombat = settings.DungeonReviveOutOfCombat;
    policyOptions.CorpseRunTeleportMs = std::uint64_t(settings.CorpseRunTeleportSec) * 1000;
    policyOptions.AggressiveStart = settings.AggressiveStart;
    policyOptions.Crafting = settings.Crafting;
    policyOptions.MountMinLevel = settings.MountMinLevel;
    policyOptions.TacticFocus = settings.TacticFocus;
    policyOptions.TacticInterrupt = settings.TacticInterrupt;
    policyOptions.TacticHeal = settings.TacticHeal;
    policyOptions.TacticRest = settings.TacticRest;
    policyOptions.TacticRecover = settings.TacticRecover;
    policyOptions.TacticCc = settings.TacticCc;
    policyOptions.DungeonHealBelowPct = settings.DungeonHealBelowPct;
    policyOptions.DungeonTankHealBelowPct = settings.DungeonTankHealBelowPct;
    policyOptions.HealerDpsManaPct = settings.HealerDpsManaPct;
    policyOptions.HealerLosMoveYards = settings.HealerLosMoveYards;
    policyOptions.Gathering = settings.Gathering;
    if (!settings.CraftingFociFile.empty())
    {
        std::ifstream fociFile(settings.CraftingFociFile);
        std::stringstream fociText;
        fociText << fociFile.rdbuf();
        std::vector<std::string> fociErrors;
        auto foci = std::make_shared<std::vector<crafting::FocusPoint>>(crafting::ParseFociTable(fociText.str(), fociErrors));
        for (std::string const& error : fociErrors)
            std::cerr << "forever-bots: CraftingFociFile: " << error << "\n";
        if (!fociFile || foci->empty())
            std::cerr << "forever-bots: CraftingFociFile: cannot read '" << settings.CraftingFociFile << "' (or it holds no object); bots do not walk to a recipe's anvil, forge or fire\n";
        else
            std::cerr << "forever-bots: crafting foci: " << foci->size() << " object(s) from " << settings.CraftingFociFile << "\n";
        policyOptions.Foci = std::move(foci);
    }
    policyOptions.AvoidGroundEffects = settings.AvoidGroundEffects;
    if (settings.AvoidGroundEffects)
    {
        auto avoidTable = std::make_shared<ground_avoid::AvoidTable>();
        if (!settings.AvoidSpellsFile.empty())
        {
            std::ifstream avoidFile(settings.AvoidSpellsFile);
            std::stringstream avoidText;
            avoidText << avoidFile.rdbuf();
            std::vector<std::string> avoidErrors;
            *avoidTable = ground_avoid::ParseAvoidSpells(avoidText.str(), avoidErrors);
            for (std::string const& error : avoidErrors)
                std::cerr << "forever-bots: AvoidSpellsFile: " << error << "\n";
            if (!avoidFile || avoidTable->Size() == 0)
                std::cerr << "forever-bots: AvoidSpellsFile: cannot read '" << settings.AvoidSpellsFile << "' (or it holds no spell); only effects of hostile casters are avoided\n";
            else
                std::cerr << "forever-bots: avoid spells: " << avoidTable->Size() << " spell(s) from " << settings.AvoidSpellsFile << "\n";
        }
        policyOptions.AvoidSpells = std::move(avoidTable);
        std::cerr << "forever-bots: bots step out of harmful ground effects (the core needs Bot.Snapshot.GroundEffects = 1)\n";
    }
    // Dungeon routes for "tank lead" (every dungeon, by the map of its entry) and the events table, shared by all dungeons. They come from the JSON files of
    // DungeonDir (yours) and DungeonDataDir (shipped), and from the old text files for maps without a JSON file (dungeon_files.h).
    DungeonSources dungeonSources;
    dungeonSources.File = settings.DungeonFile;
    dungeonSources.EventsFile = settings.DungeonEventsFile;
    dungeonSources.DataDir = settings.DungeonDataDir;
    dungeonSources.OverrideDir = settings.DungeonDir;
    {
        std::lock_guard<std::mutex> lock(dungeonBox->Lock);
        dungeonBox->Sources = dungeonSources;
    }
    std::vector<dungeon_events::Event> allEvents;
    if (!dungeonSources.File.empty() || !dungeonSources.DataDir.empty() || !dungeonSources.OverrideDir.empty() || !dungeonSources.EventsFile.empty())
    {
        std::vector<std::string> dungeonWarnings;
        DungeonSet const set = LoadDungeonSet(dungeonSources, dungeonWarnings);
        for (std::string const& warning : dungeonWarnings)
            std::cerr << "forever-bots: dungeons: " << warning << "\n";
        for (std::string const& line : set.Log)
            std::cerr << "forever-bots: " << line << "\n";
        allEvents = set.Events;
        policyOptions.DungeonsByMap = DungeonRoutes(set.Dungeons);
        std::cerr << "forever-bots: dungeon events: " << allEvents.size() << "\n";
        std::cerr << "forever-bots: tank lead knows " << policyOptions.DungeonsByMap.size() << " dungeon(s)\n";
    }
    policyOptions.Taxi = settings.Taxi;
    if (settings.Taxi.Enabled)
    {
        // The city named in the taxi journal lines comes from the capitals' trainer table (ErrandTrainerFile); without it every city is "unknown".
        std::ifstream in(settings.ErrandTrainerFile);
        if (in)
        {
            std::ostringstream text;
            text << in.rdbuf();
            std::vector<std::string> ignored;
            policyOptions.TaxiPlaces = std::make_shared<std::vector<taxi::Place>>(errand::CityPlaces(errand::ParseTrainerTable(text.str(), ignored)));
        }
    }
    policyOptions.PartySize = settings.PartySize;
    policyOptions.PartyCount = settings.PartyCount;
    policyOptions.PartyPercent = settings.PartyPercent;
    policyOptions.RandomPct = settings.RandomPct;
    policyOptions.RandomSeed = settings.RandomSeed;
    for (auto const& [mapId, spots] : settings.Hubs)
        for (auto const& spot : spots)
            policyOptions.HubsByMap[mapId].push_back({ spot[0], spot[1], spot[2] });
    if (!settings.QuestFile.empty())
    {
        std::vector<std::string> questWarnings;
        auto quests = std::make_shared<QuestData>(LoadQuestData(settings.QuestFile, questWarnings));
        for (std::string const& warning : questWarnings)
            std::cerr << "forever-bots: " << warning << "\n";
        std::cerr << "forever-bots: quests: " << quests->Starts.size() << " start(s) from " << settings.QuestFile << "\n";
        policyOptions.Quests = std::move(quests);
    }
    policyOptions.QuestTeleport = settings.QuestTeleport;
    if (!settings.TameBeastFile.empty())
    {
        std::vector<std::string> tameWarnings;
        auto beasts = std::make_shared<tame::TameBeasts>(tame::LoadTameBeasts(BesideTheModule(settings.TameBeastFile), tameWarnings));
        for (std::string const& warning : tameWarnings)
            std::cerr << "forever-bots: " << warning << "\n";
        std::cerr << "forever-bots: tame beasts: " << beasts->ByEntry.size() << " from " << BesideTheModule(settings.TameBeastFile) << "\n";
        if (!beasts->ByEntry.empty())
            policyOptions.TameBeasts = std::move(beasts);
    }
    if (!settings.RotationFile.empty())
    {
        std::vector<std::string> rotationWarnings;
        auto rotations = std::make_shared<rotation::RotationData>(rotation::LoadRotationData(BesideTheModule(settings.RotationFile), rotationWarnings));
        for (std::string const& warning : rotationWarnings)
            std::cerr << "forever-bots: " << warning << "\n";
        std::cerr << "forever-bots: rotations for " << rotations->ByClass.size() << " class(es) and " << rotations->BySpec.size() << " spec(s) from " << BesideTheModule(settings.RotationFile) << "\n";
        policyOptions.Rotations = std::move(rotations);
    }
    if (!settings.SpellCostFile.empty())
    {
        std::vector<std::string> costWarnings;
        auto costs = std::make_shared<spell_cost::SpellCostData>(spell_cost::LoadSpellCosts(settings.SpellCostFile, costWarnings));
        for (std::string const& warning : costWarnings)
            std::cerr << "forever-bots: " << warning << "\n";
        std::cerr << "forever-bots: spell costs for " << costs->BySpell.size() << " spell(s) from " << settings.SpellCostFile << "\n";
        policyOptions.SpellCosts = std::move(costs);
    }
    if (!settings.GrindFile.empty())
    {
        std::vector<std::string> grindWarnings;
        auto grind = std::make_shared<GrindData>(LoadGrindData(settings.GrindFile, grindWarnings));
        for (std::string const& warning : grindWarnings)
            std::cerr << "forever-bots: " << warning << "\n";
        std::cerr << "forever-bots: grind spots: " << grind->Spots.size() << " from " << settings.GrindFile << "\n";
        policyOptions.Grind = std::move(grind);
    }
    policyOptions.RaceOf = [&races](std::uint64_t botGuid) { return races.Get(botGuid); };
    policyOptions.SideOf = [&races](std::uint64_t botGuid) { return SideOfRace(races.Get(botGuid)); };
    policyOptions.PartySideOf = [&races](std::uint64_t botGuid) -> std::uint8_t
    {
        std::uint32_t const race = races.Get(botGuid);
        if (race == 95)
            return 1; // Skyborne (Alliance)
        if (race == 96)
            return 2; // Skyborne (Horde)
        return SideOfRace(race);
    };
    policyOptions.ClassOf = [&classes](std::uint64_t botGuid) { return classes.Get(botGuid); };
    policyOptions.SpecOf = [&run](std::uint64_t botGuid) { return run.Specs.Get(botGuid); };
    policyOptions.AutoGearing = settings.AutoGearing;
    policyOptions.AcceptInvites = settings.ChatCommands;
    policyOptions.IsAlt = [alts](std::uint64_t guid) { return alts->Has(guid); };
    policyOptions.OnEvent = [](std::string const& line) { std::cerr << "forever-bots: event " << line << "\n"; };
    policyOptions.AutoGearEveryLevels = settings.AutoGearEveryLevels;
    PumpExtras extras;
    extras.Dungeons = dungeonBox;
    extras.Commands = commands;
    extras.Alts = alts;
    if (settings.Presence.Enabled && !scenarioOn)
    {
        auto feed = std::make_shared<PresenceFeed>();
        extras.Presence = feed;
        auto scheduler = std::make_shared<presence::Scheduler>(settings.Presence, settings.RandomSeed ^ 0x9e3779b97f4a7c15ull);
        client.AddTimer(kPresenceInterval, [&run, feed, scheduler, trades]
        {
            std::vector<presence::Online> list;
            std::uint64_t fedAt = 0;
            {
                std::lock_guard<std::mutex> lock(feed->Lock);
                list = feed->Bots;
                fedAt = feed->AtMs;
            }
            std::uint64_t const now = nowMs();
            if (fedAt == 0 || now - fedAt > kPresenceStaleMs)
                return;
            for (presence::Online& bot : list)
                if (trades->Active(bot.Guid))
                    bot.Idle = false; // a trade in progress is not cut short
            scheduler->SetSettings(run.Settings.Presence);
            presence::Decisions const decisions = scheduler->Tick(now, LocalSecondsOfDay(), list);
            for (std::uint64_t guid : decisions.LogOff)
            {
                run.Roster.NoteOffline(guid);
                run.Client.ReleaseControl(guid); // the core logs it out (Bot.Presence.LogoutOnRelease)
            }
            for (std::uint64_t guid : decisions.LogOn)
            {
                run.Roster.NoteRequestSent(guid);
                run.Client.RequestControl(guid, {}, [&run, scheduler, guid](forever_module::ControlResponse const& reply)
                {
                    if (reply.granted())
                    {
                        run.Roster.OnGranted(guid, reply.bot_guid());
                        return;
                    }
                    run.Roster.OnDenied(guid);
                    if (reply.reason() == "character not found")
                    {
                        scheduler->Forget(guid);
                        run.Roster.ForgetOffline(guid);
                        run.Client.StoreDelete(bot_store::KeyFor(guid));
                    }
                    else
                        scheduler->LogOnFailed(guid, nowMs());
                    std::cerr << "forever-bots: presence: could not bring bot " << guid << " back: " << reply.reason() << "\n";
                });
            }
            if (!decisions.LogOff.empty() || !decisions.LogOn.empty())
                std::cerr << "forever-bots: presence: " << decisions.LogOff.size() << " log off, " << decisions.LogOn.size() << " back; "
                          << run.Roster.OfflineCount() << " away, " << run.Roster.Owned().size() << " online\n";
        });
        std::cerr << "forever-bots: presence is on: sessions " << settings.Presence.OnlineMinMin << " to " << settings.Presence.OnlineMaxMin
                  << " min, away " << settings.Presence.OfflineMinMin << " to " << settings.Presence.OfflineMaxMin << " min\n";
    }
    // Loot of the bots, kept as listing candidates for the Auction House (green or better, not equipment, not a quest item) and as sale candidates
    // for the vendors (white, not equipment, not a quest item). One handler for both: the client keeps one handler per hook.
    auto book = std::make_shared<AuctionBook>();
    auto sells = std::make_shared<SellBook>();
    if ((settings.Auction.Enabled || settings.Sell.Enabled) && !scenarioOn)
    {
        client.OnNotify("PlayerScript.OnLootItem", [&run, &settings, book, sells](forever_module::EventNotify const& notify)
        {
            if (notify.event_payload_case() != forever_module::EventNotify::kPlayerItemLooted)
                return;
            forever_module::PlayerItemLootedEvent const& event = notify.player_item_looted();
            if (!run.Roster.Owned().count(event.player_guid()))
                return;
            if (settings.Sell.Enabled)
            {
                sell::Item const sold = SellItemOf(event);
                if (sell::Sellable(sold))
                {
                    auto& items = sells->Items[event.player_guid()];
                    if (items.size() >= kSellItemsPerBot)
                        items.erase(items.begin());
                    items[event.item_guid()] = sold;
                }
            }
            if (!settings.Auction.Enabled || event.quality() < auction::kUncommon)
                return;
            auction::Candidate const item = CandidateOf(event);
            if (item.Needed || item.QuestItem || item.ItemGuid == 0)
                return;
            auto& items = book->Items[event.player_guid()];
            if (items.size() >= kAuctionItemsPerBot)
                items.erase(items.begin());
            items[item.ItemGuid] = item;
        });
        if (settings.Sell.Enabled)
        {
            extras.Sell = sells;
            std::cerr << "forever-bots: vendor sales are on: a sale visit every " << settings.Sell.EveryMs / 1000 << " s per bot, at most "
                      << settings.Sell.MaxSalesPerVisit << " items per visit, while the bot stands at a vendor\n";
        }
    }
    if (settings.Auction.Enabled && !scenarioOn)
    {
        auto feed = std::make_shared<AuctionFeed>();
        extras.Auction = feed;
        client.AddTimer(kAuctionInterval, [&run, &client, &settings, feed, book]
        {
            std::vector<AuctionBot> list;
            std::uint64_t fedAt = 0;
            {
                std::lock_guard<std::mutex> lock(feed->Lock);
                list = feed->Bots;
                fedAt = feed->AtMs;
            }
            std::uint64_t const now = nowMs();
            if (fedAt == 0 || now - fedAt > kPresenceStaleMs)
                return;
            auction::CallBudget budget(settings.Auction.MaxCallsPerTick);
            budget.NewTick();
            // AuctionBuy (off by default): when the bot's buy visit is due and it has money to spend, browse the next equipment slot for upgrades
            // (the core scores the listings for this bot) and buy out the best upgrade per copper that the budget allows. One browse per visit; the
            // buyout follows from the browse's reply, so it is not counted again in this tick's budget.
            auto const buyVisit = [&client, &settings, &budget, now](AuctionBot const& bot, auction::Standing& standing)
            {
                if (!settings.Auction.BuyEnabled || !auction::BuyVisitDue(settings.Auction, standing, now))
                    return;
                std::uint64_t const spend = auction::BuyBudget(settings.Auction, bot.Copper, bot.Level);
                if (spend == 0 || !budget.Take())
                    return;
                standing.LastBuyMs = now;
                forever_module::ModuleClient::AuctionBrowseFilter filter;
                filter.inventoryType = auction::BrowseSlot(standing.BuyCursor++);
                filter.maxLevel = bot.Level;
                filter.maxBuyout = auction::BuyoutBound(spend);
                filter.onlyUpgrades = true;
                std::uint64_t const botGuid = bot.Guid;
                std::uint64_t const auctioneer = bot.AuctioneerGuid;
                client.AuctionBrowseByFilterForBot(botGuid, auctioneer, filter,
                                                   [&client, &settings, botGuid, auctioneer, spend](forever_module::ActionReply const& reply)
                {
                    if (!reply.success())
                        return;
                    std::vector<auction::Offer> offers;
                    for (forever_module::AuctionListingEvent const& listing : reply.listings())
                    {
                        auction::Offer offer;
                        offer.AuctionGuid = listing.auction_guid();
                        offer.Entry = listing.item_entry();
                        offer.Count = listing.count() ? listing.count() : 1;
                        offer.Bid = listing.bid();
                        offer.Buyout = listing.buyout();
                        offer.Upgrade = listing.upgrade_score();
                        offers.push_back(offer);
                    }
                    std::optional<auction::Offer> const pick = auction::ChooseBuy(settings.Auction, spend, offers, auction::UpgradeOf);
                    if (!pick)
                        return;
                    auction::Offer const offer = *pick;
                    client.AuctionHouseForBot(botGuid, forever_module::ModuleClient::AuctionHouseOp::kBuyout, auctioneer, 0, offer.AuctionGuid, offer.Entry, 0, 0,
                                              static_cast<std::uint32_t>(offer.Buyout), [botGuid, offer](forever_module::ActionReply const& bought)
                    {
                        if (bought.success())
                            std::cerr << "forever-bots: " << auction::BoughtLine(botGuid, offer, bought.copper_spent() ? bought.copper_spent() : offer.Buyout) << "\n";
                        else
                            std::cerr << "forever-bots: " << auction::BuyFailedLine(botGuid, offer, bought.reason()) << "\n";
                    });
                });
            };
            for (AuctionBot const& bot : list)
            {
                if (!bot.Idle || bot.AuctioneerGuid == 0)
                    continue;
                auction::Standing& standing = book->Standing[bot.Guid];
                std::vector<std::uint64_t>& ends = book->Ends[bot.Guid];
                auction::PruneExpired(ends, now);
                standing.ListedNow = static_cast<std::uint32_t>(ends.size());
                auction::Bags const bags{ kBagSlotsAssumed, kBagSlotsAssumed - std::min(bot.FreeSlots, kBagSlotsAssumed) };
                if (!auction::VisitDue(settings.Auction, bags, standing, false, now))
                {
                    buyVisit(bot, standing);
                    continue;
                }
                standing.LastVisitMs = now;

                std::vector<auction::Candidate> items;
                for (auto const& [itemGuid, candidate] : book->Items[bot.Guid])
                    items.push_back(candidate);
                for (auction::Listing const& listing : auction::PlanListings(settings.Auction, bags, standing, items))
                {
                    if (!budget.Take())
                        break;
                    std::uint64_t const botGuid = bot.Guid;
                    client.AuctionHouseForBot(botGuid, forever_module::ModuleClient::AuctionHouseOp::kList, bot.AuctioneerGuid, listing.ItemGuid, 0, listing.Entry, 1,
                                              listing.Bid, listing.Buyout, [book, botGuid, listing](forever_module::ActionReply const& reply)
                    {
                        auto& items = book->Items[botGuid];
                        if (reply.success())
                        {
                            book->Ends[botGuid].push_back(nowMs() + std::uint64_t(listing.RunMinutes) * 60000u);
                            book->Standing[botGuid].Attempts[listing.ItemGuid] = listing.Attempt;
                            items.erase(listing.ItemGuid);
                            std::cerr << "forever-bots: " << auction::ListedLine(botGuid, listing, 1) << "\n";
                            return;
                        }
                        std::cerr << "forever-bots: " << auction::ListRefusedLine(botGuid, listing, reply.reason()) << "\n";
                        // A refusal for the place or the money is tried again on a later visit; a refusal about the item drops it.
                        if (auction::ClassifyListFailure(reply.reason()) == auction::ListFailure::Skip)
                            items.erase(listing.ItemGuid);
                    });
                }
                buyVisit(bot, standing);
            }
        });
        std::cerr << "forever-bots: auction house is on: a listing visit every " << settings.Auction.EveryMs / 1000 << " s per bot, at most "
                  << settings.Auction.PerVisit << " items per visit\n";
        if (settings.Auction.BuyEnabled)
            std::cerr << "forever-bots: auction buying is on: an upgrade browse (one equipment slot) every " << settings.Auction.EveryMs / 1000
                      << " s per bot, a purchase costs at most " << settings.Auction.BuyMaxPct << " percent of the money above the reserve\n";
    }
    if (settings.Errand.Enabled && !scenarioOn)
    {
        // The trainer table (ErrandTrainerFile): without it no errand can be made, so the errands stay off.
        auto trainers = std::make_shared<std::vector<errand::Trainer>>();
        std::ifstream in(settings.ErrandTrainerFile);
        if (!in)
            std::cerr << "forever-bots: ErrandTrainerFile: cannot read '" << settings.ErrandTrainerFile << "'; errands are off\n";
        else
        {
            std::ostringstream text;
            text << in.rdbuf();
            std::vector<std::string> trainerWarnings;
            *trainers = errand::ParseTrainerTable(text.str(), trainerWarnings);
            for (std::string const& warning : trainerWarnings)
                std::cerr << "forever-bots: " << settings.ErrandTrainerFile << ": " << warning << "\n";
            std::cerr << "forever-bots: errands: " << trainers->size() << " trainers from " << settings.ErrandTrainerFile << "\n";
        }
        if (!trainers->empty())
        {
            auto feed = std::make_shared<ErrandFeed>();
            extras.Errand = feed;
            auto scheduler = std::make_shared<errand::Scheduler>(settings.Errand);
            auto journal = std::make_shared<errand::Journal>(errand::CityName); // the event lines (errand_policy.h)
            std::shared_ptr<CommandQueue> queue = commands;
            client.AddTimer(kErrandInterval, [feed, scheduler, journal, trainers, queue, book, &settings]
            {
                std::uint64_t const now = nowMs();
                std::vector<ErrandBot> list;
                std::vector<std::pair<std::uint64_t, ErrandOutcome>> outcomes;
                std::uint64_t fedAt = 0;
                {
                    std::lock_guard<std::mutex> lock(feed->Lock);
                    list = feed->Bots;
                    outcomes.swap(feed->Outcomes);
                    fedAt = feed->AtMs;
                }
                for (auto const& [guid, outcome] : outcomes)
                {
                    scheduler->Finish(guid, now); // done or failed: the trip is over either way
                    if (auto const line = journal->OnEnd(guid, errand::EndReason::kDone, outcome == ErrandOutcome::kFailed, now))
                        std::cerr << "forever-bots: event " << *line << "\n";
                }
                if (fedAt == 0 || now - fedAt > kPresenceStaleMs)
                    return;

                std::vector<errand::Bot> bots;
                std::unordered_map<std::uint64_t, ErrandBot const*> byGuid;
                for (ErrandBot const& bot : list)
                {
                    byGuid[bot.Guid] = &bot;
                    errand::Bot b;
                    b.Guid = bot.Guid;
                    b.Available = bot.Available;
                    b.Called = bot.Called;
                    if (bot.TrainDue && bot.Side != 0)
                    {
                        // Only the capitals of the bot's faction that have a trainer for its skill: a start always has a trainer to walk to.
                        for (std::uint32_t city = errand::kStormwind; city <= errand::kThunderBluff; ++city)
                        {
                            if (errand::ChooseTrainer(*trainers, bot.Side, bot.Skill, city, bot.Map, bot.X, bot.Y, bot.Z))
                                b.Cities.push_back(city);
                        }
                        b.TrainDue = !b.Cities.empty();
                    }
                    // Looted items to list (the Auction House, when it is on): the walk goes to the faction's auctioneer on the bot's map, and the
                    // visit (the listing timer) lists the items once the bot is there. Off until both the errands and the auction are on.
                    auto const looted = book->Items.find(bot.Guid);
                    bool const auctionDue = settings.Auction.Enabled && bot.Side != 0 && bot.AuctioneerValid && looted != book->Items.end() && !looted->second.empty();
                    if (!b.TrainDue && auctionDue)
                    {
                        b.AuctionDue = true;
                        // The faction's capitals, as for training; the city is only the errand count per capital.
                        if (bot.Side == errand::kAlliance)
                            b.Cities = { errand::kStormwind, errand::kIronforge, errand::kDarnassus };
                        else
                            b.Cities = { errand::kUndercity, errand::kOrgrimmar, errand::kThunderBluff };
                    }
                    bots.push_back(std::move(b));
                }

                errand::Decisions const decisions = scheduler->Tick(now, bots);
                for (errand::End const& end : decisions.Ends) // before the starts: a bot may start again in the same tick
                {
                    if (auto const line = journal->OnEnd(end.Guid, end.Why, false, now))
                        std::cerr << "forever-bots: event " << *line << "\n";
                }
                std::vector<CommandQueue::ErrandItem> items;
                for (errand::End const& end : decisions.Ends)
                {
                    CommandQueue::ErrandItem item;
                    item.Guid = end.Guid;
                    item.Cancel = true;
                    items.push_back(item);
                }
                for (errand::Start const& start : decisions.Starts)
                {
                    auto const found = byGuid.find(start.Guid);
                    if (found == byGuid.end())
                        continue;
                    ErrandBot const& bot = *found->second;
                    if (start.What == errand::Kind::kAuction)
                    {
                        CommandQueue::ErrandItem item;
                        item.Guid = start.Guid;
                        item.Order.Map = bot.Map;
                        item.Order.Dest = Point{ bot.AuctioneerX, bot.AuctioneerY, bot.AuctioneerZ };
                        item.Order.Auction = true;
                        items.push_back(item);
                        std::cerr << "forever-bots: errand: bot " << start.Guid << " to the auctioneer (city " << start.City << ")\n";
                        std::cerr << "forever-bots: event " << journal->OnStart(start, now) << "\n";
                        continue;
                    }
                    auto const pick = errand::ChooseTrainer(*trainers, bot.Side, bot.Skill, start.City, bot.Map, bot.X, bot.Y, bot.Z);
                    if (!pick)
                        continue;
                    errand::Trainer const& trainer = (*trainers)[*pick];
                    CommandQueue::ErrandItem item;
                    item.Guid = start.Guid;
                    item.Order.Map = trainer.Map;
                    item.Order.Dest = Point{ trainer.X, trainer.Y, trainer.Z };
                    item.Order.Skill = trainer.Skill;
                    item.Order.Entry = trainer.Entry;
                    items.push_back(item);
                    std::cerr << "forever-bots: errand: bot " << start.Guid << " to " << trainer.Name << " (" << trainer.Subname << ") in city " << start.City << "\n";
                    std::cerr << "forever-bots: event " << journal->OnStart(start, now) << "\n";
                }
                if (!items.empty() && queue)
                {
                    std::lock_guard<std::mutex> lock(queue->Lock);
                    for (auto& item : items)
                        queue->Errands.push_back(std::move(item));
                }
                for (errand::End const& end : decisions.Ends)
                {
                    std::cerr << "forever-bots: errand: bot " << end.Guid << " ended (reason " << int(end.Why) << ")\n";
                }
            });
            std::cerr << "forever-bots: town errands are on: a check every " << kErrandInterval.count() / 1000 << " s\n";
        }
    }
    if (scenarioOn)
    {
        // The scenario takes over the party, the route and the dressing: no vendor walks, no ramp, one party of each roster. Every dungeon's roster has one size
        // (bot_settings.h), so the bots in guid order cut into parties of that size, the first dungeon's parties first.
        policyOptions.PartySize = std::uint32_t(settings.Scenario.Roster.size());
        policyOptions.PartyCount = ScenarioPartyCount(settings.Scenarios);
        policyOptions.PartyPercent = 100; // every bot of the roster is in its party
        policyOptions.RandomPct = 0;      // and a scenario is a test run: it is not varied
        policyOptions.Crafting = false;
        policyOptions.ServiceEveryMs = 0;
        policyOptions.AutoGearing = false;
        // 40-man raid support (doc 108): a raid scenario's roster is one raid group of 8 subgroups, not a flat party (bot_policy.cpp's
        // party-formation already cuts one party of PartySize bots regardless of size; Raid only changes how that one party is
        // handled once formed). settings.Scenarios front is used the same way policyOptions.PartySize already is above, since a raid
        // runs on its own schedule (doc 108), never mixed into the dungeon list.
        policyOptions.Raid = settings.Scenarios.empty() ? false : settings.Scenarios.front().Raid;
        for (ScenarioSettings const& scenario : settings.Scenarios)
        {
            PolicyOptions::ScenarioRoute route;
            for (auto const& point : scenario.Route)
                route.Route.push_back({ point[0], point[1], point[2] });
            for (auto const& [point, entry] : scenario.BossAt)
                route.RouteBossAt[point] = entry;
            route.Events = dungeon_events::ForMap(allEvents, std::uint32_t(scenario.Entry[0]));
            route.RouteMap = static_cast<std::uint32_t>(scenario.Entry[0]);
            for (std::size_t i = 0; i < scenario.KillPriority.size(); ++i)
                route.KillPriority.emplace(scenario.KillPriority[i], std::uint32_t(i));

            ScenarioPlan plan;
            plan.Name = scenario.Name;
            plan.RosterSize = std::uint32_t(scenario.Roster.size());
            plan.Raid = scenario.Raid;
            plan.EntryMap = static_cast<std::uint32_t>(scenario.Entry[0]);
            plan.Entry = { scenario.Entry[1], scenario.Entry[2], scenario.Entry[3] };
            for (auto const& point : scenario.Route)
                plan.Route.push_back({ point[0], point[1], point[2] });
            plan.DurationMs = std::uint64_t(scenario.DurationSec) * 1000;
            plan.Gear = scenario.Gear;
            plan.Supplies = scenario.Supplies;
            plan.ClassSupplies = scenario.ClassSupplies;
            plan.ClassQuests = scenario.ClassQuests;
            plan.AmmoCount = scenario.Ammo;
            plan.Bosses = scenario.Bosses;
            plan.ClassOf = policyOptions.ClassOf;
            plan.RaceOf = policyOptions.RaceOf;
            plan.TameEnabled = scenario.HasTameSpot;
            plan.TameMap = static_cast<std::uint32_t>(scenario.TameSpot[0]);
            plan.TameSpot = { scenario.TameSpot[1], scenario.TameSpot[2], scenario.TameSpot[3] };
            plan.WipeRecovery = scenario.WipeRecovery;
            plan.WipeRecoveryMs = std::uint64_t(scenario.WipeRecoverySec) * 1000;
            plan.RequireBosses = scenario.RequireBosses;
            plan.MinAlive = scenario.MinAlive;
            plan.LeaderDeadAbandonMs = std::uint64_t(scenario.LeaderDeadAbandonSec) * 1000;
            if (scenario.HasPortal)
            {
                route.HasPortal = true;
                route.PortalMap = static_cast<std::uint32_t>(scenario.Portal[0]);
                route.PortalPoint = { scenario.Portal[1], scenario.Portal[2], scenario.Portal[3] };
                route.PortalInsideMap = plan.EntryMap;
            }
            extras.Scenarios.push_back(PumpExtras::ScenarioDungeon{ std::move(plan), scenario.Parties, std::move(route) });
        }
        // The single-dungeon fields are the first dungeon's: with one dungeon they are the scenario's, with several each party's own comes from SetScenarioRouteFor.
        PolicyOptions::ScenarioRoute const& first = extras.Scenarios.front().Route;
        policyOptions.Route = first.Route;
        policyOptions.RouteBossAt = first.RouteBossAt;
        policyOptions.Events = first.Events;
        policyOptions.RouteMap = first.RouteMap;
        policyOptions.KillPriority = first.KillPriority;
        policyOptions.HasPortal = first.HasPortal;
        policyOptions.PortalMap = first.PortalMap;
        policyOptions.PortalPoint = first.PortalPoint;
        policyOptions.PortalInsideMap = first.PortalInsideMap;
        extras.ScenarioResultFile = settings.Scenario.ResultFile;
    }

    // Tier 2 is granted at handshake, so there is a region only if the core accepted it.
    std::atomic<bool> running{true};
    std::thread pump;
    if (forever_module::Tier2Session* session = client.AttachTier2())
        pump = std::thread(PumpTier2, std::ref(*session), std::cref(running), policyOptions, std::cref(settings.TelemetryFile),
                           std::cref(settings), std::move(extras));
    else
        std::cerr << "forever-bots: no Tier 2 region (" << client.tier2_error() << "); bots will not be driven\n";

    client.Run();

    running = false;
    if (pump.joinable())
        pump.join();
    return 0;
}
