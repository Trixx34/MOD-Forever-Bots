// SPDX-License-Identifier: GPL-2.0-or-later
#include "path_variety.h"

#include "bot_random.h"
#include "grind_data.h"
#include "text_lines.h"
#include "text_number.h"

#include <algorithm>
#include <cmath>
#include <fstream>

namespace path_variety {

void Crowd::Set(std::uint64_t bot, std::uint64_t key)
{
    auto const at = _of.find(bot);
    if (at != _of.end())
    {
        if (at->second == key)
            return;
        auto const old = _count.find(at->second);
        if (old != _count.end() && --old->second == 0)
            _count.erase(old);
        at->second = key;
    }
    else
        _of.emplace(bot, key);
    ++_count[key];
}

void Crowd::Clear(std::uint64_t bot)
{
    auto const at = _of.find(bot);
    if (at == _of.end())
        return;
    auto const old = _count.find(at->second);
    if (old != _count.end() && --old->second == 0)
        _count.erase(old);
    _of.erase(at);
}

std::size_t Crowd::Count(std::uint64_t key) const
{
    auto const at = _count.find(key);
    return at == _count.end() ? 0 : at->second;
}

std::uint64_t Crowd::KeyOf(std::uint64_t bot) const
{
    auto const at = _of.find(bot);
    return at == _of.end() ? 0 : at->second;
}

std::vector<ZoneSample> ParseZoneSamples(std::istream& in, std::vector<std::string>& warnings)
{
    std::vector<ZoneSample> out;
    std::size_t number = 0;
    std::vector<std::string> tokens;
    while (text_lines::NextTokens(in, number, tokens))
    {
        ZoneSample sample;
        if (tokens[0] != "z" || tokens.size() < 5 || !text_number::ParseWhole(tokens[1], sample.Zone) || !text_number::ParseWhole(tokens[2], sample.Map) ||
            !text_number::ParseDecimal(tokens[3], sample.X) || !text_number::ParseDecimal(tokens[4], sample.Y) || sample.Zone == 0 || sample.Zone >= kRegionBit)
        {
            warnings.push_back("zone samples: line " + std::to_string(number) + " skipped");
            continue;
        }
        out.push_back(sample);
    }
    return out;
}

std::vector<ZoneSample> LoadZoneSamples(std::string const& path, std::vector<std::string>& warnings)
{
    std::ifstream in(path);
    if (!in)
    {
        warnings.push_back("zone samples: cannot read " + path);
        return {};
    }
    return ParseZoneSamples(in, warnings);
}

ZoneIndex BuildZoneIndex(GrindData const& grind, std::vector<ZoneSample> const& samples)
{
    ZoneIndex index;
    index.OfSpot.reserve(grind.Spots.size());
    for (GrindSpot const& spot : grind.Spots)
    {
        std::uint32_t zone = 0;
        float best = kZoneSampleYards;
        for (ZoneSample const& sample : samples)
        {
            if (sample.Map != spot.Map)
                continue;
            float const d = std::hypot(sample.X - spot.X, sample.Y - spot.Y);
            if (d <= best)
            {
                best = d;
                zone = sample.Zone;
            }
        }
        if (zone == 0)
        {
            auto const cell = [](float v) { return std::uint32_t(std::int32_t(std::floor(v / kRegionYards)) + 512) & 0x3FFu; };
            zone = kRegionBit | ((spot.Map & 0x7FFu) << 20) | (cell(spot.X) << 10) | cell(spot.Y);
        }
        index.OfSpot.push_back(zone);
    }
    return index;
}

double ZoneWeight(Settings const& settings, ZoneOption const& zone, bool current)
{
    if (zone.Fit == 0)
        return 0.0;
    if (zone.NearestYards > float(settings.ZoneMaxWalkYards) && !current)
        return 0.0;
    double const fit = 0.1 + double(std::min<std::uint32_t>(zone.Fit, 10)) / 10.0;
    double const spread = double(settings.ZoneSpread) / 100.0;
    double const reach = double(settings.ZoneMaxWalkYards) / 2.0;
    // ZoneSpread is the share of the weight that follows distance, 1 / (1 + d / reach); the rest does not depend on it
    double const near = (1.0 - spread) + spread / (1.0 + double(zone.NearestYards) / reach);
    double cap = 1.0;
    if (settings.ZoneSoftCap != 0)
    {
        double const ratio = double(zone.Bots) / double(settings.ZoneSoftCap);
        cap = 1.0 / (1.0 + ratio * ratio);
    }
    double weight = fit * near * cap;
    if (current)
        weight *= 2.0;
    return weight;
}

std::size_t ChooseZone(Settings const& settings, std::vector<ZoneOption> const& zones, std::uint32_t current, std::uint64_t seed, std::uint64_t bot, std::uint64_t counter)
{
    if (zones.empty())
        return std::size_t(-1);
    std::vector<double> weight(zones.size());
    double total = 0.0;
    for (std::size_t i = 0; i < zones.size(); ++i)
    {
        weight[i] = ZoneWeight(settings, zones[i], zones[i].Key == current);
        total += weight[i];
    }
    if (total <= 0.0)
    {
        // nothing within reach: the nearest zone that has a spot at all
        std::size_t best = 0;
        for (std::size_t i = 1; i < zones.size(); ++i)
            if (zones[i].NearestYards < zones[best].NearestYards)
                best = i;
        return best;
    }
    double pick = bot_random::Unit(bot_random::Draw(seed, bot, kSaltZone, counter)) * total;
    for (std::size_t i = 0; i < zones.size(); ++i)
    {
        if (pick < weight[i])
            return i;
        pick -= weight[i];
    }
    return zones.size() - 1;
}

bool ShouldReview(Settings const& settings, bool hasHome, std::uint32_t homeFit, std::uint32_t level, std::uint32_t reviewedAtLevel, std::size_t homeBots,
                  std::uint64_t seed, std::uint64_t bot, std::uint64_t counter)
{
    if (!hasHome || homeFit == 0)
        return true;
    std::uint32_t const step = std::max<std::uint32_t>(1, settings.ZoneReviewLevels);
    std::uint32_t const phase = std::uint32_t(bot_random::Draw(seed, bot, kSaltZonePhase) % step);
    if ((level + phase) / step != (reviewedAtLevel + phase) / step)
        return true;
    if (settings.ZoneSoftCap != 0 && homeBots > settings.ZoneSoftCap && settings.ZoneMigratePct != 0)
        return bot_random::Below(bot_random::Draw(seed, bot, kSaltZoneReview, counter), 100) < settings.ZoneMigratePct;
    return false;
}

double Taste(Settings const& settings, std::uint64_t seed, std::uint64_t bot, std::size_t spot)
{
    if (settings.AffinityPct == 0)
        return 0.0;
    return (bot_random::Unit(bot_random::Draw(seed, bot, kSaltAffinity, spot)) * 2.0 - 1.0) * double(settings.AffinityPct) / 100.0;
}

std::vector<std::size_t> OrderSpots(Settings const& settings, std::vector<SpotOption> spots, std::uint64_t seed, std::uint64_t bot)
{
    std::vector<std::size_t> out;
    if (spots.empty())
        return out;
    if (!settings.Enabled)
    {
        for (SpotOption const& s : spots)
            out.push_back(s.Index);
        return out;
    }
    float nearest = spots[0].Yards;
    for (SpotOption const& s : spots)
        nearest = std::min(nearest, s.Yards);
    std::vector<SpotOption> kept;
    for (SpotOption const& s : spots)
        if (s.Yards <= nearest + float(settings.SpotReachYards))
            kept.push_back(s);
    if (settings.SpotMax != 0)
    {
        std::vector<SpotOption> room;
        for (SpotOption const& s : kept)
            if (s.Bots < settings.SpotMax)
                room.push_back(s);
        if (!room.empty())
            kept = std::move(room);
        // every spot in reach full: all stay in the list (the crowd penalty puts the fullest last), the bot is never left idle
    }
    auto const score = [&](SpotOption const& s)
    { return double(s.Yards) * (1.0 + Taste(settings, seed, bot, s.Index)) + double(settings.CrowdYards) * double(s.Bots); };
    std::sort(kept.begin(), kept.end(), [&](SpotOption const& a, SpotOption const& b)
              {
                  double const sa = score(a), sb = score(b);
                  return sa != sb ? sa < sb : a.Index < b.Index;
              });
    for (SpotOption const& s : kept)
        out.push_back(s.Index);
    return out;
}

bool SkipsQuest(Settings const& settings, std::uint64_t seed, std::uint64_t bot, std::uint32_t quest)
{
    if (!settings.Enabled || settings.QuestSkipMaxPct == 0)
        return false;
    // a stable appetite per bot (0..100) scales the chance: a bot of appetite 0 skips QuestSkipMaxPct percent of the quests, one of appetite 100 none
    std::uint32_t const mind = bot_random::Below(bot_random::Draw(seed, bot, kSaltQuestMind), 101);
    std::uint32_t const chance = settings.QuestSkipMaxPct * (100 - mind) / 100;
    return bot_random::Below(bot_random::Draw(seed, bot, kSaltQuestSkip, quest), 100) < chance;
}

std::uint64_t QuestOrderKey(Settings const& settings, std::uint64_t seed, std::uint64_t bot, std::uint32_t quest)
{
    return bot_random::Draw(seed, bot, kSaltQuestOrder, quest);
}

std::size_t AreaStart(Settings const& settings, std::uint64_t seed, std::uint64_t bot, std::uint32_t quest, std::size_t areas)
{
    if (!settings.Enabled || areas <= 1)
        return 0;
    return std::size_t(bot_random::Draw(seed, bot, kSaltArea, quest) % areas);
}

float StartDistance(Settings const& settings, std::uint64_t seed, std::uint64_t bot, std::size_t start, float yards)
{
    if (!settings.Enabled || settings.StartOrderPct == 0)
        return yards;
    double const wobble = (bot_random::Unit(bot_random::Draw(seed, bot, kSaltStart, start)) * 2.0 - 1.0) * double(settings.StartOrderPct) / 100.0;
    return float(double(yards) * (1.0 + wobble));
}

std::uint64_t StartOffsetMs(Settings const& settings, std::uint64_t seed, std::uint64_t bot, std::uint32_t what)
{
    if (!settings.Enabled || settings.StartOffsetMaxSec == 0)
        return 0;
    return bot_random::Draw(seed, bot, kSaltOffset, what) % (std::uint64_t(settings.StartOffsetMaxSec) * 1000 + 1);
}

std::uint64_t TripIntervalMs(Settings const& settings, std::uint64_t seed, std::uint64_t bot, std::uint64_t baseMs)
{
    if (!settings.Enabled || settings.TripSpreadPct == 0)
        return baseMs;
    return bot_random::Vary(baseMs, settings.TripSpreadPct, bot_random::Draw(seed, bot, kSaltTrip));
}

std::uint64_t TrainDelayMs(Settings const& settings, std::uint64_t seed, std::uint64_t bot, std::uint32_t level)
{
    if (!settings.Enabled || settings.TrainDelayMaxSec == 0)
        return 0;
    return bot_random::Draw(seed, bot, kSaltTrain, level) % (std::uint64_t(settings.TrainDelayMaxSec) * 1000 + 1);
}

} // namespace path_variety
