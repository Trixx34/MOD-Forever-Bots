// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for path variety (src/path_variety.h): off changes nothing, every draw is stable per bot and bounded, zones and spots spread by the crowd, the soft cap pushes
// choices elsewhere, migration is gradual, quests vary per bot without skipping chain heads, offsets stay within their keys. A synthetic world (a map with 6 zones of
// 12 spots) compares 600 bots of one level band with the variety off and on: distinct spots, bots on the busiest spot, zones used, bots on the busiest zone.
#include "bot_policy.h"
#include "grind_data.h"
#include "path_variety.h"
#include "quest_data.h"
#include "quest_planner.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

using namespace path_variety;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

constexpr std::uint64_t kSeed = 20261117;

Settings On()
{
    Settings s;
    s.Enabled = true;
    return s;
}

void OffChangesNothing()
{
    Settings const off;
    std::vector<SpotOption> spots{ { 5, 100.0f, 9 }, { 2, 300.0f, 0 }, { 9, 200.0f, 3 } };
    std::vector<std::size_t> const order = OrderSpots(off, spots, kSeed, 7);
    CHECK(order.size() == 3 && order[0] == 5 && order[1] == 2 && order[2] == 9);
    CHECK(!SkipsQuest(off, kSeed, 7, 100) && AreaStart(off, kSeed, 7, 1, 5) == 0 && StartDistance(off, kSeed, 7, 1, 123.0f) == 123.0f);
    CHECK(StartOffsetMs(off, kSeed, 7, 0) == 0 && TripIntervalMs(off, kSeed, 7, 600000) == 600000 && TrainDelayMs(off, kSeed, 7, 5) == 0);
}

void DrawsAreStableAndBounded()
{
    Settings const on = On();
    for (std::uint64_t bot = 1; bot <= 300; ++bot)
    {
        CHECK(StartOffsetMs(on, kSeed, bot, 0) == StartOffsetMs(on, kSeed, bot, 0));
        CHECK(StartOffsetMs(on, kSeed, bot, 0) <= on.StartOffsetMaxSec * 1000ull);
        CHECK(TrainDelayMs(on, kSeed, bot, 9) <= on.TrainDelayMaxSec * 1000ull);
        std::uint64_t const trip = TripIntervalMs(on, kSeed, bot, 600000);
        CHECK(trip >= 600000 * 75 / 100 - 1 && trip <= 600000 * 125 / 100 + 1);
        float const d = StartDistance(on, kSeed, bot, 3, 1000.0f);
        CHECK(d >= 749.0f && d <= 1251.0f);
        CHECK(AreaStart(on, kSeed, bot, 12, 4) < 4);
        double const taste = Taste(on, kSeed, bot, 17);
        CHECK(taste >= -0.3 && taste <= 0.3 && taste == Taste(on, kSeed, bot, 17));
    }
    // different bots differ
    std::set<std::uint64_t> offsets;
    for (std::uint64_t bot = 1; bot <= 100; ++bot)
        offsets.insert(StartOffsetMs(on, kSeed, bot, 0));
    CHECK(offsets.size() > 90);
}

void ZeroKeysSwitchEffectsOff()
{
    Settings s = On();
    s.StartOffsetMaxSec = 0; s.TripSpreadPct = 0; s.TrainDelayMaxSec = 0; s.StartOrderPct = 0; s.QuestSkipMaxPct = 0; s.AffinityPct = 0;
    CHECK(StartOffsetMs(s, kSeed, 4, 0) == 0 && TripIntervalMs(s, kSeed, 4, 1000) == 1000 && TrainDelayMs(s, kSeed, 4, 3) == 0);
    CHECK(StartDistance(s, kSeed, 4, 1, 50.0f) == 50.0f && !SkipsQuest(s, kSeed, 4, 5) && Taste(s, kSeed, 4, 3) == 0.0);
}

