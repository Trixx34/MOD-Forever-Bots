// Unit tests for BotTelemetry, on hand-built snapshots. No core needed.
#include "bot_telemetry.h"

#include <cstdio>
#include <string>
#include <vector>

using forever_module::BotActionStatus;
using forever_module::BotSnapshot;
using forever_module::MakeUnitRef;
using forever_module::NearbyUnit;
using forever_module::WorldTickSnapshotView;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

constexpr std::uint64_t kBot = 42;
constexpr std::uint64_t kEnemy = MakeUnitRef(8, 1, 2);

// Field positions in a telemetry line (see BotTelemetry::Header()).
constexpr std::size_t kKind = 1;
constexpr std::size_t kLevel = 3;
constexpr std::size_t kFights = 7;
constexpr std::size_t kTargetsGone = 8;
constexpr std::size_t kDeaths = 9;
constexpr std::size_t kCastsApplied = 10;
constexpr std::size_t kRefusals = 11;
constexpr std::size_t kStuckEpisodes = 12;
constexpr std::size_t kDetail = 15;

std::vector<std::string> Split(std::string const& line)
{
    std::vector<std::string> fields;
    std::string current;
    for (char c : line)
    {
        if (c == ',')
        {
            fields.push_back(current);
            current.clear();
        }
        else
            current.push_back(c);
    }
    fields.push_back(current);
    return fields;
}

// Collects the lines a BotTelemetry writes.
struct Recorder
{
    std::vector<std::string> lines;
    BotTelemetry telemetry;

    explicit Recorder(BotTelemetry::Options options)
        : telemetry([this](std::string const& line) { lines.push_back(line); }, options)
    {
    }

    std::size_t Count(char const* kind) const
    {
        std::size_t n = 0;
        for (std::string const& line : lines)
            if (Split(line)[kKind] == kind)
                ++n;
        return n;
    }

    // The last line of a kind, or an empty string.
    std::string Last(char const* kind) const
    {
        for (auto it = lines.rbegin(); it != lines.rend(); ++it)
            if (Split(*it)[kKind] == kind)
                return *it;
        return {};
    }
};

// A bot at x yards with the given health and threat target. Level 10 unless stated.
BotSnapshot Bot(std::uint32_t health, float x = 0.0f, std::uint64_t threat = 0, std::uint32_t level = 10)
{
    BotSnapshot bot;
    bot.guid = kBot;
    bot.health = health;
    bot.max_health = 1000;
    bot.level = level;
    bot.pos_x = x;
    bot.threat_target_guid = threat;
    return bot;
}

// A hostile unit at `x` yards that is alive.
NearbyUnit Enemy(float x)
{
    NearbyUnit unit;
    unit.guid = kEnemy;
    unit.pos_x = x;
    unit.health = 100;
    unit.max_health = 100;
    unit.is_hostile = true;
    return unit;
}

WorldTickSnapshotView View(BotSnapshot bot, std::vector<NearbyUnit> nearby = {}, std::uint64_t tick = 1)
{
    WorldTickSnapshotView view;
    view.tick_number = tick;
    view.max_nearby_units = 8;
    view.nearby.resize(view.max_nearby_units);
    bot.nearby_count = static_cast<std::uint32_t>(nearby.size());
    for (std::size_t i = 0; i < nearby.size(); ++i)
        view.nearby[i] = nearby[i];
    view.bots.push_back(bot);
    return view;
}

BotTelemetry::Options Quiet()
{
    BotTelemetry::Options options;
    options.SummaryEveryMs = 3600000; // no summary lines unless a test wants them
    options.StuckAfterMs = 60000;
    return options;
}

void FightLifecycle()
{
    Recorder rec(Quiet());
    rec.telemetry.Observe(View(Bot(1000)), 0);
    rec.telemetry.Observe(View(Bot(1000, 0, kEnemy), { Enemy(3.0f) }), 1000);
    CHECK(rec.Count("fight_start") == 1);

    // The target is gone when the threat drops: the fight ends as target_gone, after 4000 ms.
    rec.telemetry.Observe(View(Bot(900)), 5000);
    CHECK(rec.Count("fight_end") == 1);
    CHECK(rec.Last("fight_end").find("reason target_gone") != std::string::npos);
    CHECK(rec.Last("fight_end").find("duration_ms 4000") != std::string::npos);
    CHECK(Split(rec.Last("fight_end"))[kTargetsGone] == "1");
    CHECK(Split(rec.Last("fight_end"))[kFights] == "1");

    // Every line has the same number of fields as the header.
    std::size_t const fields = Split(BotTelemetry::Header()).size();
    for (std::string const& line : rec.lines)
        CHECK(Split(line).size() == fields);
}

