// Ground effects a bot should step out of (SDK 0.19, layout 21): the spells of data/avoid_spells.txt, and the geometry of getting out. Pure logic: no core, no clock, no
// file access. The module reads the file and passes its text to ParseAvoidSpells; the policy asks EscapePoint where to go (tests/ground_avoid_test.cpp).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ground_avoid {

constexpr float kDefaultRadiusYards = 6.0f;   // the radius used for an effect whose radius nobody knows (0 in the snapshot and in the table)
constexpr float kExitMarginYards = 3.0f;      // the bot walks to this far outside the edge of the effect
constexpr float kInsideMarginYards = 1.0f;    // inside means closer than radius + this: the edge is not exact, and the effect grows a little

// The spells of the Classic dungeons' creatures that leave something harmful on the ground (tools/gen_avoid_spells.py): spell id -> radius (0 = not known).
// A spell that is in the table is avoided even if the snapshot's hostile flag is off; the dungeon files' bosses[].strategy.avoid_spells add to it.
class AvoidTable
{
public:
    void Add(std::uint32_t spell, float radius)
    {
        float& r = _radius[spell];
        if (radius > r)
            r = radius;
    }
    bool Contains(std::uint32_t spell) const { return _radius.count(spell) != 0; }
    // The table's radius for the spell, 0 when it has none or does not know it.
    float Radius(std::uint32_t spell) const
    {
        auto const it = _radius.find(spell);
        return it == _radius.end() ? 0.0f : it->second;
    }
    std::size_t Size() const { return _radius.size(); }

private:
    std::unordered_map<std::uint32_t, float> _radius;
};

// Parses the file: "spell id|radius|kind|map|creature entry|name" per line; blank lines and lines starting with '#' are skipped. A malformed line is left out and
// described in `errors` ("line N: ...").
AvoidTable ParseAvoidSpells(std::string_view text, std::vector<std::string>& errors);

struct Circle
{
    float X = 0.0f;
    float Y = 0.0f;
    float Radius = 0.0f;
};

struct Point2
{
    float X = 0.0f;
    float Y = 0.0f;
};

// Is the bot inside (closer than radius + kInsideMarginYards to the center of) any of the circles?
bool InsideAny(float x, float y, std::vector<Circle> const& circles);

// Where a bot standing at (x, y) goes to get out of the circles it is in: the nearest point (along 16 directions, up to 40 yards) that is outside every circle by
// kExitMarginYards. nullopt when the bot is in none of them (nothing to do) or no direction gets it out. The direction away from the nearest center is preferred
// when two are equally far.
std::optional<Point2> EscapePoint(float x, float y, std::vector<Circle> const& circles);

} // namespace ground_avoid