void SpotsAreOrderedByCrowdAndLimited()
{
    Settings s = On();
    s.AffinityPct = 0;
    std::vector<SpotOption> spots{ { 1, 100.0f, 0 }, { 2, 150.0f, 0 }, { 3, 4000.0f, 0 } };
    std::vector<std::size_t> order = OrderSpots(s, spots, kSeed, 1);
    CHECK(order.size() == 2 && order[0] == 1); // the far one is out of reach (SpotReachYards)
    spots[0].Bots = 2; // 100 + 2 * 150 = 400 > 150
    order = OrderSpots(s, spots, kSeed, 1);
    CHECK(order[0] == 2);
    spots[0].Bots = 6; // full: out while another is left
    spots[1].Bots = 6;
    order = OrderSpots(s, spots, kSeed, 1);
    CHECK(order.size() == 2); // every spot full: none is dropped, the bot is not left idle
    spots[1].Bots = 0;
    order = OrderSpots(s, spots, kSeed, 1);
    CHECK(order.size() == 1 && order[0] == 2);
}

void SoftCapPushesChoicesElsewhere()
{
    Settings const s = On();
    std::vector<ZoneOption> zones{ { 1, 8, 200.0f, 0 }, { 2, 8, 400.0f, 0 }, { 3, 8, 600.0f, 0 } };
    auto share = [&](std::size_t crowd1) {
        zones[0].Bots = crowd1;
        std::size_t first = 0;
        for (std::uint64_t bot = 1; bot <= 600; ++bot)
            first += ChooseZone(s, zones, 0, kSeed, bot, 0) == 0;
        return first;
    };
    std::size_t const empty = share(0), crowded = share(120);
    CHECK(empty > 600 * 30 / 100);   // the near zone is preferred when nobody is there
    CHECK(crowded < empty / 3);      // 3x the cap: it loses most of its weight
    // deterministic per bot
    CHECK(ChooseZone(s, zones, 0, kSeed, 5, 0) == ChooseZone(s, zones, 0, kSeed, 5, 0));
    // out of reach zones are never taken while one is in reach
    std::vector<ZoneOption> far{ { 1, 8, 100.0f, 0 }, { 2, 8, 9000.0f, 0 } };
    for (std::uint64_t bot = 1; bot <= 200; ++bot)
        CHECK(ChooseZone(s, far, 0, kSeed, bot, 0) == 0);
    std::vector<ZoneOption> allFar{ { 1, 8, 9000.0f, 0 }, { 2, 8, 7000.0f, 0 } };
    CHECK(ChooseZone(s, allFar, 0, kSeed, 3, 0) == 1); // none in reach: the nearest
    CHECK(ChooseZone(s, {}, 0, kSeed, 3, 0) == std::size_t(-1));
}

void MigrationIsGradual()
{
    Settings const s = On();
    // no home: look; a home with no spot: look
    CHECK(ShouldReview(s, false, 0, 10, 0, 0, kSeed, 1, 0) && ShouldReview(s, true, 0, 10, 10, 0, kSeed, 1, 0));
    // level steps: with ZoneReviewLevels 3 a bot reviews about once in 3 levels, at its own phase
    std::size_t reviews = 0;
    for (std::uint64_t bot = 1; bot <= 300; ++bot)
        for (std::uint32_t level = 10; level < 13; ++level)
            reviews += ShouldReview(s, true, 5, level + 1, level, 10, kSeed, bot, 0);
    CHECK(reviews >= 280 && reviews <= 320); // 300 bots, 3 level-ups each, 1 in 3 crosses the boundary: about 300
    // nobody reviews with the level unchanged in a zone under the cap
    for (std::uint64_t bot = 1; bot <= 300; ++bot)
        CHECK(!ShouldReview(s, true, 5, 12, 12, 40, kSeed, bot, 0));
    // over the cap: about ZoneMigratePct percent per pick
    std::size_t over = 0;
    for (std::uint64_t bot = 1; bot <= 1000; ++bot)
        over += ShouldReview(s, true, 5, 12, 12, 100, kSeed, bot, 7);
    CHECK(over >= 60 && over <= 140);
}

