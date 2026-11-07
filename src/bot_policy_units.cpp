// SPDX-License-Identifier: GPL-2.0-or-later
// bot_policy_detail helpers: units, distances, pulls and stand points (declared in bot_policy_internal.h).
#include "bot_policy_internal.h"

namespace bot_policy_detail {

float DistanceBetween(NearbyUnit const& a, NearbyUnit const& b)
{
    float const dx = a.pos_x - b.pos_x;
    float const dy = a.pos_y - b.pos_y;
    return std::sqrt(dx * dx + dy * dy);
}

std::uint32_t ZeroExpDifference(std::uint32_t level)
{
    if (level < 8)
        return 5;
    if (level < 10)
        return 6;
    if (level < 12)
        return 7;
    if (level < 16)
        return 8;
    if (level < 20)
        return 9;
    if (level < 30)
        return 11;
    if (level < 40)
        return 12;
    if (level < 45)
        return 13;
    if (level < 50)
        return 14;
    if (level < 55)
        return 15;
    if (level < 60)
        return 16;
    return 17;
}

bool IsGrey(BotSnapshot const& bot, NearbyUnit const& unit)
{
    return bot.level != 0 && unit.level != 0 && unit.level < bot.level && bot.level - unit.level >= ZeroExpDifference(bot.level);
}

float HealthShare(NearbyUnit const& unit)
{
    return unit.max_health ? float(unit.health) / float(unit.max_health) : 1.0f;
}

bool OnOtherLevel(BotSnapshot const& bot, NearbyUnit const& unit)
{
    float const height = std::fabs(unit.pos_z - bot.pos_z);
    return height >= kOtherLevelHeight && height > kOtherLevelSlope * rotation::DistanceYards(bot, unit);
}

NearbyUnit const* PullTarget(BotSnapshot const& bot, std::span<NearbyUnit const> units,
                            std::function<bool(std::uint64_t)> const& avoided, bool defending,
                            std::vector<std::uint32_t> const& onlyEntries, bool dungeon,
                            std::function<bool(NearbyUnit const&)> const& leashed)
{
    NearbyUnit const* best = nullptr;
    float bestHealth = 0.0f;
    float bestDist = 0.0f;
    for (NearbyUnit const& candidate : units)
    {
        if (!IsLiveMob(candidate) || avoided(candidate.guid))
            continue;
        if (leashed && leashed(candidate))
            continue; // off the party's route (a pack beside it, a creature that fled): left alone, the party does not drift after it
        // A creature on another level is not pulled (one that attacks the bot is: `defending`, and the stall watchdog deals with it if it cannot be reached).
        if (!defending && OnOtherLevel(bot, candidate))
            continue;
        // A bot hunting for a quest pulls only the creatures the quest wants (unless it is already being attacked).
        if (!defending && !onlyEntries.empty() &&
            std::find(onlyEntries.begin(), onlyEntries.end(), forever_module::UnitRefEntry(candidate.guid)) == onlyEntries.end())
            continue;
        // Nothing to gain from a grey creature (no experience): left alone, so the bot goes looking for something that levels it. Not when
        // hunting for a quest (the quest wants it), nor in a dungeon, nor when the bot is already being attacked.
        if (!defending && !dungeon && onlyEntries.empty() && IsGrey(bot, candidate))
            continue;
        // Not worth pulling: well above the bot's level, or an elite. (A level of 0 is unknown and not judged.) A bot that is already
        // in combat is being attacked, and the snapshot does not say by what: it fights the closest creature, whatever its level, since
        // standing still until it dies is worse. The same goes for the pack-size rule below.
        // In a dungeon (the route is active) elites, bosses and higher levels are the content: the party fights what it meets.
        if (!defending && !dungeon && bot.level != 0 &&
            ((candidate.level != 0 && candidate.level > bot.level + kMaxLevelAbove) || candidate.elite))
            continue;
        float const health = HealthShare(candidate);
        float const dist = rotation::DistanceYards(bot, candidate);
        if (best && !(health < bestHealth || (health == bestHealth && dist < bestDist)))
            continue;

        std::size_t group = 0;
        for (NearbyUnit const& other : units)
            if (IsLiveMob(other) && DistanceBetween(candidate, other) <= kPullRadius)
                ++group;
        if (!defending && group > (dungeon ? kMaxDungeonPull : kMaxPull))
            continue;

        best = &candidate;
        bestHealth = health;
        bestDist = dist;
    }
    return best;
}

float RouteLegDistance(std::vector<Point> const& route, std::size_t index, std::optional<Point> const& start, float x, float y)
{
    if (route.empty() || index >= route.size())
        return std::numeric_limits<float>::infinity();
    Point const& to = route[index];
    Point const from = index > 0 ? route[index - 1] : start.value_or(to);
    float const dx = to.X - from.X;
    float const dy = to.Y - from.Y;
    float const length2 = dx * dx + dy * dy;
    float t = length2 > 0.0f ? ((x - from.X) * dx + (y - from.Y) * dy) / length2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    return std::hypot(x - (from.X + t * dx), y - (from.Y + t * dy));
}

NearbyUnit const* Closest(BotSnapshot const& bot, std::span<NearbyUnit const> units, bool (*wanted)(NearbyUnit const&))
{
    NearbyUnit const* best = nullptr;
    float bestDist = std::numeric_limits<float>::max();
    for (NearbyUnit const& unit : units)
    {
        if (!wanted(unit))
            continue;
        float const dist = rotation::DistanceYards(bot, unit);
        if (dist < bestDist)
        {
            bestDist = dist;
            best = &unit;
        }
    }
    return best;
}

bool IsLiveMob(NearbyUnit const& unit)
{
    return unit.is_hostile && unit.health > 0 && forever_module::UnitRefType(unit.guid) == 8;
}

bool IsPlayer(NearbyUnit const& unit)
{
    return forever_module::UnitRefType(unit.guid) == 2 && unit.health > 0 && !unit.is_hostile;
}

void AvoidUnit(std::unordered_map<std::uint64_t, std::uint64_t>& avoidUntil, std::unordered_map<std::uint64_t, std::uint32_t>& count,
               std::uint64_t ref, std::uint64_t nowMs)
{
    std::uint32_t const times = ++count[ref];
    std::uint32_t const doublings = std::min<std::uint32_t>(times - 1, kMaxAvoidDoublings);
    avoidUntil[ref] = nowMs + (kAvoidMs << doublings);
}

void AvoidStalledUnit(std::unordered_map<std::uint64_t, std::uint64_t>& avoidUntil, std::unordered_map<std::uint64_t, std::uint32_t>& count,
                      std::uint64_t ref, std::uint64_t nowMs)
{
    std::uint32_t const times = ++count[ref];
    std::uint32_t const doublings = std::min<std::uint32_t>(times - 1, kMaxStallDoublings);
    avoidUntil[ref] = nowMs + (kStallAvoidMs << doublings);
}

NearbyUnit const* FindLiveMob(std::span<NearbyUnit const> units, std::uint64_t guid)
{
    for (NearbyUnit const& unit : units)
        if (unit.guid == guid && IsLiveMob(unit))
            return &unit;
    return nullptr;
}

Point AoeKitePoint(BotSnapshot const& bot, NearbyUnit const& warder, int retry, std::optional<Point> const& entry)
{
    if (entry && std::hypot(entry->X - warder.pos_x, entry->Y - warder.pos_y) > kAoeStandYards)
    {
        if (retry == 0)
        {
            float ex = entry->X - warder.pos_x;
            float ey = entry->Y - warder.pos_y;
            float const elength = std::hypot(ex, ey);
            ex /= elength;
            ey /= elength;
            float const slotAngle = float(int((bot.guid * 7) % 11) - 5) * kAoeKiteSpreadDegrees * 3.14159265f / 180.0f;
            float const ec = std::cos(slotAngle);
            float const es = std::sin(slotAngle);
            return Point{ warder.pos_x + (ex * ec - ey * es) * kAoeStandYards, warder.pos_y + (ex * es + ey * ec) * kAoeStandYards, bot.pos_z };
        }
        --retry;
    }
    float dx = bot.pos_x - warder.pos_x;
    float dy = bot.pos_y - warder.pos_y;
    float const length = std::sqrt(dx * dx + dy * dy);
    if (length > 0.01f)
    {
        dx /= length;
        dy /= length;
    }
    else
    {
        dx = 1.0f;
        dy = 0.0f;
    }
    int const slot = int((bot.guid * 7) % 11) - 5;   // -5..5, fixed per bot
    int const turn = retry == 0 ? 0 : (retry % 2 == 1 ? (retry + 1) / 2 : -(retry / 2));
    float const angle = (float(slot) * kAoeKiteSpreadDegrees + float(turn) * kAoeKiteRetryDegrees) * 3.14159265f / 180.0f;
    float const c = std::cos(angle);
    float const s = std::sin(angle);
    return Point{ warder.pos_x + (dx * c - dy * s) * kAoeStandYards, warder.pos_y + (dx * s + dy * c) * kAoeStandYards, bot.pos_z };
}

std::vector<ground_avoid::Circle> HazardCircles(PolicyOptions const& options, BotSnapshot const& bot)
{
    std::vector<ground_avoid::Circle> circles;
    auto const dungeon = options.DungeonsByMap.find(bot.map_id);
    bool const hasDungeon = dungeon != options.DungeonsByMap.end();
    for (std::size_t i = 0; i < bot.ground_effect_count && i < forever_module::kMaxGroundEffects; ++i)
    {
        forever_module::GroundEffect const& effect = bot.ground_effects[i];
        bool harmful = effect.hostile != 0;
        float radius = effect.radius;
        if (options.AvoidSpells && options.AvoidSpells->Contains(effect.spell_id))
        {
            harmful = true;
            if (radius <= 0.0f)
                radius = options.AvoidSpells->Radius(effect.spell_id);
        }
        if (!harmful && hasDungeon)
            harmful = std::find(dungeon->second.AvoidSpells.begin(), dungeon->second.AvoidSpells.end(), effect.spell_id) != dungeon->second.AvoidSpells.end();
        if (!harmful)
            continue;
        circles.push_back(ground_avoid::Circle{ effect.x, effect.y, radius > 0.0f ? radius : ground_avoid::kDefaultRadiusYards });
    }
    if (hasDungeon)
        circles.insert(circles.end(), dungeon->second.AvoidZones.begin(), dungeon->second.AvoidZones.end());
    return circles;
}

std::optional<Point> BandStandPoint(BotSnapshot const& bot, NearbyUnit const& target, float yards, std::vector<ground_avoid::Circle> const& hazards)
{
    float dx = bot.pos_x - target.pos_x;
    float dy = bot.pos_y - target.pos_y;
    float const length = std::sqrt(dx * dx + dy * dy);
    if (length > 0.01f)
    {
        dx /= length;
        dy /= length;
    }
    else
    {
        dx = 1.0f;
        dy = 0.0f;
    }
    for (int step = 0; step <= 8; ++step)
    {
        int const turn = step == 0 ? 0 : (step % 2 == 1 ? (step + 1) / 2 : -(step / 2));
        float const angle = float(turn) * kBandStepDegrees * 3.14159265f / 180.0f;
        float const c = std::cos(angle);
        float const s = std::sin(angle);
        Point const point{ target.pos_x + (dx * c - dy * s) * yards, target.pos_y + (dx * s + dy * c) * yards, bot.pos_z };
        if (!ground_avoid::InsideAny(point.X, point.Y, hazards))
            return point;
    }
    return std::nullopt;
}

std::uint64_t PlayerRef(std::uint64_t counter)
{
    return forever_module::MakeUnitRef(static_cast<std::uint8_t>(kPlayerType), 0, counter);
}

float Distance2d(BotSnapshot const& a, BotSnapshot const& b)
{
    float const dx = a.pos_x - b.pos_x;
    float const dy = a.pos_y - b.pos_y;
    return std::sqrt(dx * dx + dy * dy);
}

float DistanceTo(BotSnapshot const& bot, Point const& point)
{
    float const dx = bot.pos_x - point.X;
    float const dy = bot.pos_y - point.Y;
    return std::sqrt(dx * dx + dy * dy);
}

} // namespace bot_policy_detail
