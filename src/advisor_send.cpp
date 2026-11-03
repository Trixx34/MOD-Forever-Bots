// SPDX-License-Identifier: GPL-2.0-or-later
#include "advisor_send.h"

#include "forever_module/module_client.h"

namespace advisor {

bool Send(forever_module::ModuleClient& client, Order const& order)
{
    using forever_module::ModuleClient;
    if (order.Bot == 0)
        return false;
    switch (order.What)
    {
        case Kind::Emote:
            if (order.Arg > 4)
                return false;
            client.BotEmote(order.Bot, static_cast<forever_module::EmoteAction::EmoteKind>(order.Arg), order.Target);
            return true;
        case Kind::LfgJoin:
            client.BotLfgJoin(order.Bot, order.Arg, order.Map);
            return true;
        case Kind::PartySay:
            if (order.Text.empty())
                return false;
            client.BotSpeak(order.Bot, ModuleClient::SpeakKind::kParty, 0, order.Text);
            return true;
        case Kind::GuildSay:
            if (order.Text.empty())
                return false;
            client.BotSpeak(order.Bot, ModuleClient::SpeakKind::kGuild, 0, order.Text);
            return true;
        case Kind::TradeSay:
            if (order.Text.empty())
                return false;
            client.BotSpeakChannel(order.Bot, ModuleClient::SpeakChannel::kTrade, order.Text);
            return true;
    }
    return false;
}

bool Decode(forever_module::EventNotify const& notify, lfg_queue_fill::Queue& out)
{
    if (!notify.has_lfg_queue())
        return false;
    forever_module::LfgQueueEvent const& event = notify.lfg_queue();
    out.MapId = event.map_id();
    out.QueuedSec = event.queued_seconds();
    out.OpenRoles.assign(event.open_roles().begin(), event.open_roles().end());
    return true;
}

}  // namespace advisor
