// SPDX-License-Identifier: GPL-2.0-or-later
// Capital inspection (config CityInspect, off by default): an idle bot in a capital that sees a player within 20 yards wearing a higher-level item than
// its own now and then salutes, applauds or cheers at the player. Pure logic, unit-tested in tests/city_inspect_test.cpp. Limits: one bot per player per
// 5 minutes, CityInspectZoneMax emotes per map per minute.
#pragma once

#include "realism.h"

#include <cstdint>
#include <map>

namespace city_inspect {

struct Settings
{
    bool Enabled = false;                 // CityInspect
    std::uint32_t ChancePct = 5;          // CityInspectChancePct
    std::uint32_t ZoneMax = 2;            // CityInspectZoneMax: emotes a minute over all bots of one map
    float RangeYards = 20.0f;
    std::uint64_t PlayerCooldownMs = 5ull * 60 * 1000;
};

struct Facts
{
    bool Idle = false;                    // out of combat, no target, no hostile in view, not moving on an order
    bool InCapital = false;
    std::uint32_t BotItemLevel = 0;       // the average item level of what the bot wears
    std::uint32_t PlayerItemLevel = 0;    // the best item the player wears (0: not known, then nothing happens)
    float Distance = 1e9f;
};

enum class Emote : std::uint32_t { Salute = 0, Applaud = 1, Cheer = 2 };   // the values of EmoteAction::EmoteKind

class Inspector
{
public:
    // True and the emote when this bot may emote at this player now. Records the use of the limits only when it returns true.
    bool Try(Settings const& settings, Facts const& facts, std::uint64_t seed, std::uint64_t bot, std::uint64_t player, std::uint32_t map, std::uint64_t nowMs,
             Emote& emote);

private:
    std::map<std::uint64_t, std::uint64_t> _playerMs;   // player -> last time any bot emoted at it
    realism::ZoneRate _zone;
    std::uint32_t _counter = 0;
};

}  // namespace city_inspect