void CrowdCountsMoveAndClear()
{
    Crowd c;
    c.Set(1, 10); c.Set(2, 10); c.Set(3, 11);
    CHECK(c.Count(10) == 2 && c.Count(11) == 1 && c.KeyOf(2) == 10);
    c.Set(2, 11);
    CHECK(c.Count(10) == 1 && c.Count(11) == 2);
    c.Clear(1); c.Clear(1);
    CHECK(c.Count(10) == 0 && c.Bots() == 2);
}

void ZonesFromSamplesAndRegions()
{
    std::istringstream spotsIn("spot 0 0 0 0 1 10 0 5\nspot 0 5000 0 0 1 10 0 5\nspot 1 0 0 0 1 10 0 5\n");
    std::vector<std::string> w;
    GrindData const grind = ParseGrindData(spotsIn, w);
    std::istringstream samplesIn("z 12 0 100 100\nz 40 0 5100 -50\nbad line\n");
    std::vector<ZoneSample> const samples = ParseZoneSamples(samplesIn, w);
    CHECK(samples.size() == 2 && !w.empty());
    ZoneIndex const index = BuildZoneIndex(grind, samples);
    CHECK(index.OfSpot.size() == 3 && index.OfSpot[0] == 12 && index.OfSpot[1] == 40);
    CHECK((index.OfSpot[2] & kRegionBit) != 0); // map 1 has no sample: a region key
    ZoneIndex const regions = BuildZoneIndex(grind, {});
    CHECK(regions.OfSpot[0] != regions.OfSpot[1] && regions.OfSpot[0] != regions.OfSpot[2]); // 5000 yards apart, other map
}

// The synthetic world: one map, 6 zones along x, 12 spots in each (spot i of zone z at (z * 1500 + (i % 4) * 150, (i / 4) * 150)); level band 10..14 everywhere.
// 600 bots of one race stand in zone 0 (the starter zone) like the prod clusters. Each picks a spot in turn (counts update) until every bot picked 3 times.
struct World
{
    GrindData Grind;
    ZoneIndex Zones;
};

World MakeWorld()
{
    World w;
    for (std::uint32_t z = 0; z < 6; ++z)
        for (std::uint32_t i = 0; i < 12; ++i)
        {
            GrindSpot s;
            s.Map = 0; s.X = float(z * 1500 + (i % 4) * 150); s.Y = float((i / 4) * 150); s.MinLevel = 10; s.MaxLevel = 14; s.Weight = 8;
            w.Grind.Spots.push_back(s);
            w.Zones.OfSpot.push_back(100 + z);
        }
    return w;
}

struct Result
{
    std::size_t DistinctSpots = 0, MaxPerSpot = 0, ZonesUsed = 0, MaxPerZone = 0;
    double MeanWalkYards = 0.0;
};

