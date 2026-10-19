// Flight points (layout 17): when a bot's long walk is replaced by a flight. Pure logic with no SDK dependency, unit-tested in tests/taxi_test.cpp.
// Bots know every flight node and fly free (the core's Bot.Taxi.*), so the only questions are whether the walk is long enough to be worth it, whether a
// flight master is close enough to be on the way, and when to give up after the core refused a flight. The core makes the last decision (a flight
// must land the bot Bot.Taxi.MinGainYards closer); this rule only keeps the bot from walking to a flight master for nothing.
#pragma once

#include <cstdint>

namespace taxi {

struct Settings
{
    bool Enabled = false;                       // config key Taxi (off by default)
    float MinWalkYards = 1500.0f;               // a walk shorter than this is walked (TaxiMinWalkYards)
    float MasterMaxYards = 600.0f;              // a flight master farther than this from the bot is not walked to (TaxiMasterMaxYards)
    float ReachYards = 4.0f;                    // within this of the flight master the bot can use it (the core allows 5)
    std::uint64_t BlockedMs = 600000;           // after a refused flight the bot walks for this long (TaxiBlockedSec)
    std::uint64_t WaitMs = 8000;                // after a flight order the bot is given this long to show it is on the flight
};

struct Facts
{
    float X = 0.0f;
    float Y = 0.0f;
    bool InFlight = false;
    bool InCombat = false;
    bool Alive = true;
    bool MasterValid = false;                   // the snapshot names a flight master the bot can walk to
    float MasterX = 0.0f;
    float MasterY = 0.0f;
};

enum class Move : std::uint8_t
{
    Walk,           // go on walking to the destination
    WalkToMaster,   // walk to the flight master first
    Fly,            // send the flight order now
    Wait            // a flight is under way or was just ordered: send nothing
};

// What to do about a walk to (destX, destY). `blockedUntilMs` is when a refused flight stops being held against flying; `waitUntilMs` when an order
// just sent may still be on its way.
Move Decide(Settings const& settings, Facts const& facts, float destX, float destY, std::uint64_t nowMs, std::uint64_t blockedUntilMs,
            std::uint64_t waitUntilMs);

} // namespace taxi
