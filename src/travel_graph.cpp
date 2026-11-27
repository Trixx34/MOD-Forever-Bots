// SPDX-License-Identifier: GPL-2.0-or-later
#include "travel_graph.h"

#include <algorithm>
#include <cmath>
#include <queue>

namespace travel_graph
{
namespace
{
float Flat(Pos a, Pos b)
{
    return std::hypot(a.X - b.X, a.Y - b.Y);
}
}

std::uint64_t Graph::Key(std::uint32_t map, float x, float y)
{
    auto const cell = [](float v) { return std::uint64_t(std::int64_t(std::floor(v / kCellYards)) + 0x80000) & 0xFFFFF; };
    return (std::uint64_t(map) << 40) | (cell(x) << 20) | cell(y);
}

void Graph::Observe(std::uint64_t bot, std::uint32_t map, Pos at, std::uint64_t nowMs, bool walking)
{
    if (!walking)
    {
        _anchors.erase(bot);
        return;
    }
    auto const [it, added] = _anchors.try_emplace(bot, Anchor{ map, at, nowMs });
    if (added)
        return;
    Anchor& last = it->second;
    float const yards = Flat(last.At, at);
    float const seconds = float(nowMs - last.Ms) / 1000.0f;
    bool const broken = last.Map != map || yards > kStepMaxYards || (yards >= kStepMinYards && yards > kStepMaxYardsPerSec * seconds);
    if (!broken && yards < kStepMinYards)
    {
        // A bot that stands does not age its anchor out of the speed check.
        if (yards < 1.0f)
            last.Ms = nowMs;
        return;
    }
    if (!broken)
        Step(map, last.At, at);
    last = Anchor{ map, at, nowMs };
}

void Graph::Step(std::uint32_t map, Pos from, Pos to)
{
    std::uint64_t const a = Key(map, from.X, from.Y);
    std::uint64_t const b = Key(map, to.X, to.Y);
    if (a == b || (_nodes.size() + 2 > kMaxNodes && (!_nodes.count(a) || !_nodes.count(b))))
        return;
    _nodes.try_emplace(a, Node{ from, {} });
    _nodes.try_emplace(b, Node{ to, {} });
    auto const link = [this](std::uint64_t x, std::uint64_t y)
    {
        std::vector<std::uint64_t>& edges = _nodes[x].Edges;
        if (edges.size() < kMaxEdges && std::find(edges.begin(), edges.end(), y) == edges.end())
            edges.push_back(y);
    };
    link(a, b);
    // The cells' own heights count too: a drop inside one cell would otherwise not be seen.
    if (std::fabs(from.Z - to.Z) <= kReverseMaxDz && std::fabs(_nodes[a].At.Z - _nodes[b].At.Z) <= kReverseMaxDz)
        link(b, a);
}

std::optional<Leg> Graph::Next(std::uint32_t map, Pos from, Pos to) const
{
    // The nearest known cell around the bot.
    std::uint64_t start = 0;
    float startYards = kStartReachYards;
    for (int dx = -2; dx <= 2; ++dx)
        for (int dy = -2; dy <= 2; ++dy)
        {
            auto const node = _nodes.find(Key(map, from.X + float(dx) * kCellYards, from.Y + float(dy) * kCellYards));
            if (node != _nodes.end() && Flat(node->second.At, from) <= startYards)
            {
                start = node->first;
                startYards = Flat(node->second.At, from);
            }
        }
    if (start == 0)
        return std::nullopt;

    struct Seen
    {
        float Walked = 0.0f;
        std::uint64_t Parent = 0;
    };
    std::unordered_map<std::uint64_t, Seen> seen{ { start, Seen{ startYards, 0 } } };
    using Open = std::pair<float, std::uint64_t>; // walked + straight rest, node
    std::priority_queue<Open, std::vector<Open>, std::greater<Open>> open;
    open.emplace(startYards + Flat(_nodes.at(start).At, to), start);
    std::uint64_t best = start;
    float bestRest = Flat(_nodes.at(start).At, to);
    std::size_t expanded = 0;
    while (!open.empty() && expanded < kMaxExpanded && bestRest > kGoalReachYards)
    {
        std::uint64_t const key = open.top().second;
        open.pop();
        ++expanded;
        Node const& node = _nodes.at(key);
        float const walked = seen.at(key).Walked;
        for (std::uint64_t const next : node.Edges)
        {
            Node const& other = _nodes.at(next);
            float const total = walked + Flat(node.At, other.At);
            auto const found = seen.find(next);
            if (found != seen.end() && found->second.Walked <= total)
                continue;
            seen[next] = Seen{ total, key };
            float const rest = Flat(other.At, to);
            open.emplace(total + rest, next);
            if (rest < bestRest)
            {
                best = next;
                bestRest = rest;
            }
        }
    }
    // Short of the destination, the known way must be worth it; a way that reaches it always is.
    if (bestRest > kGoalReachYards && bestRest > Flat(from, to) - kMinGainYards)
        return std::nullopt;

    // Back along the way to the furthest cell within one leg of the bot.
    std::uint64_t at = best;
    while (seen.at(at).Walked > kLegYards && seen.at(at).Parent != 0 && seen.at(at).Parent != start)
        at = seen.at(at).Parent;
    if (Flat(_nodes.at(at).At, from) < kStepMinYards)
        return std::nullopt;
    return Leg{ _nodes.at(at).At, seen.at(best).Walked + bestRest };
}
}
