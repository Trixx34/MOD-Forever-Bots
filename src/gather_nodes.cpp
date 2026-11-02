// SPDX-License-Identifier: GPL-2.0-or-later
#include "gather_nodes.h"

#include "text_split.h"

#include <cmath>
#include <limits>
#include <sstream>

namespace gather {

namespace {

bool ParseUnsigned(std::string_view text, std::uint32_t& out)
{
    if (text.empty() || text.size() > 10)
        return false;
    std::uint64_t value = 0;
    for (char c : text)
    {
        if (c < '0' || c > '9')
            return false;
        value = value * 10 + std::uint64_t(c - '0');
    }
    if (value > std::numeric_limits<std::uint32_t>::max())
        return false;
    out = std::uint32_t(value);
    return true;
}

} // namespace

std::vector<NodePoint> ParseNodeTable(std::string_view text, std::vector<std::string>& errors)
{
    std::vector<NodePoint> nodes;
    std::size_t lineNumber = 0;
    for (std::string_view line : text_split::Split(text, '\n'))
    {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line.empty() || line.front() == '#')
            continue;
        auto const fail = [&](char const* why) { errors.push_back("line " + std::to_string(lineNumber) + ": " + why); };
        std::vector<std::string_view> const f = text_split::Split(line, ' ');
        if (f.size() != 6)
        {
            fail("expected skill map x y z entry");
            continue;
        }
        NodePoint node;
        if (!ParseUnsigned(f[0], node.Skill) || node.Skill == 0 || !ParseUnsigned(f[1], node.Map) || !ParseUnsigned(f[5], node.Entry))
        {
            fail("bad skill, map or entry");
            continue;
        }
        std::istringstream xyz{ std::string(f[2]) + " " + std::string(f[3]) + " " + std::string(f[4]) };
        std::string rest;
        if (!(xyz >> node.X >> node.Y >> node.Z) || (xyz >> rest))
        {
            fail("position must be three numbers");
            continue;
        }
        nodes.push_back(node);
    }
    return nodes;
}

std::optional<std::size_t> NearestNode(std::vector<NodePoint> const& nodes, std::uint32_t skill, std::uint32_t map, float x, float y)
{
    std::optional<std::size_t> best;
    float bestDistance = 0.0f;
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        NodePoint const& node = nodes[i];
        if (node.Skill != skill || node.Map != map)
            continue;
        float const distance = std::hypot(node.X - x, node.Y - y);
        if (!best || distance < bestDistance)
        {
            best = i;
            bestDistance = distance;
        }
    }
    return best;
}

} // namespace gather
