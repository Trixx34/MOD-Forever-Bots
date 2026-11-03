// SPDX-License-Identifier: GPL-2.0-or-later
// main_detail: the .bots and .autogear commands, bot trade and the chat orders (declared in main_internal.h, called from main()).
#include "main_internal.h"

namespace main_detail {

// The CommandScript request: .bots <verb> (fleet administration and the chat orders as a dot command) and .autogear.
forever_module::EventReply CommandScriptReply(forever_module::EventRequest const& request, std::shared_ptr<CommandQueue> const& commands,
                                              std::shared_ptr<AltRegistry> const& alts, RunContext& run, BotSettings& settings)
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
        // alt/spawn/list/autogear below are always available regardless of ChatOrders/ChatControl: they manage which
        // characters play as bots at all (fleet administration), not in-fight orders to bots already playing, so they
        // are intentionally not gated the same way ParseChatCommand's verbs are a few lines down.
        if (!commandWords.empty() && commandWords[0] == "alt")
        {
            BotsAltCommand(reply, commandWords, issuerCounter, alts, run);
            return reply;
        }
        if (!commandWords.empty() && commandWords[0] == "spawn")
        {
            BotsSpawnCommand(reply, commandWords, issuerCounter, run);
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
        if (parsed->Verb == ChatVerb::Lfg && !settings.LfgCommand)
        {
            setReply(false, "LFG is off (LfgCommand = 0 in the module's config).");
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
}

// .bots alt add <name> | alt remove <name> | alt list: the alts that play as bots. The answer is in `reply`.
void BotsAltCommand(forever_module::EventReply& reply, std::vector<std::string> const& commandWords, std::uint64_t issuerCounter,
                    std::shared_ptr<AltRegistry> const& alts, RunContext& run)
{
    auto const setReply = [&reply](bool ok, std::string const& message)
    {
        reply.mutable_command()->set_success(ok);
        reply.mutable_command()->set_result_text(message);
    };
    auto const tellIssuer = [&run, issuerCounter](std::string const& text) { run.Client.SendPlayerMessage(issuerCounter, text); };
    if (commandWords.size() == 2 && commandWords[1] == "list")
    {
        std::lock_guard<std::mutex> lock(alts->Lock);
        std::string names;
        for (auto const& [name, guid] : alts->ByName)
            names += (names.empty() ? "" : ", ") + name;
        setReply(true, names.empty() ? "No alts are playing as bots." : "Alts as bots: " + names);
        return;
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
        return;
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
            return;
        }
        run.Roster.OnRevoked(guid, "alt removed");
        run.Client.ReleaseControl(guid); // the core logs an alt out when the module lets it go
        setReply(true, name + " is being logged out.");
        return;
    }
    setReply(false, "Usage: .bots alt add <name> | alt remove <name> | alt list");
}

// .bots spawn <race> <class> [level]: one bot of that race and class. The answer is in `reply`.
void BotsSpawnCommand(forever_module::EventReply& reply, std::vector<std::string> const& commandWords, std::uint64_t issuerCounter, RunContext& run)
{
    auto const setReply = [&reply](bool ok, std::string const& message)
    {
        reply.mutable_command()->set_success(ok);
        reply.mutable_command()->set_result_text(message);
    };
    auto const tellIssuer = [&run, issuerCounter](std::string const& text) { run.Client.SendPlayerMessage(issuerCounter, text); };
    std::uint32_t race = 0, cls = 0, level = 0;
    auto const number = [](std::string const& text, std::uint32_t& out)
    {
        return std::from_chars(text.data(), text.data() + text.size(), out).ec == std::errc{};
    };
    if (commandWords.size() < 3 || commandWords.size() > 4 || !number(commandWords[1], race) || !number(commandWords[2], cls) || (commandWords.size() == 4 && !number(commandWords[3], level)) ||
        race == 0 || cls == 0)
    {
        setReply(false, "Usage: .bots spawn <race id> <class id> [level]");
        return;
    }
    if (!run.Roster.CanRequest())
    {
        setReply(false, "The module cannot take another bot (its bot limit is reached).");
        return;
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
}

// Bot trade (trade_policy.h): the steps of a trade run as client calls; the trade events, and a timer that cancels a trade nobody works on, feed them.
void SetUpTrade(forever_module::ModuleClient& client, std::shared_ptr<trade::TradeBook> const& trades, std::shared_ptr<std::function<void(std::vector<trade::Step> const&)>> const& runTrade,
                std::shared_ptr<CommandQueue> const& commands)
{
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
    client.OnNotify("LfgScript.OnQueueUpdate", [commands, &settings](forever_module::EventNotify const& notify)
    {
        lfg_queue_fill::Queue queue;
        if (!settings.LfgQueueFill.Enabled || !advisor::Decode(notify, queue))
            return;
        std::lock_guard<std::mutex> lock(commands->Lock);
        if (commands->LfgQueues.size() < 32)
            commands->LfgQueues.push_back(std::move(queue));
    });
    client.OnNotify("PlayerScript.OnBotTrade", [trades, runTrade, commands](forever_module::EventNotify const& notify)
    {
        if (notify.event_payload_case() != forever_module::EventNotify::kBotTrade)
            return;
        forever_module::BotTradeEvent const& event = notify.bot_trade();
        if (event.status() > static_cast<std::uint32_t>(trade::Status::Completed))
            return;
        // A busy bot refuses a trade request with its reason (WhisperReplies); the refusal is cancelled on the Run() thread.
        std::string busyLine;
        if (event.status() == static_cast<std::uint32_t>(trade::Status::Requested))
        {
            std::lock_guard<std::mutex> lock(commands->Lock);
            if (commands->BusyBots.count(event.bot_guid()))
                busyLine = BusyLine(event.bot_guid(), BusyKind::None);
        }
        // A finished trade: the bot may thank the partner with a gift (MailGifts; the policy ignores it when the setting is off).
        if (event.status() == static_cast<std::uint32_t>(trade::Status::Completed))
        {
            std::lock_guard<std::mutex> lock(commands->Lock);
            if (commands->GiftThanks.size() < 64)
                commands->GiftThanks.emplace_back(event.bot_guid(), event.partner_guid());
        }
        (*runTrade)(trades->OnEvent(event.bot_guid(), event.partner_guid(), static_cast<trade::Status>(event.status()), nowMs(), busyLine));
    });
    // A trade nobody is working on is cancelled.
    client.AddTimer(std::chrono::seconds(5), [trades, runTrade] { (*runTrade)(trades->OnIdle(nowMs())); });
}

void SetUpChat(forever_module::ModuleClient& client, std::shared_ptr<CommandQueue> const& commands, BotSettings& settings, std::shared_ptr<trade::TradeBook> const& trades,
               std::shared_ptr<std::function<void(std::vector<trade::Step> const&)>> const& runTrade)
{
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
            // A whisper that is not an order may get a reply from the bot (WhisperReplies; the pump thread decides it).
            if (settings.ChatWhisperReplies && whisper.channel() == 3 && !ParseChatCommand(whisper.text(), settings.ChatOrders, settings.ChatControl))
            {
                std::lock_guard<std::mutex> lock(commands->Lock);
                if (commands->Whispers.size() < 64)
                    commands->Whispers.push_back({ whisper.player_guid(), whisper.recipient_guid(), whisper.text() });
                return;
            }
        }
        if ((!settings.ChatCommands && !settings.LfgCommand) || notify.event_payload_case() != forever_module::EventNotify::kPlayerChat)
            return;
        forever_module::PlayerChatEvent const& chat = notify.player_chat();
        auto const parsed = ParseChatCommand(chat.text(), settings.ChatOrders, settings.ChatControl);
        if (!parsed)
            return; // ordinary chat
        // LFG has its own switch (LfgCommand); every other verb needs ChatCommands and is not taken from the General or Trade channel (channel 4 and 5: the core
        // reports the public channels with these numbers; the SDK's PlayerChatEvent only documents 1 to 3, see doc/dungeons.md).
        bool const publicChannel = chat.channel() == 4 || chat.channel() == 5;
        if (parsed->Verb == ChatVerb::Lfg ? !settings.LfgCommand : (!settings.ChatCommands || publicChannel))
            return;
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
        std::vector<CommandQueue::AmbientLine> ambientLines;
        std::vector<BotPolicy::WhisperReply> whispers;
        std::vector<social::Order> socialOrders;
        std::vector<advisor::Order> advisorOrders;
        {
            std::lock_guard<std::mutex> lock(commands->Lock);
            replies.swap(commands->Replies);
            says.swap(commands->Says);
            talents.swap(commands->Talents);
            whispers.swap(commands->WhisperReplies);
            ambientLines.swap(commands->Ambient);
            socialOrders.swap(commands->Social);
            advisorOrders.swap(commands->Advisor);
        }
        for (social::Order const& order : socialOrders)
        {
            // The core's answer goes back to the pump thread (the policy updates the feature's state from it).
            bool const sent = social::Send(client, order, [commands](social::Outcome const& outcome)
            {
                std::lock_guard<std::mutex> lock(commands->Lock);
                if (commands->SocialOutcomes.size() < 256)
                    commands->SocialOutcomes.push_back(outcome);
            });
            if (!sent)
            {
                social::Outcome failed;
                failed.What = order.What;
                failed.Bot = order.Bot;
                failed.Op = order.Op;
                failed.Reason = "the order could not be sent: unknown action";
                std::lock_guard<std::mutex> lock(commands->Lock);
                commands->SocialOutcomes.push_back(std::move(failed));
            }
        }
        for (advisor::Order const& order : advisorOrders)
            advisor::Send(client, order);
        for (BotPolicy::WhisperReply const& whisper : whispers)
            client.BotSpeak(whisper.Bot, forever_module::ModuleClient::SpeakKind::kWhisper, whisper.Player, whisper.Text);
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
        {
            if (say.Player != 0)
                client.BotSpeak(say.Bot, forever_module::ModuleClient::SpeakKind::kWhisper, say.Player, say.Text);
            else
                client.BotSpeak(say.Bot, forever_module::ModuleClient::SpeakKind::kSay, 0, say.Text);
        }
        for (CommandQueue::AmbientLine const& line : ambientLines)
            client.BotSpeakChannel(line.Bot, line.Trade ? forever_module::ModuleClient::SpeakChannel::kTrade : forever_module::ModuleClient::SpeakChannel::kGeneral, line.Text);
    });
}

} // namespace main_detail
