// SPDX-License-Identifier: GPL-2.0-or-later
// A last line of defence against refused moves sent again and again. The core answers a move it cannot path with "no route", and some source (the policy,
// the vendor-sale pump) sent the same move on every tick: 140 bots refused 5,000 times a minute. This gate sits between all the sources and the core:
// it remembers the last move sent to each bot, and when the bot's answer is "no route" it holds back moves to (about) that place for a pause that doubles
// with every answer in a row (5 s up to 2.7 minutes). It does not decide anything else: a move to another place passes.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "forever_module/tier2_session.h"

namespace move_gate
{

constexpr std::uint64_t kPauseMs = 5000;
constexpr std::uint32_t kMaxDoublings = 5;
constexpr float kSameYards = 12.0f;
constexpr float kPartialMinYards = 150.0f;     // a refused walk this long may be a first leg only (a capped navmesh path)
constexpr float kPartialProgressYards = 25.0f; // the bot is this much nearer the refused place: the walk made progress, the hold is lifted

class MoveGate
{
public:
    // The bot's last answer: a new "no route" arms the pause for the place of the move sent last.
    void Observe(std::uint64_t guid, std::uint8_t lastType, forever_module::BotActionStatus status, std::uint32_t sequence, std::uint64_t nowMs, float botX = 0.0f, float botY = 0.0f)
    {
        Entry& entry = _bots[guid];
        if (sequence == 0 || sequence == entry.SeenSequence)
            return;
        entry.SeenSequence = sequence;
        if (lastType != static_cast<std::uint8_t>(forever_module::ActionType::kMoveTo))
            return;
        if (status != forever_module::BotActionStatus::kNoRoute)
        {
            if (status == forever_module::BotActionStatus::kApplied)
                entry.Streak = 0;
            return;
        }
        if (!entry.HasSent)
            return;
        bool const same = entry.BlockedUntilMs != 0 && Near(entry.BlockedX, entry.BlockedY, entry.SentX, entry.SentY);
        entry.Streak = same ? std::min(entry.Streak + 1, kMaxDoublings + 1) : 1;
        entry.BlockedX = entry.SentX;
        entry.BlockedY = entry.SentY;
        entry.BlockedUntilMs = nowMs + (kPauseMs << (entry.Streak - 1));
        entry.ArmedYards = std::hypot(botX - entry.BlockedX, botY - entry.BlockedY);
    }

    // Every tick, with where the bot is now. The core carries out a move it has no full route for as far as the navmesh path reaches, and a long path is capped, so a
    // long walk that was refused may have made real progress: when the bot is kPartialProgressYards nearer the refused place than when the hold began, the hold is lifted
    // (and the run of refusals ends), so the policy can send the next leg.
    void Progress(std::uint64_t guid, float botX, float botY, std::uint64_t nowMs)
    {
        auto const found = _bots.find(guid);
        if (found == _bots.end())
            return;
        Entry& entry = found->second;
        if (nowMs >= entry.BlockedUntilMs || entry.ArmedYards < kPartialMinYards)
            return;
        if (entry.ArmedYards - std::hypot(botX - entry.BlockedX, botY - entry.BlockedY) >= kPartialProgressYards)
        {
            entry.BlockedUntilMs = 0;
            entry.Streak = 0;
        }
    }

    // Takes out the moves to a place that is paused; remembers the others as the last one sent. Returns how many were held back.
    std::size_t Filter(std::vector<forever_module::BotAction>& actions, std::uint64_t nowMs)
    {
        std::size_t held = 0;
        std::vector<forever_module::BotAction> kept;
        kept.reserve(actions.size());
        for (forever_module::BotAction const& action : actions)
        {
            if (action.type == forever_module::ActionType::kMoveTo)
            {
                Entry& entry = _bots[action.bot_guid];
                if (nowMs < entry.BlockedUntilMs && Near(entry.BlockedX, entry.BlockedY, action.dest_x, action.dest_y))
                {
                    ++held;
                    continue;
                }
                entry.HasSent = true;
                entry.SentX = action.dest_x;
                entry.SentY = action.dest_y;
            }
            kept.push_back(action);
        }
        actions = std::move(kept);
        return held;
    }

private:
    struct Entry
    {
        bool HasSent = false;
        float SentX = 0.0f, SentY = 0.0f;
        float BlockedX = 0.0f, BlockedY = 0.0f;
        std::uint64_t BlockedUntilMs = 0;
        float ArmedYards = 0.0f;       // the bot's distance to the blocked place when the hold began
        std::uint32_t Streak = 0;
        std::uint32_t SeenSequence = 0;
    };

    static bool Near(float ax, float ay, float bx, float by)
    {
        return std::hypot(ax - bx, ay - by) < kSameYards;
    }

    std::unordered_map<std::uint64_t, Entry> _bots;
};

} // namespace move_gate