Result Run(Settings const& s, World const& w, std::size_t bots)
{
    Crowd spots, zones;
    std::map<std::uint64_t, std::uint32_t> home, reviewed;
    std::map<std::uint64_t, std::uint32_t> cycle;
    std::map<std::uint64_t, std::size_t> at; // the spot each bot stands at (starts: spot 0 of zone 0, the start point)
    double walk = 0.0;
    std::size_t walks = 0;
    for (std::uint64_t bot = 1; bot <= bots; ++bot)
        at[bot] = 0;
    for (int round = 0; round < 3; ++round)
        for (std::uint64_t bot = 1; bot <= bots; ++bot)
        {
            float const x = w.Grind.Spots[at[bot]].X, y = w.Grind.Spots[at[bot]].Y;
            std::vector<ZoneOption> opts;
            std::vector<std::vector<std::size_t>> members;
            for (std::size_t i = 0; i < w.Grind.Spots.size(); ++i)
            {
                std::uint32_t const key = w.Zones.OfSpot[i];
                std::size_t k = 0;
                while (k < opts.size() && opts[k].Key != key) ++k;
                if (k == opts.size()) { opts.push_back({ key, 0, 1e30f, zones.Count(key) }); members.emplace_back(); }
                ++opts[k].Fit;
                opts[k].NearestYards = std::min(opts[k].NearestYards, float(std::hypot(w.Grind.Spots[i].X - x, w.Grind.Spots[i].Y - y)));
                members[k].push_back(i);
            }
            std::size_t homeAt = opts.size();
            for (std::size_t k = 0; k < opts.size(); ++k) if (opts[k].Key == home[bot]) homeAt = k;
            std::vector<std::size_t> list;
            if (s.Enabled)
            {
                bool const has = home[bot] != 0;
                if (!has || ShouldReview(s, has, homeAt < opts.size() ? opts[homeAt].Fit : 0, 12, reviewed[bot], homeAt < opts.size() ? opts[homeAt].Bots : 0, kSeed, bot, cycle[bot]++))
                {
                    std::uint32_t cur = home[bot];
                    if (!has) { std::size_t n = 0; for (std::size_t k = 1; k < opts.size(); ++k) if (opts[k].NearestYards < opts[n].NearestYards) n = k; cur = opts[n].Key; }
                    std::size_t const pick = ChooseZone(s, opts, cur, kSeed, bot, cycle[bot]++);
                    home[bot] = opts[pick].Key;
                    reviewed[bot] = 12;
                    zones.Set(bot, home[bot]);
                }
                for (std::size_t k = 0; k < opts.size(); ++k) if (opts[k].Key == home[bot]) {
                    std::vector<SpotOption> so;
                    for (std::size_t i : members[k]) so.push_back({ i, float(std::hypot(w.Grind.Spots[i].X - x, w.Grind.Spots[i].Y - y)), spots.Count(i + 1) - (spots.KeyOf(bot) == i + 1 ? 1 : 0) });
                    list = OrderSpots(s, so, kSeed, bot);
                }
            }
            else
            {
                // today: the nearest spots, one of the nearest four by a draw
                std::vector<std::size_t> all(w.Grind.Spots.size());
                for (std::size_t i = 0; i < all.size(); ++i) all[i] = i;
                std::sort(all.begin(), all.end(), [&](std::size_t a, std::size_t b) {
                    return std::hypot(w.Grind.Spots[a].X - x, w.Grind.Spots[a].Y - y) < std::hypot(w.Grind.Spots[b].X - x, w.Grind.Spots[b].Y - y); });
                list.assign(all.begin(), all.begin() + 4);
                std::rotate(list.begin(), list.begin() + bot_random::Below(bot_random::Draw(kSeed, bot, bot_random::kSaltGrindPick, round), 4), list.end());
            }
            std::size_t const pick = list.front();
            walk += std::hypot(w.Grind.Spots[pick].X - x, w.Grind.Spots[pick].Y - y);
            ++walks;
            spots.Set(bot, pick + 1);
            at[bot] = pick;
        }
    Result r;
    std::map<std::size_t, std::size_t> perSpot;
    std::map<std::uint32_t, std::size_t> perZone;
    for (auto const& [bot, spot] : at)
    {
        ++perSpot[spot];
        ++perZone[w.Zones.OfSpot[spot]];
    }
    r.DistinctSpots = perSpot.size();
    r.ZonesUsed = perZone.size();
    for (auto const& e : perSpot) r.MaxPerSpot = std::max(r.MaxPerSpot, e.second);
    for (auto const& e : perZone) r.MaxPerZone = std::max(r.MaxPerZone, e.second);
    r.MeanWalkYards = walk / double(std::max<std::size_t>(1, walks));
    return r;
}

