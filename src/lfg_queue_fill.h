// SPDX-License-Identifier: GPL-2.0-or-later
// LFG queue fill (config LfgQueueFill, off by default): when the dungeon finder queue of a map has waited long enough and still misses a role, one idle
// bot joins it in that role. Pure logic, unit-tested in tests/lfg_queue_fill_test.cpp. (The older LfgFill key is the separate "offer bots to a human
// party" feature, lfg_fill.h.) The policy (bot_policy_advisor.cpp) picks the bot and queues the order; no combat code is involved.
#pragma once

#include <cstdint>
#include <map>
#include <vector>

namespace lfg_queue_fill {

struct Settings
{
    bool Enabled = false;                 // LfgQueueFill
    std::uint32_t WaitSec = 60;           // LfgQueueFillWaitSec: the queue must have waited this long
    std::uint32_t CooldownSec = 120;      // LfgQueueFillCooldownSec: at most one bot per queue (map) in this time
};

constexpr std::uint32_t kTank = 1, kHealer = 2, kDps = 4;

struct Queue
{
    std::uint32_t MapId = 0;
    std::uint32_t QueuedSec = 0;
    std::vector<std::uint32_t> OpenRoles;   // bit flags per queue entry, as the event carries them
};

// The role to fill (kTank, kHealer or kDps, in that order of need), or 0 when the feature is off, the queue has not waited long enough, or no role is open.
std::uint32_t RoleToFill(Settings const& settings, Queue const& queue);

// One bot per queue per cooldown.
class Limiter
{
public:
    bool Ready(std::uint32_t map, std::uint64_t nowMs, std::uint32_t cooldownSec) const;
    void Note(std::uint32_t map, std::uint64_t nowMs) { _last[map] = nowMs ? nowMs : 1; }

private:
    std::map<std::uint32_t, std::uint64_t> _last;
};

}  // namespace lfg_queue_fill
