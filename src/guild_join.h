// SPDX-License-Identifier: GPL-2.0-or-later
// Guilds (config key GuildJoin, off by default): an idle bot accepts a guild invite, and leaves when told to. Pure logic. The policy (TurnSocial) turns a decision
// into social::Order, which main_commands.cpp sends with ModuleClient::BotGuild. A core without the guild action refuses it and the policy logs the
// event guild_gap.
#pragma once

#include <cstdint>

namespace guild {

struct Settings
{
    bool Enabled = false;
};

struct Context
{
    bool Idle = false;
    bool InCombat = false;
    bool InvitePending = false;        // a guild invite is waiting for the bot
    bool HasGuild = false;
    bool LeaveRequested = false;       // a player asked the bot to leave its guild
};

enum class Action { None, Join, Leave };

Action Decide(Settings const& settings, Context const& ctx);

}  // namespace guild
