#include "taxi_policy.h"

#include <cmath>

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

} // namespace taxi
