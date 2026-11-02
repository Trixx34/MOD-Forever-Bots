// SPDX-License-Identifier: GPL-2.0-or-later
// Duels (config key Duels, off by default): a bot that is idle, healthy and at least DuelMinLevel challenges a rival that stands near it, or accepts a
// challenge sent to it. Pure logic: the caller passes the bot's state and the clock. No duel action exists in the SDK yet (doc/second-list-gaps.md),
// so the policy only logs the decision (event duel_gap) until one is added.
#pragma once

#include <cstdint>

namespace duels {

struct Settings
{
    bool Enabled = false;
    std::uint32_t MinLevel = 10;      // DuelMinLevel
    std::uint32_t CooldownMin = 30;   // DuelCooldownMin: minutes between two duels of one bot
};

struct State
{
    std::uint64_t NextMs = 0;         // no new duel before this
};

struct Context
{
    bool Idle = false;
    bool InCombat = false;
    bool Dead = false;
    std::uint32_t Level = 0;
    bool RivalNearby = false;         // a player the bot could challenge stands near
    bool ChallengeReceived = false;   // a player has challenged this bot
};

enum class Action { None, Challenge, Accept };

Action Decide(Settings const& settings, State const& state, Context const& ctx, std::uint64_t nowMs);
void NoteStarted(State& state, Settings const& settings, std::uint64_t nowMs);

}  // namespace duels
