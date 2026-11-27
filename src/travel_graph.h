// SPDX-License-Identifier: GPL-2.0-or-later
// TravelGraph (default off; doc/travel-graph.md): the ways bots have walked, as a graph of 40 yard cells per map. A long walk the core's navmesh finds no full route
// for is then taken leg by leg along ways another bot (or this one) has walked before. Held in memory only: it is learned again after a restart.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace travel_graph
{
constexpr float kCellYards = 40.0f;
constexpr float kStepMinYards = 25.0f;          // an observed step shorter than this is held until the bot is further on
constexpr float kStepMaxYards = 100.0f;         // a longer one is a teleport or a gap in the observations: dropped
constexpr float kStepMaxYardsPerSec = 20.0f;    // faster than any mount: a flight, a boat or a teleport
constexpr float kReverseMaxDz = 12.0f;          // a step that climbs or drops more than this is kept one way only (a jump down)
constexpr float kStartReachYards = 60.0f;       // the bot must be this near a known cell
constexpr float kGoalReachYards = 40.0f;
constexpr float kMinGainYards = 100.0f;         // the known way must end at least this much nearer the destination than the bot is
constexpr float kLegYards = 200.0f;
constexpr std::size_t kMaxNodes = 200000;
constexpr std::size_t kMaxExpanded = 40000;
constexpr std::size_t kMaxEdges = 12;

struct Pos
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

struct Leg
{
    Pos Point;                  // walk here next
    float Remaining = 0.0f;     // the known way from the bot to its end, plus the straight rest
};

class Graph
{
public:
    // One look at a bot. `walking`: alive, on its own feet or mount (a ghost walks on water, so it is not learned from).
    void Observe(std::uint64_t bot, std::uint32_t map, Pos at, std::uint64_t nowMs, bool walking);
    // The next leg from `from` towards `to` along known ways; none when the bot is off the known ways or they lead no nearer.
    std::optional<Leg> Next(std::uint32_t map, Pos from, Pos to) const;
    std::size_t Nodes() const { return _nodes.size(); }

private:
    struct Node
    {
        Pos At;
        std::vector<std::uint64_t> Edges;
    };
    struct Anchor
    {
        std::uint32_t Map = 0;
        Pos At;
        std::uint64_t Ms = 0;
    };

    static std::uint64_t Key(std::uint32_t map, float x, float y);
    void Step(std::uint32_t map, Pos from, Pos to);

    std::unordered_map<std::uint64_t, Node> _nodes;
    std::unordered_map<std::uint64_t, Anchor> _anchors;
};
}
