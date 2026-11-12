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
        case Kind::Say:
            if (order.Text.empty())
                return false;
            client.BotSpeak(order.Bot, ModuleClient::SpeakKind::kSay, 0, order.Text);
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

bool DecodeDecline(forever_module::EventNotify const& notify, std::uint64_t& decliner)
{
    if (!notify.has_group_invite_declined())
        return false;
    decliner = notify.group_invite_declined().decliner_guid();
    return true;
}

bool Decode(forever_module::EventNotify const& notify, BossKill& out)
{
    if (!notify.has_boss_killed())
        return false;
    forever_module::BossKilledEvent const& event = notify.boss_killed();
    out.Map = event.map_id();
    out.Instance = event.instance_id();
    out.Entry = event.creature_entry();
    out.Killer = event.killer_guid();
    out.GroupLeader = event.group_leader_guid();
    out.DungeonBoss = event.dungeon_boss() != 0;
    return true;
}

bool Decode(forever_module::EventNotify const& notify, GuildLevel& out)
{
    if (!notify.has_guild_member_level())
        return false;
    forever_module::GuildMemberLevelEvent const& event = notify.guild_member_level();
    out.Guild = event.guild_guid();
    out.Member = event.member_guid();
    out.Level = event.new_level();
    return true;
}

}  // namespace advisor
