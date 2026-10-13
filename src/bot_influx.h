// BotInfluxStats: turns what the module sees of its bots into InfluxDB points. Pure logic over snapshots and telemetry events; the
// points go to a sink, so it is unit-tested with hand-built snapshots (tests/influx_stats_test.cpp).
//
// Measurements (the writer adds its prefix, "forever_" by default):
//   bots           controlled, alive, dead, in_combat, moving, in_group (gauges, from the latest snapshot)
//   bots_by_class  tag class=<name>; online (alive bots of that class; the module's class table)
//   bots_by_level  tag band=<0-9|10-19|...>; online
//   bot_events     deaths, fights, stuck_episodes, level_ups, quests_done (counts since the module started; use derivative())
// Not reported: auction house activity, because the bots do none yet.
#pragma once

#include "forever_module/tier2_session.h"
#include "line_protocol.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class BotInfluxStats
{
public:
    using PointSink = std::function<void(influx_stats::Point point)>;

    struct Options
    {
        std::uint64_t SampleEveryMs = 10000;
        // The class of a bot (0 = unknown). Unknown classes are counted in the bots measurement but not in bots_by_class.
        std::function<std::uint32_t(std::uint64_t botGuid)> ClassOf;
        // Wall clock in nanoseconds for the point timestamps.
        std::function<std::int64_t()> NowNs;
    };

    BotInfluxStats(PointSink sink, Options options) : _sink(std::move(sink)), _options(std::move(options)) {}

    // "Warrior" for class id 1 and so on; "class<N>" for an id not in the table.
    static std::string ClassName(std::uint32_t classId);
    // "40-49" for band 4; the last band reads "110+".
    static std::string LevelBandLabel(std::uint32_t band);
    static constexpr std::uint32_t kLevelBands = 12;

    // A telemetry event (BotTelemetry::SetEventSink). Counts the kinds listed above.
    void OnEvent(std::string const& kind, std::uint64_t botGuid);

    // Reads one snapshot: notices quests handed in, and every SampleEveryMs emits the gauge and counter points.
    void Observe(forever_module::WorldTickSnapshotView const& view, std::uint64_t nowMs);

    std::uint64_t Deaths() const { return _deaths; }
    std::uint64_t QuestsDone() const { return _questsDone; }

private:
    void Emit(forever_module::WorldTickSnapshotView const& view);

    PointSink _sink;
    Options _options;
    bool _started = false;
    std::uint64_t _nextSampleMs = 0;

    // Per bot: quests seen complete (status 2) and not yet gone from the log. One that vanishes was handed in.
    std::unordered_map<std::uint64_t, std::unordered_set<std::uint32_t>> _completeQuests;

    std::uint64_t _deaths = 0;
    std::uint64_t _fights = 0;
    std::uint64_t _stuckEpisodes = 0;
    std::uint64_t _levelUps = 0;
    std::uint64_t _questsDone = 0;
    std::unordered_set<std::uint32_t> _seenClasses;
};
