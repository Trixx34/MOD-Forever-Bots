// SPDX-License-Identifier: GPL-2.0-or-later
#include "below_map.h"

#include <algorithm>
#include <cmath>

namespace below_map
{

namespace
{
constexpr std::size_t kMaxNoRoutes = 64;
constexpr float kArrivedYards = 6.0f;

void Prune(Options const& options, Watch& watch, std::uint64_t nowMs)
{
    std::erase_if(watch.NoRoutes, [&](NoRoute const& at) { return nowMs - at.AtMs > options.StormWindowMs; });
}

// The sign starts over: the drop must persist again, the storm's answers are dropped.
void Reset(Watch& watch)
{
    watch.LastChecks = watch.BelowChecks;
    watch.BelowSinceMs = 0;
    watch.BelowChecks = 0;
    watch.NoRoutes.clear();
}
} // namespace

Ground FindGround(std::vector<float> const& placeZs, float z, float floorBandYards)
{
    Ground ground;
    if (placeZs.empty())
        return ground;
    ground.HasGround = true;
    ground.Places = std::uint32_t(placeZs.size());
    ground.LowZ = placeZs.front();
    ground.HighZ = placeZs.front();
    for (std::size_t i = 0; i < placeZs.size(); ++i)
    {
        float const placeZ = placeZs[i];
        ground.LowZ = std::min(ground.LowZ, placeZ);
        ground.HighZ = std::max(ground.HighZ, placeZ);
        float const dz = std::fabs(placeZ - z);
        // The closest floor to the bot's height; on a tie the lower one (the floor under the bot rather than the one over it).
        if (i == 0 || dz < ground.NearestDz || (dz == ground.NearestDz && placeZ < ground.GroundZ))
        {
            ground.Index = i;
            ground.GroundZ = placeZ;
            ground.NearestDz = dz;
        }
    }
    // Floors: the heights sorted, a new floor wherever the gap to the one below is more than the band.
    std::vector<float> sorted = placeZs;
    std::sort(sorted.begin(), sorted.end());
    ground.Floors = 1;
    for (std::size_t i = 1; i < sorted.size(); ++i)
        if (sorted[i] - sorted[i - 1] > floorBandYards)
            ++ground.Floors;
    return ground;
}

bool IsSkip(Verdict verdict)
{
    switch (verdict)
    {
        case Verdict::kSkipTransport:
        case Verdict::kSkipSwimming:
        case Verdict::kSkipOnPath:
        case Verdict::kSkipNearGround:
        case Verdict::kSkipMoving:
        case Verdict::kSkipFalling:
        case Verdict::kSkipFlying:
        case Verdict::kSkipFewPlaces:
            return true;
        default:
            return false;
    }
}

bool IsSend(Verdict verdict)
{
    return verdict == Verdict::kSendBelow || verdict == Verdict::kSendStorm;
}

void NoteNoRoute(Options const& options, Watch& watch, float toDestYards, float destX, float destY, float destZ, float botZ, std::uint64_t nowMs)
{
    if (!options.Enabled || toDestYards < kArrivedYards || toDestYards >= options.TinyYards)
        return;
    // A refused walk to a point at the bot's own height (an explore walk) or below it says nothing about a bot under the ground.
    if (options.StormRiseYards > 0.0f && destZ - botZ < options.StormRiseYards)
        return;
    Prune(options, watch, nowMs);
    // The same destination refused again (the retry of one point) is one answer, not many.
    if (options.StormDistinct && std::any_of(watch.NoRoutes.begin(), watch.NoRoutes.end(), [&](NoRoute const& at)
                                             { return std::hypot(at.X - destX, at.Y - destY) < options.StormSameYards; }))
        return;
    if (watch.NoRoutes.size() >= kMaxNoRoutes)
        watch.NoRoutes.erase(watch.NoRoutes.begin());
    watch.NoRoutes.push_back({ nowMs, destX, destY });
}

void NoteRouted(Options const& options, Watch& watch, float z, std::uint64_t nowMs)
{
    if (!options.Enabled)
        return;
    watch.Routed = true;
    watch.RoutedMs = nowMs;
    watch.RoutedZ = z;
}

Verdict Check(Options const& options, Watch& watch, Facts const& facts, std::uint64_t nowMs)
{
    if (!options.Enabled)
        return Verdict::kFine;
    Prune(options, watch, nowMs);
    if (facts.Falling && !watch.Falling)
        watch.FallingSinceMs = nowMs;
    watch.Falling = facts.Falling;
    bool const lowZ = facts.Alive && facts.HasGround && facts.GroundZ - facts.Z > options.DropYards;
    if (lowZ)
    {
        // 2026-11-20: a low z the bot's own state explains is not a fall under the map, and the drop must persist without it (the sign starts over).
        Verdict excuse = Verdict::kFine;
        bool const longFall = facts.Falling && nowMs - watch.FallingSinceMs >= options.FallGraceMs;
        if (facts.Places < std::max<std::uint32_t>(1, options.MinPlaces))
            excuse = Verdict::kSkipFewPlaces;
        else if (facts.Flying)
            excuse = Verdict::kSkipFlying;
        else if (facts.Falling && !longFall)
            excuse = Verdict::kSkipFalling;
        else if (options.RequireStill && facts.Moving && !longFall)
            excuse = Verdict::kSkipMoving;
        if (excuse != Verdict::kFine)
        {
            watch.LastChecks = watch.BelowChecks;
            watch.BelowSinceMs = 0;
            watch.BelowChecks = 0;
            watch.LastSkipBelow = true;
            return excuse;
        }
    }
    bool const below = lowZ;
    if (below)
    {
        if (watch.BelowChecks == 0)
            watch.BelowSinceMs = nowMs;
        ++watch.BelowChecks;
    }
    else
    {
        watch.BelowSinceMs = 0;
        watch.BelowChecks = 0;
    }
    bool const belowLong = below && nowMs - watch.BelowSinceMs >= options.BelowForMs && watch.BelowChecks >= std::max<std::uint32_t>(1, options.BelowChecks);
    bool const storm = options.StormCount != 0 && watch.NoRoutes.size() >= options.StormCount;
    if (!belowLong && !storm)
        return below ? Verdict::kWaiting : Verdict::kFine;
    if (!facts.Alive || facts.InCombat || facts.Travelling || facts.Held)
        return Verdict::kBlocked;
    // The false positives of arm C (below_map.h): a sign the bot's own state explains is not acted on.
    bool const recentWalk = watch.Routed && nowMs - watch.RoutedMs <= options.RoutedRecentMs && watch.RoutedZ - facts.Z <= options.RoutedDropYards;
    Verdict skip = Verdict::kFine;
    if (facts.OnTransport)
        skip = Verdict::kSkipTransport;
    else if (facts.Swimming)
        skip = Verdict::kSkipSwimming;
    else if (belowLong)
    {
        if (recentWalk && !facts.Falling)
            skip = Verdict::kSkipOnPath;
    }
    else if (facts.HasGround && facts.NearestDz <= options.NearGroundYards)
        skip = Verdict::kSkipNearGround;
    else if (recentWalk && !watch.NoRoutes.empty() && watch.RoutedMs >= watch.NoRoutes.front().AtMs)
        skip = Verdict::kSkipOnPath;   // a walk applied after the storm began: the bot is on the navmesh
    if (skip != Verdict::kFine)
    {
        watch.LastSkipBelow = belowLong;
        Reset(watch);
        return skip;
    }
    if (nowMs < watch.NextTryMs)
        return Verdict::kCooldown;
    watch.NextTryMs = nowMs + options.CooldownMs;
    Reset(watch);
    return belowLong ? Verdict::kSendBelow : Verdict::kSendStorm;
}

bool SkipLogDue(Options const& options, Watch& watch, std::uint64_t nowMs)
{
    if (watch.SkipLogged && nowMs - watch.SkipLoggedMs < options.SkipLogMs)
        return false;
    watch.SkipLogged = true;
    watch.SkipLoggedMs = nowMs;
    return true;
}

char const* VerdictName(Verdict verdict)
{
    switch (verdict)
    {
        case Verdict::kFine: return "fine";
        case Verdict::kWaiting: return "waiting";
        case Verdict::kBlocked: return "blocked";
        case Verdict::kCooldown: return "cooldown";
        case Verdict::kSendBelow: return "z_below";
        case Verdict::kSendStorm: return "no_route_storm";
        case Verdict::kSkipTransport: return "on_transport";
        case Verdict::kSkipSwimming: return "swimming";
        case Verdict::kSkipOnPath: return "on_path";
        case Verdict::kSkipNearGround: return "near_ground";
        case Verdict::kSkipMoving: return "moving";
        case Verdict::kSkipFalling: return "falling";
        case Verdict::kSkipFlying: return "flying";
        case Verdict::kSkipFewPlaces: return "few_places";
    }
    return "?";
}

} // namespace below_map
