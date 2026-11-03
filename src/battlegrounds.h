// SPDX-License-Identifier: GPL-2.0-or-later
// Battlegrounds (config key Battlegrounds, off by default): an idle bot at least BattlegroundMinLevel queues for a battleground. A queue that has not
// popped within BattlegroundMaxWaitMin is left. Pure logic. The policy (TurnSocial) turns a decision into social::Order, which main_commands.cpp sends with
// ModuleClient::BotBattleground. A core without the battleground action refuses it and the policy logs the event battleground_gap.
#pragma once

#include <cstdint>

namespace battlegrounds {

struct Settings
{
    bool Enabled = false;
    std::uint32_t MinLevel = 10;      // BattlegroundMinLevel
    std::uint32_t MaxWaitMin = 10;    // BattlegroundMaxWaitMin: a queue older than this is left
    std::uint32_t CooldownMin = 60;   // fixed gap between two queues of one bot
};

struct State
{
    bool Queued = false;
    std::uint64_t QueuedAtMs = 0;
    std::uint64_t NextMs = 0;
};

struct Context
{
    bool Idle = false;
    bool InCombat = false;
    std::uint32_t Level = 0;
    bool InBattleground = false;      // the queue popped and the bot is inside
};

enum class Action { None, Queue, Leave };

Action Decide(Settings const& settings, State const& state, Context const& ctx, std::uint64_t nowMs);
void NoteQueued(State& state, std::uint64_t nowMs);
void NoteLeft(State& state, Settings const& settings, std::uint64_t nowMs);

}  // namespace battlegrounds
