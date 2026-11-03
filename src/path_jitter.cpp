// SPDX-License-Identifier: GPL-2.0-or-later
#include "path_jitter.h"

#include "bot_random.h"

#include <cmath>

namespace path_jitter {
namespace {

float Dist(Pt a, Pt b) { return std::hypot(a.X - b.X, a.Y - b.Y); }

// Distance from p to the segment a-b.
float SegmentDist(Pt p, Pt a, Pt b)
{
    float const dx = b.X - a.X, dy = b.Y - a.Y;
    float const len2 = dx * dx + dy * dy;
    float t = len2 > 0.0f ? ((p.X - a.X) * dx + (p.Y - a.Y) * dy) / len2 : 0.0f;
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    return Dist(p, Pt{ a.X + t * dx, a.Y + t * dy });
}

}  // namespace

Pt Apply(Settings const& settings, Facts const& facts, Pt target, std::uint64_t seed, std::uint64_t bot)
{
    if (!settings.Enabled || settings.Yards <= 0.0f || facts.InCombat || facts.HasTarget || facts.EnemyNear || facts.InDungeon || !facts.Walking)
        return target;
    // Keyed on the target so a bot that re-issues the same move keeps the same offset.
    std::uint64_t const key = std::uint64_t(std::int64_t(target.X * 4.0f)) * 1000003ull + std::uint64_t(std::int64_t(target.Y * 4.0f));
    double const angle = bot_random::Unit(bot_random::Draw(seed, bot, 0xD1, key)) * 6.283185307179586;
    float const radius = float(bot_random::Unit(bot_random::Draw(seed, bot, 0xD2, key))) * settings.Yards;
    Pt const moved{ target.X + float(std::cos(angle)) * radius, target.Y + float(std::sin(angle)) * radius };
    if (facts.Unwalkable)
        for (Pt const& bad : *facts.Unwalkable)
            if (Dist(moved, bad) < settings.KeepClearYards)
                return target;
    if (facts.Required)
        for (Pt const& must : *facts.Required)
            // The old line from the bot to the target passes the waypoint, or the new one does not: the jittered move would skip it.
            if (SegmentDist(must, facts.From, target) < settings.KeepClearYards && SegmentDist(must, facts.From, moved) >= settings.KeepClearYards)
                return target;
    return moved;
}

}  // namespace path_jitter
