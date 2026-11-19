// SPDX-License-Identifier: GPL-2.0-or-later
#include "below_map.h"

#include <algorithm>

namespace below_map
{

namespace
{
constexpr std::size_t kMaxNoRoutes = 64;
constexpr float kArrivedYards = 6.0f;

void Prune(Options const& options, Watch& watch, std::uint64_t nowMs)
{
    std::erase_if(watch.NoRoutes, [&](std::uint64_t at) { return nowMs - at > options.StormWindowMs; });
}

// The sign starts over: the drop must persist again, the storm's answers are dropped.
void Reset(Watch& watch)
{
    watch.BelowSinceMs = 0;
    watch.BelowChecks = 0;
    watch.NoRoutes.clear();
}
} // namespace

bool IsSkip(Verdict verdict)
{
    return verdict == Verdict::kSkipTransport || verdict == Verdict::kSkipSwimming || verdict == Verdict::kSkipOnPath || verdict == Verdict::kSkipNearGround;
}

bool IsSend(Verdict verdict)
{
    return verdict == Verdict::kSendBelow || verdict == Verdict::kSendStorm;
}

void NoteNoRoute(Options const& options, Watch& watch, float toDestYards, std::uint64_t nowMs)
{
    if (!options.Enabled || toDestYards < kArrivedYards || toDestYards >= options.TinyYards)
        return;
    Prune(options, watch, nowMs);
    if (watch.NoRoutes.size() >= kMaxNoRoutes)
        watch.NoRoutes.erase(watch.NoRoutes.begin());
    watch.NoRoutes.push_back(nowMs);
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
    bool const below = facts.Alive && facts.HasGround && facts.GroundZ - facts.Z > options.DropYards;
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
    else if (recentWalk && !watch.NoRoutes.empty() && watch.RoutedMs >= watch.NoRoutes.front())
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
    }
    return "?";
}

} // namespace below_map
