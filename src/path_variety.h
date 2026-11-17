// SPDX-License-Identifier: GPL-2.0-or-later
// Path variety (config PathVariety, off by default): bots of one race and class no longer all walk the same leveling path.
// Prod 2026-11-17: 800 bots on 23 zones, 14.6 percent of same race+class pairs share their grind spot, one spot held 72 bots and one zone 119 of the
// level 13 to 15 bots. Everything here is a pure function of the run's seed, the bot's guid and a salt (bot_random::Draw), so a bot keeps its
// variation across restarts, and tests can pin every value. Nothing here touches a survival rule, a rotation, a party or a route.
//
// Five levers, each with its own key and each switched off by 0 (or by the master switch):
//   zones   a bot has a home zone among the level-appropriate zones of its map and side; it is chosen by a seeded weighted draw (fit, distance, a soft cap on bots
//           per zone) and reviewed only at level steps (every ZoneReviewLevels, at a per-bot phase), when the zone has no spot for its level any more, or with a
//           small chance per spot pick when the zone is over its soft cap: bots migrate one at a time and never teleport
//   spots   inside the zone, the spots are ordered by distance, per-bot taste (a stable +-AffinityPct per bot and spot) and the number of bots already at a
//           spot (CrowdYards per bot); a spot with SpotMax bots is skipped while another is left; spots farther than SpotReachYards beyond the nearest are not used,
//           so the level pace is bounded by the extra walk
//   quests  a bot takes the next quest among the first QuestOrderWindow eligible ones by a per-bot order, skips up to QuestSkipMaxPct of the quests that no other
//           quest needs (it grinds meanwhile), starts its hunting areas at a per-bot index, and ranks the quest starts of its map with a +-StartOrderPct distance wobble
//   offset  the first vendor trip and the first grind walk after login start a per-bot delay later (StartOffsetMaxSec); a level-up trainer visit waits TrainDelayMaxSec at most
//   trips   the vendor interval is stretched or shortened per bot by +-TripSpreadPct
#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <unordered_map>
#include <vector>

struct GrindData;