void DeathEndsTheFight()
{
    Recorder rec(Quiet());
    rec.telemetry.Observe(View(Bot(1000)), 0);
    rec.telemetry.Observe(View(Bot(1000, 0, kEnemy), { Enemy(3.0f) }), 1000);
    rec.telemetry.Observe(View(Bot(0, 0, kEnemy), { Enemy(3.0f) }), 2000);
    CHECK(rec.Count("death") == 1);
    CHECK(Split(rec.Last("death"))[kDeaths] == "1");
    CHECK(rec.Last("fight_end").find("reason died") != std::string::npos);

    // Still dead: no more death lines. Revived: one alive line.
    rec.telemetry.Observe(View(Bot(0, 0, kEnemy), { Enemy(3.0f) }), 3000);
    CHECK(rec.Count("death") == 1);
    rec.telemetry.Observe(View(Bot(500)), 4000);
    CHECK(rec.Count("alive") == 1);
}

void StuckFightIsReportedOnce()
{
    Recorder rec(Quiet());
    rec.telemetry.Observe(View(Bot(1000)), 0);
    rec.telemetry.Observe(View(Bot(1000, 0, kEnemy), { Enemy(3.0f) }), 1000);

    // No change for a minute: stuck, once. Still no change later: no second report.
    rec.telemetry.Observe(View(Bot(1000, 0, kEnemy), { Enemy(3.0f) }), 61000);
    rec.telemetry.Observe(View(Bot(1000, 0, kEnemy), { Enemy(3.0f) }), 62000);
    CHECK(rec.Count("stuck") == 1);

    // The bot moves: the episode ends.
    rec.telemetry.Observe(View(Bot(1000, 5.0f, kEnemy), { Enemy(3.0f) }), 70000);
    CHECK(rec.Count("unstuck") == 1);
    CHECK(rec.Count("stuck") == 1);
}

void CountsCastsAndRefusals()
{
    BotTelemetry::Options options = Quiet();
    options.SummaryEveryMs = 1000;
    Recorder rec(options);

    BotSnapshot bot = Bot(1000);
    rec.telemetry.Observe(View(bot), 0);

    bot.last_action_sequence = 1;
    bot.last_action_status = BotActionStatus::kApplied;
    rec.telemetry.Observe(View(bot), 1000);

    bot.last_action_sequence = 2;
    bot.last_action_status = BotActionStatus::kOutOfRange;
    rec.telemetry.Observe(View(bot), 2000);

    // The same sequence seen again is not counted twice.
    rec.telemetry.Observe(View(bot), 2500);

    CHECK(Split(rec.Last("summary"))[kCastsApplied] == "1");
    CHECK(Split(rec.Last("summary"))[kRefusals] == "1");
}

void LevelChangeIsRecorded()
{
    Recorder rec(Quiet());
    rec.telemetry.Observe(View(Bot(1000, 0, 0, 10)), 0);
    rec.telemetry.Observe(View(Bot(1000, 0, 0, 11)), 1000);
    CHECK(rec.Count("level") == 1);
    CHECK(rec.Last("level").find("from 10 to 11") != std::string::npos);
    CHECK(Split(rec.Last("level"))[kLevel] == "11");
}

void SummaryIsWrittenOnItsInterval()
{
    BotTelemetry::Options options = Quiet();
    options.SummaryEveryMs = 60000;
    Recorder rec(options);
    rec.telemetry.Observe(View(Bot(1000)), 0);
    rec.telemetry.Observe(View(Bot(1000)), 59000);
    CHECK(rec.Count("summary") == 0);
    rec.telemetry.Observe(View(Bot(1000)), 60000);
    CHECK(rec.Count("summary") == 1);
}

void HeaderHasSixteenFields()
{
    CHECK(Split(BotTelemetry::Header()).size() == 16);
}

} // namespace

int main()
{
    HeaderHasSixteenFields();
    FightLifecycle();
    DeathEndsTheFight();
    StuckFightIsReportedOnce();
    CountsCastsAndRefusals();
    LevelChangeIsRecorded();
    SummaryIsWrittenOnItsInterval();

    if (failures == 0)
        std::printf("telemetry_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
