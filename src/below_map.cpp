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
} // namespace

void NoteNoRoute(Options const& options, Watch& watch, float toDestYards, std::uint64_t nowMs)
{
    if (!options.Enabled || toDestYards < kArrivedYards || toDestYards >= options.TinyYards)
        return;
    Prune(options, watch, nowMs);
    if (watch.NoRoutes.size() >= kMaxNoRoutes)
        watch.NoRoutes.erase(watch.NoRoutes.begin());
    watch.NoRoutes.push_back(nowMs);
}

Verdict Check(Options const& options, Watch& watch, Facts const& facts, std::uint64_t nowMs)
{
    if (!options.Enabled)
        return Verdict::kFine;
    Prune(options, watch, nowMs);
    bool const below = facts.Alive && facts.HasGround && facts.GroundZ - facts.Z > options.DropYards;
    if (below)
    {
        if (watch.BelowSinceMs == 0)
            watch.BelowSinceMs = nowMs;
    }
    else
        watch.BelowSinceMs = 0;
    bool const belowLong = below && nowMs - watch.BelowSinceMs >= options.BelowForMs;
    bool const storm = options.StormCount != 0 && watch.NoRoutes.size() >= options.StormCount;
    if (!belowLong && !storm)
        return below ? Verdict::kWaiting : Verdict::kFine;
    if (!facts.Alive || facts.InCombat || facts.Travelling || facts.Held)
        return Verdict::kBlocked;
    if (nowMs < watch.NextTryMs)
        return Verdict::kCooldown;
    watch.NextTryMs = nowMs + options.CooldownMs;
    watch.NoRoutes.clear();
    watch.BelowSinceMs = 0;
    return belowLong ? Verdict::kSendBelow : Verdict::kSendStorm;
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
    }
    return "?";
}

} // namespace below_map
