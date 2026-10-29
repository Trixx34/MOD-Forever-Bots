#include "errand_trainers.h"

#include "text_split.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>

namespace errand {

namespace {

// Splits on a separator, keeping empty fields.
std::vector<std::string_view> Split(std::string_view text, char sep)
{
    return text_split::Split(text, sep);
}

std::string_view Trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}

bool ParseUnsigned(std::string_view text, std::uint64_t& value)
{
    text = Trim(text);
    if (text.empty() || text.size() > 20)
        return false;
    std::uint64_t result = 0;
    for (char c : text)
    {
        if (c < '0' || c > '9')
            return false;
        result = result * 10 + std::uint64_t(c - '0');
    }
    value = result;
    return true;
}

bool ParseFloat(std::string_view text, float& value)
{
    std::string const copy(Trim(text));
    if (copy.empty())
        return false;
    char* end = nullptr;
    errno = 0;
    double const parsed = std::strtod(copy.c_str(), &end);
    if (end != copy.c_str() + copy.size() || errno != 0 || !std::isfinite(parsed))
        return false;
    value = static_cast<float>(parsed);
    return true;
}

} // namespace

std::string CityName(std::uint32_t city)
{
    switch (city)
    {
        case kStormwind:
            return "Stormwind";
        case kIronforge:
            return "Ironforge";
        case kDarnassus:
            return "Darnassus";
        case kUndercity:
            return "Undercity";
        case kOrgrimmar:
            return "Orgrimmar";
        case kThunderBluff:
            return "ThunderBluff";
        default:
            break;
    }
    return "city" + std::to_string(city);
}

std::uint32_t CityOf(std::string_view name)
{
    if (name == "Stormwind")
        return kStormwind;
    if (name == "Ironforge")
        return kIronforge;
    if (name == "Darnassus")
        return kDarnassus;
    if (name == "Undercity")
        return kUndercity;
    if (name == "Orgrimmar")
        return kOrgrimmar;
    if (name == "ThunderBluff")
        return kThunderBluff;
    return 0;
}

std::vector<Trainer> ParseTrainerTable(std::string_view text, std::vector<std::string>& errors)
{
    std::vector<Trainer> trainers;
    std::size_t lineNo = 0;
    std::size_t start = 0;
    while (start <= text.size())
    {
        std::size_t const end = text.find('\n', start);
        std::string_view line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        start = end == std::string_view::npos ? text.size() + 1 : end + 1;
        ++lineNo;

        line = Trim(line);
        if (line.empty() || line.front() == '#')
            continue;

        auto const fail = [&](char const* why) { errors.push_back("line " + std::to_string(lineNo) + ": " + why); };
        std::vector<std::string_view> const f = Split(line, '|');
        if (f.size() != 10)
        {
            fail("expected 10 fields");
            continue;
        }

        Trainer t;
        std::uint64_t side = 0, map = 0, entry = 0, skill = 0, guid = 0;
        std::vector<std::string_view> const xyz = Split(Trim(f[5]), ' ');
        float x = 0.0f, y = 0.0f, z = 0.0f;
        if (!ParseUnsigned(f[0], side) || (side != kAlliance && side != kHorde))
        {
            fail("side must be 1 or 2");
            continue;
        }
        t.City = CityOf(Trim(f[1]));
        if (t.City == 0)
        {
            fail("unknown city");
            continue;
        }
        if (!ParseUnsigned(f[2], map) || map > 1)
        {
            fail("map must be 0 or 1");
            continue;
        }
        if (!ParseUnsigned(f[3], entry) || entry == 0 || entry > std::numeric_limits<std::uint32_t>::max())
        {
            fail("bad entry");
            continue;
        }
        if (!ParseUnsigned(f[4], skill) || skill == 0 || skill > std::numeric_limits<std::uint32_t>::max())
        {
            fail("bad skill");
            continue;
        }
        if (xyz.size() != 3 || !ParseFloat(xyz[0], x) || !ParseFloat(xyz[1], y) || !ParseFloat(xyz[2], z))
        {
            fail("position must be three numbers");
            continue;
        }
        if (!ParseUnsigned(f[9], guid))
        {
            fail("bad guid");
            continue;
        }
        t.Side = static_cast<std::uint32_t>(side);
        t.Map = static_cast<std::uint32_t>(map);
        t.Entry = static_cast<std::uint32_t>(entry);
        t.Skill = static_cast<std::uint32_t>(skill);
        t.X = x;
        t.Y = y;
        t.Z = z;
        t.Name = std::string(Trim(f[6]));
        t.Subname = std::string(Trim(f[7]));
        t.Guid = guid;
        trainers.push_back(std::move(t));
    }
    return trainers;
}

int TrainingDueSlot(std::uint16_t const skill[2], std::uint16_t const value[2], std::uint16_t const max[2])
{
    for (int slot = 0; slot < 2; ++slot)
    {
        if (skill[slot] != 0 && value[slot] < max[slot])
            return slot;
    }
    return -1;
}

std::optional<std::size_t> ChooseTrainer(std::vector<Trainer> const& trainers, std::uint32_t side, std::uint32_t skill, std::uint32_t city,
                                         std::uint32_t map, float x, float y, float z)
{
    std::optional<std::size_t> best;
    float bestDist = 0.0f;
    bool bestOnMap = false;
    for (std::size_t i = 0; i < trainers.size(); ++i)
    {
        Trainer const& t = trainers[i];
        if (t.Side != side || t.Skill != skill || t.City != city)
            continue;
        bool const onMap = t.Map == map;
        float const dist = std::hypot(t.X - x, t.Y - y, t.Z - z);
        // A trainer on the bot's map beats one on another map; within the same class the nearer wins, then the lower guid.
        bool const better = !best || (onMap && !bestOnMap) ||
                            (onMap == bestOnMap && (dist < bestDist || (dist == bestDist && t.Guid < trainers[*best].Guid)));
        if (better)
        {
            best = i;
            bestDist = dist;
            bestOnMap = onMap;
        }
    }
    return best;
}

std::vector<taxi::Place> CityPlaces(std::vector<Trainer> const& trainers)
{
    struct Sum
    {
        double X = 0.0;
        double Y = 0.0;
        std::size_t Count = 0;
    };
    std::map<std::pair<std::uint32_t, std::uint32_t>, Sum> sums;    // (city, map) -> sum of the trainer positions
    for (Trainer const& trainer : trainers)
    {
        Sum& sum = sums[{ trainer.City, trainer.Map }];
        sum.X += trainer.X;
        sum.Y += trainer.Y;
        ++sum.Count;
    }
    std::vector<taxi::Place> places;
    for (auto const& [key, sum] : sums)
        places.push_back(taxi::Place{ CityName(key.first), key.second, float(sum.X / double(sum.Count)), float(sum.Y / double(sum.Count)) });
    return places;
}

} // namespace errand