namespace path_variety {

struct Settings
{
    bool Enabled = false;                       // PathVariety
    std::uint32_t ZoneSpread = 50;              // PathZoneSpread (0..100): 0 = no zone choice at all; otherwise the share of a zone's weight that follows distance (the rest is flat)
    std::uint32_t ZoneSoftCap = 40;             // PathZoneSoftCap (0..500): bots per zone after which a zone loses weight; 0 = no cap
    std::uint32_t ZoneMaxWalkYards = 2500;      // PathZoneMaxWalkYards (200..6000): a zone farther than this from the bot is not chosen (unless it is the only one)
    std::uint32_t ZoneReviewLevels = 3;         // PathZoneReviewLevels (1..10): a bot looks for a new home zone every this many levels
    std::uint32_t ZoneMigratePct = 10;          // PathZoneMigratePct (0..100): chance per spot pick for a bot whose zone is over the soft cap to review it
    std::uint32_t SpotMax = 6;                  // PathSpotMax (0..100): most bots at one grind spot while another is free; 0 = no limit
    std::uint32_t CrowdYards = 150;             // PathCrowdYards (0..2000): the distance penalty for each bot already at a spot
    std::uint32_t AffinityPct = 30;             // PathSpotAffinityPct (0..90): stable per-bot taste for a spot, +- this many percent of the distance
    std::uint32_t SpotReachYards = 700;         // PathSpotReachYards (100..4000): spots beyond the nearest one by more than this are not used
    std::uint32_t QuestSkipMaxPct = 25;         // PathQuestSkipMaxPct (0..60): the chance of the least quest-minded bot to leave a free-standing quest to grind
    std::uint32_t QuestOrderWindow = 3;         // PathQuestOrderWindow (1..8): the next quest is taken among this many eligible ones by a per-bot order; 1 = file order
    std::uint32_t StartOrderPct = 25;           // PathStartOrderPct (0..60): the distance to a quest start is wobbled per bot by +- this many percent when ranking starts
    std::uint32_t StartOffsetMaxSec = 120;      // PathStartOffsetMaxSec (0..900): longest first delay of the first vendor trip and grind walk after login
    std::uint32_t TripSpreadPct = 25;           // PathTripSpreadPct (0..60): the vendor interval varies per bot by +- this many percent
    std::uint32_t TrainDelayMaxSec = 60;        // PathTrainDelayMaxSec (0..600): the trainer visit after a level-up waits up to this long
};

constexpr std::uint32_t kZoneSaltBase = 0xB0;   // salts 0xB0.. (personality uses 0xA0..0xA4)
enum Salt : std::uint32_t
{
    kSaltZone = 0xB0, kSaltZoneReview = 0xB1, kSaltZonePhase = 0xB2, kSaltAffinity = 0xB3, kSaltQuestSkip = 0xB4, kSaltQuestMind = 0xB5,
    kSaltQuestOrder = 0xB6, kSaltArea = 0xB7, kSaltStart = 0xB8, kSaltOffset = 0xB9, kSaltTrip = 0xBA, kSaltTrain = 0xBB,
};

// Bots counted per key (a zone or a grind spot): each bot is at one key at most. Memory is bounded by the number of bots.
class Crowd
{
public:
    void Set(std::uint64_t bot, std::uint64_t key);
    void Clear(std::uint64_t bot);
    std::size_t Count(std::uint64_t key) const;
    std::uint64_t KeyOf(std::uint64_t bot) const; // 0 when the bot is not counted
    std::size_t Bots() const { return _of.size(); }

private:
    std::unordered_map<std::uint64_t, std::uint64_t> _of;
    std::unordered_map<std::uint64_t, std::size_t> _count;
};

// Zones. The grind file has no zone column (the creature table of this fork leaves zoneId at 0), so a zone comes from a sample file, one record per line:
//   z <zone> <map> <x> <y>        a place known to be in that zone (the position of a character the zone of which is known)
// A spot takes the zone of the nearest sample on its map within kZoneSampleYards; with none it falls into a coarse square region (kRegionYards) of its map,
// so the lever still works without the file.
struct ZoneSample
{
    std::uint32_t Zone = 0;
    std::uint32_t Map = 0;
    float X = 0.0f;
    float Y = 0.0f;
};
constexpr float kZoneSampleYards = 1500.0f;
constexpr float kRegionYards = 1500.0f;
constexpr std::uint32_t kRegionBit = 0x80000000u;

std::vector<ZoneSample> ParseZoneSamples(std::istream& in, std::vector<std::string>& warnings);
std::vector<ZoneSample> LoadZoneSamples(std::string const& path, std::vector<std::string>& warnings);

struct ZoneIndex
{
    std::vector<std::uint32_t> OfSpot; // zone key per grind spot (a real zone id, or kRegionBit | region)
};
ZoneIndex BuildZoneIndex(GrindData const& grind, std::vector<ZoneSample> const& samples);

// One zone a bot could choose: its key, how many level-fit spots it has, the distance to its nearest one and the bots whose home it is.
struct ZoneOption
{
    std::uint32_t Key = 0;
    std::uint32_t Fit = 0;
    float NearestYards = 0.0f;
    std::size_t Bots = 0;
};

// The weight of a zone in the draw: fit (more spots, more weight, saturating at 10), distance (nearer is better, by the ZoneSpread share), and the soft
// cap on bots: 1 / (1 + (bots / cap)^2). The bot's own zone has twice the weight (stay, so a review moves few bots). Zero when out of reach.
double ZoneWeight(Settings const& settings, ZoneOption const& zone, bool current);

// The index into `zones` the bot takes as its home. `current` is the key of its present home (0 = none). Deterministic in (seed, guid, counter).
// With no zone within reach it takes the nearest; empty input gives npos.
std::size_t ChooseZone(Settings const& settings, std::vector<ZoneOption> const& zones, std::uint32_t current, std::uint64_t seed, std::uint64_t bot, std::uint64_t counter);

// Whether a bot with a home zone looks at the choice again now: it has no home, its home has no spot for its level, it crossed a review level (the boundary has a
// per-bot phase, so bots of one level do not review together), or its zone is over the cap and a draw of ZoneMigratePct passes.
bool ShouldReview(Settings const& settings, bool hasHome, std::uint32_t homeFit, std::uint32_t level, std::uint32_t reviewedAtLevel, std::size_t homeBots,
                  std::uint64_t seed, std::uint64_t bot, std::uint64_t counter);

// A spot as the policy sees it for one bot: its index in the grind data, the distance from the bot, the bots there now.
struct SpotOption
{
    std::size_t Index = 0;
    float Yards = 0.0f;
    std::size_t Bots = 0;
};

// The spots to try, best first: those within SpotReachYards of the nearest, minus the full ones (while another is left), ordered by
// distance * (1 + taste) + CrowdYards * bots. Off (not Enabled): the input order.
std::vector<std::size_t> OrderSpots(Settings const& settings, std::vector<SpotOption> spots, std::uint64_t seed, std::uint64_t bot);

// The stable taste of a bot for a spot in [-AffinityPct, +AffinityPct] percent.
double Taste(Settings const& settings, std::uint64_t seed, std::uint64_t bot, std::size_t spot);

// Quests. `quest` is a quest id.
bool SkipsQuest(Settings const& settings, std::uint64_t seed, std::uint64_t bot, std::uint32_t quest);      // never for a quest others need
std::uint64_t QuestOrderKey(Settings const& settings, std::uint64_t seed, std::uint64_t bot, std::uint32_t quest); // smaller first
std::size_t AreaStart(Settings const& settings, std::uint64_t seed, std::uint64_t bot, std::uint32_t quest, std::size_t areas);
float StartDistance(Settings const& settings, std::uint64_t seed, std::uint64_t bot, std::size_t start, float yards);

// Offsets and trips.
std::uint64_t StartOffsetMs(Settings const& settings, std::uint64_t seed, std::uint64_t bot, std::uint32_t what); // 0..StartOffsetMaxSec
std::uint64_t TripIntervalMs(Settings const& settings, std::uint64_t seed, std::uint64_t bot, std::uint64_t baseMs); // base +- TripSpreadPct
std::uint64_t TrainDelayMs(Settings const& settings, std::uint64_t seed, std::uint64_t bot, std::uint32_t level); // 0..TrainDelayMaxSec

} // namespace path_variety
