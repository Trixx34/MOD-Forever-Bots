// SPDX-License-Identifier: GPL-2.0-or-later
#include "taxi_policy.h"

#include <cmath>
#include <string>

namespace taxi {

namespace {
float Dist(float ax, float ay, float bx, float by)
{
    return std::hypot(ax - bx, ay - by);
}
}

Move Decide(Settings const& settings, Facts const& facts, float destX, float destY, std::uint64_t nowMs, std::uint64_t blockedUntilMs,
            std::uint64_t waitUntilMs)
{
    if (!settings.Enabled)
        return Move::Walk;
    if (facts.InFlight || nowMs < waitUntilMs)
        return Move::Wait;
    if (facts.InCombat || !facts.Alive || !facts.MasterValid || nowMs < blockedUntilMs)
        return Move::Walk;

    float const walk = Dist(facts.X, facts.Y, destX, destY);
    if (walk < settings.MinWalkYards)
        return Move::Walk;

    float const toMaster = Dist(facts.X, facts.Y, facts.MasterX, facts.MasterY);
    if (toMaster <= settings.ReachYards)
        return Move::Fly;
    // A flight master that is too far, or that lies behind the bot (the detour to it would be more than half of the walk), is not worth the trip.
    if (toMaster > settings.MasterMaxYards || toMaster > walk * 0.5f)
        return Move::Walk;
    // And one that is farther from the destination than the bot is would take it the wrong way.
    if (Dist(facts.MasterX, facts.MasterY, destX, destY) >= walk)
        return Move::Walk;
    return Move::WalkToMaster;
}

std::string PlaceName(std::vector<Place> const& places, std::uint32_t map, float x, float y, float maxYards)
{
    Place const* best = nullptr;
    float bestYards = maxYards;
    for (Place const& place : places)
    {
        float const yards = Dist(place.X, place.Y, x, y);
        if (place.Map == map && yards <= bestYards)
        {
            best = &place;
            bestYards = yards;
        }
    }
    return best ? best->Name : "unknown";
}

namespace {
std::string CityKey(Flight const& flight)
{
    return " city=" + (flight.Place.empty() ? std::string("unknown") : flight.Place);
}
}

Progress Track(Flight& flight, bool inFlight, std::uint64_t nowMs, std::uint64_t waitMs, std::uint64_t maxFlightMs)
{
    if (flight.OrderedMs == 0)
        return Progress::None;
    if (inFlight)
    {
        if (flight.TookOffMs == 0)
            flight.TookOffMs = nowMs;
        if (nowMs - flight.TookOffMs >= maxFlightMs)
            return Progress::TooLong;
        return Progress::None;
    }
    if (flight.TookOffMs != 0)
        return Progress::Landed;
    if (nowMs >= flight.OrderedMs + waitMs)
        return Progress::NeverStarted;
    return Progress::None;
}

std::uint64_t Seconds(std::uint64_t fromMs, std::uint64_t toMs)
{
    return toMs > fromMs ? (toMs - fromMs) / 1000 : 0;
}

std::string OrderLine(std::uint64_t bot, std::uint32_t level, std::uint32_t map, int walkYards, Flight const& flight)
{
    return "taxi: bot=" + std::to_string(bot) + " level=" + std::to_string(level) + " map=" + std::to_string(map) + " walk=" + std::to_string(walkYards) +
           " to_x=" + std::to_string(int(flight.DestX)) + " to_y=" + std::to_string(int(flight.DestY)) + " master=" + std::to_string(flight.Master) + CityKey(flight);
}

std::string LandedLine(std::uint64_t bot, std::uint32_t map, float x, float y, Flight const& flight, std::uint64_t nowMs)
{
    return "taxi_landed: bot=" + std::to_string(bot) + " map=" + std::to_string(map) + " x=" + std::to_string(int(x)) + " y=" + std::to_string(int(y)) +
           " to_x=" + std::to_string(int(flight.DestX)) + " to_y=" + std::to_string(int(flight.DestY)) + " took_s=" + std::to_string(Seconds(flight.OrderedMs, nowMs)) + CityKey(flight);
}

std::string TimeoutLine(std::uint64_t bot, std::uint32_t map, Progress why, Flight const& flight, std::uint64_t nowMs)
{
    return "taxi_timeout: bot=" + std::to_string(bot) + " map=" + std::to_string(map) + " reason=" + (why == Progress::TooLong ? "too_long" : "not_started") +
           " waited_s=" + std::to_string(Seconds(flight.OrderedMs, nowMs)) + CityKey(flight);
}

std::string RefusedLine(std::uint64_t bot, std::uint32_t map, char const* status)
{
    return "taxi_refused: bot=" + std::to_string(bot) + " map=" + std::to_string(map) + " status=" + status;
}

} // namespace taxi
