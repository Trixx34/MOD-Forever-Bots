#include "crafting_foci.h"

#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>

namespace crafting {

namespace {

constexpr std::uint32_t kSkillBlacksmithing = 164;
constexpr std::uint32_t kSkillCooking = 185;
constexpr std::uint32_t kSkillMining = 186;
constexpr std::uint32_t kSkillEngineering = 202;

std::vector<std::string_view> Split(std::string_view text, char separator)
{
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true)
    {
        std::size_t const at = text.find(separator, start);
        if (at == std::string_view::npos)
        {
            parts.push_back(text.substr(start));
            return parts;
        }
        parts.push_back(text.substr(start, at - start));
        start = at + 1;
    }
}

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

std::uint32_t FocusOfSkill(std::uint32_t skill)
{
    switch (skill)
    {
        case kSkillBlacksmithing:
        case kSkillEngineering:
            return kFocusAnvil;
        case kSkillMining:
            return kFocusForge;
        case kSkillCooking:
            return kFocusCookingFire;
        default:
            return 0;
    }
}

std::vector<FocusPoint> ParseFociTable(std::string_view text, std::vector<std::string>& errors)
{
    std::vector<FocusPoint> points;
    std::size_t lineNumber = 0;
    for (std::string_view line : Split(text, '\n'))
    {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line.empty() || line.front() == '#')
            continue;
        auto const fail = [&](char const* why) { errors.push_back("line " + std::to_string(lineNumber) + ": " + why); };
        std::vector<std::string_view> const f = Split(line, '|');
        if (f.size() < 7)
        {
            fail("expected city|map|focus|x y z|radius|entry|name");
            continue;
        }
        FocusPoint point;
        point.City = std::string(f[0]);
        if (point.City.empty() || !ParseUnsigned(f[1], point.Map) || !ParseUnsigned(f[2], point.Focus) || point.Focus == 0)
        {
            fail("bad city, map or focus");
            continue;
        }
        std::istringstream xyz{ std::string(f[3]) };
        std::string rest;
        if (!(xyz >> point.X >> point.Y >> point.Z) || (xyz >> rest))
        {
            fail("position must be three numbers");
            continue;
        }
        if (!ParseUnsigned(f[4], point.Radius) || !ParseUnsigned(f[5], point.Entry))
        {
            fail("bad radius or entry");
            continue;
        }
        point.Name = std::string(f[6]);
        points.push_back(std::move(point));
    }
    return points;
}

std::optional<std::size_t> NearestFocus(std::vector<FocusPoint> const& points, std::uint32_t focus, std::uint32_t map, float x, float y)
{
    std::optional<std::size_t> best;
    float bestDistance = 0.0f;
    for (std::size_t i = 0; i < points.size(); ++i)
    {
        FocusPoint const& point = points[i];
        if (point.Focus != focus || point.Map != map)
            continue;
        float const distance = std::hypot(point.X - x, point.Y - y);
        if (!best || distance < bestDistance)
        {
            best = i;
            bestDistance = distance;
        }
    }
    return best;
}

bool InReach(FocusPoint const& point, std::uint32_t map, float x, float y)
{
    return point.Map == map && std::hypot(point.X - x, point.Y - y) <= float(point.Radius);
}

} // namespace crafting
