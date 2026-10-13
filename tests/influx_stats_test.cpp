// Unit tests for BotInfluxStats and the Influx settings keys. Hand-built snapshots; no core needed.
#include "bot_influx.h"
#include "bot_settings.h"
#include "bot_telemetry.h"

#include <cstdio>

namespace {
int failures = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

using forever_module::BotSnapshot;
using forever_module::WorldTickSnapshotView;

BotSnapshot Bot(std::uint64_t guid, std::uint32_t level, std::uint32_t health = 100)
{
    BotSnapshot b;
    b.guid = guid;
    b.level = level;
    b.health = health;
    b.max_health = 100;
    return b;
}

std::string Line(influx_stats::Point const& p) { return influx_stats::EncodePoint(p); }

struct Rig
{
    std::vector<influx_stats::Point> points;
    BotInfluxStats stats;
    Rig() : stats([this](influx_stats::Point p) { points.push_back(std::move(p)); }, MakeOptions()) {}
    static BotInfluxStats::Options MakeOptions()
    {
        BotInfluxStats::Options o;
        o.SampleEveryMs = 1000;
        o.NowNs = [] { return std::int64_t(7); };
        o.ClassOf = [](std::uint64_t guid) { return guid <= 2 ? 1u : (guid == 3 ? 8u : 0u); };
        return o;
    }
    std::string Find(std::string const& prefix) const
    {
        for (auto const& p : points)
        {
            std::string l = Line(p);
            if (l.rfind(prefix, 0) == 0)
                return l;
        }
        return {};
    }
};
} // namespace

int main()
{
    CHECK(BotInfluxStats::ClassName(1) == "Warrior" && BotInfluxStats::ClassName(12) == "Demon Hunter" && BotInfluxStats::ClassName(99) == "class99");
    CHECK(BotInfluxStats::LevelBandLabel(0) == "0-9" && BotInfluxStats::LevelBandLabel(4) == "40-49" && BotInfluxStats::LevelBandLabel(11) == "110+");

    // Gauges: alive/dead, combat, class and level bands. The first Observe only starts the clock.
    {
        Rig rig;
        WorldTickSnapshotView view;
        BotSnapshot a = Bot(1, 5); a.in_combat = 1; a.moving = 1;
        BotSnapshot b = Bot(2, 12); b.group_size = 5;
        BotSnapshot c = Bot(3, 12);
        BotSnapshot dead = Bot(4, 60, 0);
        view.bots = {a, b, c, dead};
        rig.stats.Observe(view, 0);
        CHECK(rig.points.empty());
        rig.stats.Observe(view, 500);
        CHECK(rig.points.empty());
        rig.stats.Observe(view, 1000);
        CHECK(rig.Find("bots ") == "bots controlled=4i,alive=3i,dead=1i,in_combat=1i,moving=1i,in_group=1i 7");
        CHECK(rig.Find("bots_by_class,class=Warrior") == "bots_by_class,class=Warrior online=2i 7");
        CHECK(rig.Find("bots_by_class,class=Mage") == "bots_by_class,class=Mage online=1i 7");
        CHECK(rig.Find("bots_by_level,band=0-9") == "bots_by_level,band=0-9 online=1i 7");
        CHECK(rig.Find("bots_by_level,band=10-19") == "bots_by_level,band=10-19 online=2i 7");
        CHECK(rig.Find("bots_by_level,band=60-69").find("online=0i") != std::string::npos); // the dead bot is not online

        // Next sample: a class that dropped to zero stays as zero.
        rig.points.clear();
        view.bots = {c};
        rig.stats.Observe(view, 2000);
        CHECK(rig.Find("bots_by_class,class=Warrior") == "bots_by_class,class=Warrior online=0i 7");
    }

    // Counters from telemetry events, and quests handed in.
    {
        Rig rig;
        rig.stats.OnEvent("death", 1);
        rig.stats.OnEvent("death", 2);
        rig.stats.OnEvent("fight_start", 1);
        rig.stats.OnEvent("stuck", 1);
        rig.stats.OnEvent("level", 1);
        rig.stats.OnEvent("summary", 1); // not counted

        WorldTickSnapshotView view;
        BotSnapshot a = Bot(1, 5);
        a.quest_count = 2;
        a.quests[0].quest_id = 10; a.quests[0].status = 2;
        a.quests[1].quest_id = 11; a.quests[1].status = 1;
        view.bots = {a};
        rig.stats.Observe(view, 0);
        CHECK(rig.stats.QuestsDone() == 0);
        a.quest_count = 1; // quest 10 (complete) left the log; quest 11 (incomplete) is still there
        a.quests[0].quest_id = 11; a.quests[0].status = 1;
        view.bots = {a};
        rig.stats.Observe(view, 1000);
        CHECK(rig.stats.QuestsDone() == 1);
        CHECK(rig.Find("bot_events") == "bot_events deaths=2i,fights=1i,stuck_episodes=1i,level_ups=1i,quests_done=1i 7");
        // An incomplete quest that vanishes (abandoned) is not a quest done.
        a.quest_count = 0;
        view.bots = {a};
        rig.stats.Observe(view, 1100);
        CHECK(rig.stats.QuestsDone() == 1);
    }

    // The telemetry event sink drives the counters.
    {
        Rig rig;
        BotTelemetry telemetry{BotTelemetry::Sink{}, BotTelemetry::Options{}};
        telemetry.SetEventSink([&rig](std::string const& kind, std::uint64_t guid) { rig.stats.OnEvent(kind, guid); });
        WorldTickSnapshotView view;
        view.bots = {Bot(1, 5)};
        telemetry.Observe(view, 0);
        view.bots = {Bot(1, 5, 0)};
        telemetry.Observe(view, 100);
        CHECK(rig.stats.Deaths() == 1);
    }

    // Settings: Influx.* keys are handed on with the prefix removed; the sample interval is the module's own.
    {
        std::vector<std::string> w;
        BotSettings s = ParseBotSettings({{"Influx.Enabled", "1"}, {"Influx.Org", "o"}, {"Influx.SampleIntervalSec", "30"}}, w);
        CHECK(s.Influx.size() == 2 && s.Influx["Enabled"] == "1" && s.Influx["Org"] == "o");
        CHECK(s.InfluxSampleSec == 30 && w.empty());
        w.clear();
        s = ParseBotSettings({{"Influx.SampleIntervalSec", "0"}}, w);
        CHECK(s.InfluxSampleSec == 10 && w.size() == 1);
    }

    std::printf(failures ? "influx_stats: FAILED\n" : "influx_stats: ok\n");
    return failures ? 1 : 0;
}
