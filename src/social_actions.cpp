// SPDX-License-Identifier: GPL-2.0-or-later
#include "social_actions.h"

#include "mail_gifts.h"

#include "forever_module/module_client.h"

#include <memory>
#include <vector>

namespace social {

using forever_module::ActionReply;
using forever_module::ModuleClient;

namespace {

Outcome Answer(Order const& order, ActionReply const& reply)
{
    Outcome outcome;
    outcome.What = order.What;
    outcome.Bot = order.Bot;
    outcome.Op = order.Op;
    outcome.Success = reply.success();
    outcome.Reason = reply.reason();
    return outcome;
}

Outcome Failed(Order const& order, std::string reason)
{
    Outcome outcome;
    outcome.What = order.What;
    outcome.Bot = order.Bot;
    outcome.Op = order.Op;
    outcome.Reason = std::move(reason);
    return outcome;
}

}  // namespace

bool Send(ModuleClient& client, Order const& order, Done done)
{
    if (order.Bot == 0)
        return false;
    auto const answer = [order, done](ActionReply const& reply)
    {
        if (done)
            done(Answer(order, reply));
    };
    switch (order.What)
    {
        case Kind::Duel:
            client.BotDuel(order.Bot, static_cast<ModuleClient::DuelOp>(order.Op), order.Player, answer);
            return true;
        case Kind::Battleground:
            client.BotBattleground(order.Bot, static_cast<ModuleClient::BattlegroundOp>(order.Op), order.Id, answer);
            return true;
        case Kind::Guild:
            client.BotGuild(order.Bot, static_cast<ModuleClient::GuildOp>(order.Op), answer);
            return true;
        case Kind::Gift:
        {
            // The bags first: the core lists what the bot can trade (never a bound item; it needs the core's Bot.Trade.Enabled), PickGift chooses a small
            // stack, and the mail goes out from the reply. The core checks the item again when the mail is sent.
            if (order.Player == 0)
                return false;
            client.BotTrade(order.Bot, ModuleClient::BotTradeOp::kListBags, 0, 0, 0, [&client, order, done, answer](ActionReply const& bags)
            {
                if (!bags.success())
                {
                    if (done)
                        done(Answer(order, bags));
                    return;
                }
                std::vector<gifts::BagStack> stacks;
                for (auto const& item : bags.bag_items())
                    stacks.push_back({ item.item_guid(), item.entry(), item.count(), item.name() });
                int const pick = gifts::PickGift(stacks, order.Seed);
                if (pick < 0)
                {
                    if (done)
                        done(Failed(order, "no item fit to give"));
                    return;
                }
                gifts::BagStack const& stack = stacks[std::size_t(pick)];
                client.BotGiftMail(order.Bot, order.Player, stack.ItemGuid, stack.Count, 0, gifts::GiftSubject(stack.Name), answer);
            });
            return true;
        }
    }
    return false;
}

char const* GapEventName(Kind kind)
{
    switch (kind)
    {
        case Kind::Duel: return "duel_gap";
        case Kind::Battleground: return "battleground_gap";
        case Kind::Guild: return "guild_gap";
        case Kind::Gift: return "gift_gap";
    }
    return "social_gap";
}

bool IsUnknownActionReason(std::string const& reason)
{
    return reason.find("unknown action") != std::string::npos;
}

std::uint32_t PickBattleground(std::uint32_t level, std::uint64_t seed)
{
    std::vector<std::uint32_t> open;
    if (level >= 10)
        open.push_back(2);   // Warsong Gulch
    if (level >= 20)
        open.push_back(3);   // Arathi Basin
    if (level >= 61)
        open.push_back(7);   // Eye of the Storm
    if (level >= 51)
        open.push_back(1);   // Alterac Valley
    return open.empty() ? 0 : open[std::size_t(seed % open.size())];
}

bool IsBattlegroundMap(std::uint32_t mapId)
{
    return mapId == 30 || mapId == 489 || mapId == 529 || mapId == 566;
}

}  // namespace social
