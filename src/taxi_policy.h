// Flight points (layout 17): when a bot's long walk is replaced by a flight. Pure logic with no SDK dependency, unit-tested in tests/taxi_test.cpp.
// Bots know every flight node and fly free (the core's Bot.Taxi.*), so the only questions are whether the walk is long enough to be worth it, whether a
// flight master is close enough to be on the way, and when to give up after the core refused a flight. The core makes the last decision (a flight
// must land the bot Bot.Taxi.MinGainYards closer); this rule only keeps the bot from walking to a flight master for nothing.
#pragma once

#include <cstdint>
#include <string>

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


// ---- the journal lines (OnEvent text, key=value; the "taxi:" line keeps its keys and order, new keys are appended) ----

// A flight that was ordered and has not ended yet. Zero OrderedMs: none.
struct Flight
{
    std::uint64_t OrderedMs = 0;
    std::uint64_t TookOffMs = 0;                // first time the bot was seen in flight (0: not yet)
    float DestX = 0.0f;
    float DestY = 0.0f;
    std::uint64_t Master = 0;                   // guid of the flight master the order named
};

enum class Progress : std::uint8_t
{
    None,           // nothing to say
    Landed,         // was in flight and is not any more
    NeverStarted,   // no flight showed within WaitMs of the order
    TooLong         // in flight for longer than MaxFlightMs
};

constexpr std::uint64_t kMaxFlightMs = 1800000;  // 30 minutes: longer than any flight; then the flight is written off

// Advances `flight` with what the snapshot shows now and says whether it ended (the caller then clears it). Call once per tick per bot.
Progress Track(Flight& flight, bool inFlight, std::uint64_t nowMs, std::uint64_t waitMs, std::uint64_t maxFlightMs = kMaxFlightMs);

// Whole seconds from `fromMs` to `toMs` (0 when toMs is earlier).
std::uint64_t Seconds(std::uint64_t fromMs, std::uint64_t toMs);

// "taxi: bot=G level=L map=M walk=D" and the appended keys " to_x=X to_y=Y master=GUID".
std::string OrderLine(std::uint64_t bot, std::uint32_t level, std::uint32_t map, int walkYards, Flight const& flight);
// "taxi_landed: bot=G map=M x=X y=Y to_x=DX to_y=DY took_s=N" (took_s counts from the order).
std::string LandedLine(std::uint64_t bot, std::uint32_t map, float x, float y, Flight const& flight, std::uint64_t nowMs);
// "taxi_timeout: bot=G map=M reason=not_started|too_long waited_s=N"
std::string TimeoutLine(std::uint64_t bot, std::uint32_t map, Progress why, Flight const& flight, std::uint64_t nowMs);
// "taxi_refused: bot=G map=M status=NAME" (the core refused the order; the bot walks)
std::string RefusedLine(std::uint64_t bot, std::uint32_t map, char const* status);

} // namespace taxi
