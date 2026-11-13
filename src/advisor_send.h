// SPDX-License-Identifier: GPL-2.0-or-later
// The one place that names the protocol 21 SDK calls (BotEmote, BotLfgJoin, BotSpeak kParty / kGuild) and decodes the two protocol 21 events, so an SDK
// rename is a one-file change.
#pragma once

#include "advisor_orders.h"

#include "damage_events.h"
#include "lfg_queue_fill.h"

#include <cstdint>

namespace forever_module {
class ModuleClient;
class EventNotify;
}

namespace advisor {

// Sends the order. False when it could not be sent (an order with no bot, or an empty line).
bool Send(forever_module::ModuleClient& client, Order const& order);

struct GuildLevel
{
    std::uint64_t Guild = 0;
    std::uint64_t Member = 0;
    std::uint32_t Level = 0;
};

// The payload of EventNotify::guild_member_level() (hook GuildScript.OnMemberLevel).
bool Decode(forever_module::EventNotify const& notify, GuildLevel& out);

// The payload of EventNotify::group_invite_declined() (hook GroupScript.OnInviteDeclined): the player that declined; false when the notify carries none.
bool DecodeDecline(forever_module::EventNotify const& notify, std::uint64_t& decliner);

// The payload of EventNotify::boss_killed() (hook PlayerScript.OnBossKill, protocol 27): a dungeon boss died to a player, a pet or a charm.
struct BossKill
{
    std::uint32_t Map = 0;
    std::uint32_t Instance = 0;
    std::uint32_t Entry = 0;
    std::uint64_t Killer = 0;       // raw guid counter of the killing player
    std::uint64_t GroupLeader = 0;  // raw guid counter of the killer's group leader, 0 without a group
    bool DungeonBoss = false;       // the creature carries the dungeon boss flag
};
bool Decode(forever_module::EventNotify const& notify, BossKill& out);

// The payload of EventNotify::player_damaged() (hook PlayerScript.OnDamaged, protocol 29); false when the notify carries none. AtMs is left 0 for the caller.
bool Decode(forever_module::EventNotify const& notify, damage_events::Hit& out);

// The payload of EventNotify::lfg_queue() (hook LfgScript.OnQueueUpdate); false when the notify carries none.
bool Decode(forever_module::EventNotify const& notify, lfg_queue_fill::Queue& out);

}  // namespace advisor