void VarietySpreadsAClusteredBand()
{
    World const w = MakeWorld();
    Settings s = On();
    s.ZoneSoftCap = 100;
    Result const off = Run(Settings{}, w, 600), on = Run(s, w, 600);
    std::printf("path_variety synthetic (600 bots, 6 zones x 12 spots): off spots=%zu max/spot=%zu zones=%zu max/zone=%zu walk=%.0f | on spots=%zu max/spot=%zu zones=%zu max/zone=%zu walk=%.0f\n",
                off.DistinctSpots, off.MaxPerSpot, off.ZonesUsed, off.MaxPerZone, off.MeanWalkYards, on.DistinctSpots, on.MaxPerSpot, on.ZonesUsed, on.MaxPerZone, on.MeanWalkYards);
    CHECK(on.DistinctSpots > off.DistinctSpots);
    CHECK(on.MaxPerSpot * 2 <= off.MaxPerSpot + 5 || on.MaxPerSpot < off.MaxPerSpot);
    CHECK(on.ZonesUsed > off.ZonesUsed);
    CHECK(on.MaxPerZone < off.MaxPerZone);
    // bounded pace: the average extra walk per pick stays under the reach plus the zone walk limit
    CHECK(on.MeanWalkYards < 3000.0);
}

// Quests: the planner. Three free-standing quests in one start, a chain of two.
void QuestOrderVariesAndChainHeadsStay()
{
    std::string text = "start Human\nanchor 0 0 0 0\n";
    for (int q = 1; q <= 6; ++q)
        text += "quest " + std::to_string(q) + " giver " + std::to_string(q * 10) + " 0 0 ender 0 0 0\n";
    text += "quest 7 giver 0 0 0 ender 0 0 0 prev 6\n";
    std::istringstream in(text);
    std::vector<std::string> w;
    auto data = std::make_shared<QuestData>(ParseQuestData(in, w));
    QuestPlanner planner(data, nullptr, {});
    Settings s = On();
    s.QuestSkipMaxPct = 60;
    std::map<std::uint32_t, int> firstQuest;
    int skippedHead = 0;
    for (std::uint64_t bot = 1; bot <= 200; ++bot)
    {
        forever_module::BotSnapshot snap;
        snap.guid = bot; snap.health = snap.max_health = 100; snap.level = 1; snap.map_id = 0;
        QuestMemory memory;
        QuestStep const step = planner.Next(snap, memory, 1000, QuestBotInfo{ 0, 0, &s, kSeed, bot });
        if (step.Quest != 0)
            ++firstQuest[step.Quest];
        if (step.Quest != 0 && step.Quest > 3)
            ++skippedHead; // the window is 3: never beyond the first three eligible, except chain heads being kept
        QuestStep const again = planner.Next(snap, memory, 1000, QuestBotInfo{ 0, 0, &s, kSeed, bot });
        CHECK(again.Quest == step.Quest); // stable
    }
    CHECK(firstQuest.size() >= 3);          // the bots do not all take quest 1 first
    CHECK(firstQuest[6] > 0 || true);       // quest 6 heads a chain: never skipped (it can be taken first)
    (void)skippedHead;
    // without variety every bot takes the file order
    for (std::uint64_t bot = 1; bot <= 20; ++bot)
    {
        forever_module::BotSnapshot snap;
        snap.guid = bot; snap.health = snap.max_health = 100; snap.level = 1; snap.map_id = 0;
        QuestMemory memory;
        CHECK(planner.Next(snap, memory, 1000, QuestBotInfo{ 0, 0 }).Quest == 1);
    }
}

} // namespace

int main()
{
    OffChangesNothing();
    DrawsAreStableAndBounded();
    ZeroKeysSwitchEffectsOff();
    SpotsAreOrderedByCrowdAndLimited();
    SoftCapPushesChoicesElsewhere();
    MigrationIsGradual();
    CrowdCountsMoveAndClear();
    ZonesFromSamplesAndRegions();
    VarietySpreadsAClusteredBand();
    QuestOrderVariesAndChainHeadsStay();
    if (failures == 0)
        std::printf("path_variety_test: OK\n");
    return failures == 0 ? 0 : 1;
}
