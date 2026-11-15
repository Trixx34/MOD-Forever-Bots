// SPDX-License-Identifier: GPL-2.0-or-later
// Meeting greeting (config MeetGreet, off by default): a bot that is out of combat, not in a group, and passes a player or another bot within 15 yards
// anywhere in the world (the capitals have CityGreet) now and then waves or bows, says a short /say hello, or both. Pure logic, unit-tested in
// tests/meet_greet_test.cpp; BotPolicy decides when to ask, the advisor orders send the lines (Kind::Say, Kind::Emote).
//
// Limits (all of them must allow it): a chance per look, one greeting per bot per MeetGreetBotCooldownSec, one greeting per pair (either order) per
// MeetGreetPairCooldownSec so two bots never greet each other back and forth, MeetGreetZoneMax greetings per map per minute. A line is not
// repeated by the same bot within its last 8 lines, nor by anyone within the last 24 lines said. The maps are bounded.
#pragma once

#include "realism.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>

namespace meet_greet {

struct Settings
{
    bool Enabled = false;                          // MeetGreet
    std::uint32_t ChancePct = 3;                   // MeetGreetChancePct: per look of an eligible bot at a unit in range
    std::uint32_t ZoneMax = 2;                     // MeetGreetZoneMax: greetings a minute over all bots of one map
    std::uint32_t BotCooldownSec = 600;            // MeetGreetBotCooldownSec: the least time between two greetings of one bot
    std::uint32_t PairCooldownSec = 1800;          // MeetGreetPairCooldownSec: the least time between two greetings of the same two characters
    std::uint32_t WavePct = 40;                    // MeetGreetWavePct: of the greetings, the share that is an emote (a wave, now and then a bow) with no words
    std::uint32_t SayPct = 100;                    // MeetGreetSayPct: of the others, the share that also gets words; the rest is an emote alone
    float RangeYards = 15.0f;
};

struct Facts
{
    bool Quiet = false;        // out of combat, no target, no hostile in view, alive
    bool Free = false;         // not in a group, not in a capital with CityGreet on
    float Distance = 1e9f;     // to the other character
};

struct Greeting
{
    bool Say = false;
    bool Emote = false;
    std::uint32_t EmoteKind = 3;   // EmoteAction::EmoteKind: 3 wave, 4 bow
    std::uint32_t Draw = 0;        // random number for the line
};

class Greeter
{
public:
    // True when `bot` may greet `other` (a player or a bot, guid counters) now; fills `out`. Records the limits only when it returns true.
    bool Try(Settings const& settings, Facts const& facts, std::uint64_t seed, std::uint64_t bot, std::uint64_t other, std::uint32_t map, std::uint64_t nowMs,
             Greeting& out);

    // The index of the line this bot says out of `poolSize`, starting at `draw` and skipping lines recently said (by the bot, or by anyone).
    // Remembers the choice. When every line was said recently the one nearest to `draw` that the bot did not say last is used.
    std::size_t PickLine(std::uint64_t bot, std::size_t poolSize, std::uint32_t draw);

    std::size_t Tracked() const { return _pairMs.size() + _botMs.size() + _botLines.size(); }

    static constexpr std::size_t kMaxTracked = 4096;
    static constexpr std::size_t kBotMemory = 8;
    static constexpr std::size_t kGlobalMemory = 24;

private:
    void Prune(Settings const& settings, std::uint64_t nowMs);

    std::map<std::uint64_t, std::uint64_t> _pairMs;                 // pair key -> last greeting
    std::map<std::uint64_t, std::uint64_t> _botMs;                  // bot -> last greeting
    std::map<std::uint64_t, std::deque<std::size_t>> _botLines;     // bot -> its last line indexes
    std::deque<std::size_t> _recent;                                // the last line indexes said by anyone
    realism::ZoneRate _zone;
    std::uint32_t _counter = 0;
};

}  // namespace meet_greet
