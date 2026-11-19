// SPDX-License-Identifier: GPL-2.0-or-later
// TraceBots: everything the module sees and does for a few named bots, one file per bot under TraceDir (doc/sim-ab.md, "Tracing a bot").
// Per bot: its events (every policy event line naming it), the actions decided for it each tick, every action result the core reports (refusals with
// their reason), and a state line (life, health, level, place, combat, target, hostiles near) whenever it changes and every heartbeat. The files rotate at
// TraceMaxMB / 2 with one old file kept, so a bot never has more than TraceMaxMB on disk. Nothing of it goes to the journal.
#pragma once

#include "rotating_file.h"

#include "forever_module/tier2_session.h"

#include <cstdint>
#include <algorithm>
#include <functional>
#include <span>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bot_trace
{

struct Options
{
    std::unordered_set<std::uint64_t> Bots;   // TraceBots
    std::string Dir;                           // TraceDir
    std::uint64_t MaxBytesPerBot = 20ull * 1024 * 1024;   // TraceMaxMB
    std::uint64_t HeartbeatMs = 5000;          // a state line at least this often, even when nothing changed
    float HostileYards = 40.0f;                // the hostile summary counts the hostile units within this distance
    // The wall-clock stamp of each line; the default is local time "2026-11-19T16:04:51.123".
    std::function<std::string()> Stamp;
};

// The hostile summary of one bot's nearby units (pure; for the state line and the tests).
struct Hostiles
{
    std::uint32_t Count = 0;          // hostile and alive within HostileYards
    std::uint32_t MaxLevel = 0;
    std::uint32_t AboveLevel = 0;     // of those, the ones at least 3 levels above the bot
    std::uint32_t Elites = 0;
    std::uint32_t Nearest = 0;        // entry of the nearest one (0 none)
    std::uint32_t NearestLevel = 0;
    float NearestYards = 0.0f;
};
Hostiles SummarizeHostiles(forever_module::BotSnapshot const& bot, std::span<forever_module::NearbyUnit const> nearby, float yards);

// "state life=0 hp=52% level=8 map=0 pos=-10606,294,40 combat=1 target=202 moving=1 casting=0 hostiles=3 hostile_max=10 above=2 elites=0 nearest=202:10@6"
std::string StateLine(forever_module::BotSnapshot const& bot, Hostiles const& hostiles);

class Tracer
{
public:
    explicit Tracer(Options options);
    bool Empty() const { return _options.Bots.empty(); }
    bool Wants(std::uint64_t guid) const { return _options.Bots.count(guid) != 0; }

    // A policy event line ("died: bot=235 ..."): written to the named bot's file when it is traced. Thread-safe.
    void Event(std::string const& line);
    // One snapshot tick and the actions decided for it (after the policy and the move gate): results, actions and the state line of each traced bot.
    void Observe(forever_module::WorldTickSnapshotView const& view, std::vector<forever_module::BotAction> const& actions, std::uint64_t nowMs);
    // A free line for a traced bot (the pump's notes). Thread-safe.
    void Note(std::uint64_t guid, std::string const& text);

    std::string PathOf(std::uint64_t guid) const;

private:
    struct PerBot
    {
        std::unique_ptr<RotatingFile> File;
        std::uint32_t SeenSequence = 0;
        std::string LastState;
        std::uint64_t LastStateMs = 0;
    };
    PerBot& BotOf(std::uint64_t guid);
    void Write(std::uint64_t guid, std::string const& text);

    Options _options;
    std::mutex _lock;
    std::unordered_map<std::uint64_t, PerBot> _bots;
};

} // namespace bot_trace
