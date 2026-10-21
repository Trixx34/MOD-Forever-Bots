#include "ground_avoid.h"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace ground_avoid {

namespace {

constexpr int kDirections = 16;
constexpr float kStepYards = 1.0f;
constexpr float kMaxWalkYards = 40.0f;
constexpr float kPi = 3.14159265358979f;

bool ParseUint(std::string_view text, std::uint32_t& out)
{
    auto const result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc() && result.ptr == text.data() + text.size();
}

bool ParseFloat(std::string const& text, float& out)
{
    try
    {
        std::size_t used = 0;
        out = std::stof(text, &used);
        return used == text.size() && std::isfinite(out) && out >= 0.0f && out <= 200.0f;
    }
    catch (...)
    {
        return false;
    }
}

} // namespace

AvoidTable ParseAvoidSpells(std::string_view text, std::vector<std::string>& errors)
{
    AvoidTable table;
    std::size_t lineNo = 0, at = 0;
    while (at <= text.size())
    {
        std::size_t end = text.find('\n', at);
        if (end == std::string_view::npos)
            end = text.size();
        std::string_view line = text.substr(at, end - at);
        at = end + 1;
        ++lineNo;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.remove_suffix(1);
        if (line.empty() || line.front() == '#')
            continue;
        std::vector<std::string> field;
        std::size_t from = 0;
        while (true)
        {
            std::size_t bar = line.find('|', from);
            field.emplace_back(line.substr(from, bar == std::string_view::npos ? std::string_view::npos : bar - from));
            if (bar == std::string_view::npos)
                break;
            from = bar + 1;
        }
        std::uint32_t spell = 0;
        float radius = 0.0f;
        if (field.size() < 3 || !ParseUint(field[0], spell) || spell == 0 || !ParseFloat(field[1], radius))
        {
            errors.push_back("line " + std::to_string(lineNo) + ": expected 'spell id|radius|kind|...'");
            continue;
        }
        table.Add(spell, radius);
    }
    return table;
}

bool InsideAny(float x, float y, std::vector<Circle> const& circles)
{
    for (Circle const& c : circles)
        if (std::hypot(x - c.X, y - c.Y) < c.Radius + kInsideMarginYards)
            return true;
    return false;
}

std::optional<Point2> EscapePoint(float x, float y, std::vector<Circle> const& circles)
{
    if (!InsideAny(x, y, circles))
        return std::nullopt;

    // The direction straight away from the nearest center is tried first, so that a tie goes to leaving, not crossing, the effect.
    float awayAngle = 0.0f, nearest = 1e9f;
    for (Circle const& c : circles)
    {
        float const d = std::hypot(x - c.X, y - c.Y);
        if (d < nearest)
        {
            nearest = d;
            awayAngle = d > 0.01f ? std::atan2(y - c.Y, x - c.X) : 0.0f;
        }
    }

    auto const outside = [&](float px, float py)
    {
        for (Circle const& c : circles)
            if (std::hypot(px - c.X, py - c.Y) < c.Radius + kExitMarginYards)
                return false;
        return true;
    };

    std::optional<Point2> best;
    float bestWalk = 1e9f;
    for (int i = 0; i < kDirections; ++i)
    {
        float const angle = awayAngle + float(i) * 2.0f * kPi / float(kDirections);
        for (float walk = kStepYards; walk <= kMaxWalkYards; walk += kStepYards)
        {
            float const px = x + std::cos(angle) * walk;
            float const py = y + std::sin(angle) * walk;
            if (outside(px, py))
            {
                if (walk < bestWalk - 0.001f)   // strictly shorter: the first direction (away) wins ties
                {
                    bestWalk = walk;
                    best = Point2{ px, py };
                }
                break;
            }
        }
    }
    return best;
}

} // namespace ground_avoid
