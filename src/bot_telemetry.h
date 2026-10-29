// BotTelemetry: a record of what each controlled bot did, written as CSV lines for runs that are read afterwards (overnight
// tests). Pure logic: it reads snapshots and hands lines to a sink, so it is unit-tested with hand-built snapshots
// (tests/telemetry_test.cpp).
//
// Every line is self-contained: it carries the bot's running totals, so the last line of a bot tells the whole story.
// Event lines mark fight starts and ends, target switches, targets that vanished during a fight, deaths and revivals,
// level changes, quest completions, and "stuck" (a fight with no change in health, position or target for StuckAfterMs).
// Columns 16 on: detail (free text, commas replaced by ';'), then class and race (ids, 0 = unknown), then copper (the bot's
// money; only on summary lines, empty on the others). Appended after detail so the first 15 columns and the detail position
// keep their meaning for existing readers.
// Summary lines are written every SummaryEveryMs per bot, so a run that stops writing has stopped.
#pragma once

#include "forever_module/tier2_session.h"

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

class BotTelemetry
{
public:
    using Sink = std::function<void(std::string const& line)>;

    struct Options
    {
        std::uint64_t SummaryEveryMs = 60000;
        std::uint64_t StuckAfterMs = 60000;
        // Class and race ids by bot guid, for the class and race columns (0 = unknown, also when unset).
        std::function<std::uint32_t(std::uint64_t botGuid)> ClassOf;
        std::function<std::uint32_t(std::uint64_t botGuid)> RaceOf;
    };

    BotTelemetry(Sink sink, Options options) : _sink(std::move(sink)), _options(options) {}

    // The CSV header line, written once when a file is new.
    static char const* Header();

    // Reads one snapshot. nowMs is the same monotonic clock the policy uses.
    void Observe(forever_module::WorldTickSnapshotView const& view, std::uint64_t nowMs);

private:
    struct Record
    {
        bool Seeded = false;
        bool Alive = true;
        bool CorpseValid = false;              // the corpse position last reported while the bot was down
        std::uint32_t CorpseMap = 0;
        float CorpseX = 0.0f, CorpseY = 0.0f, CorpseZ = 0.0f;
        std::uint32_t Level = 0;
        std::uint32_t LastSequence = 0;
        std::uint64_t NextSummaryMs = 0;

        // The current fight, if the bot has a threat target.
        std::uint64_t Target = 0;
        std::uint64_t FightStartMs = 0;
        std::uint32_t FightStartHealth = 0;

        // Stuck watch: the last time health, position or target changed during a fight.
        std::uint64_t LastChangeMs = 0;
        std::uint32_t LastHealth = 0;
        float LastX = 0.0f;
        float LastY = 0.0f;
        bool Stuck = false;

        // Quests seen complete (status 2) in the log and not yet gone from it: a hand-in is the quest leaving the log.
        std::set<std::uint32_t> CompleteInLog;

        // Totals since the module started.
        std::uint32_t Fights = 0;
        std::uint32_t TargetsGone = 0;
        std::uint32_t Deaths = 0;
        std::uint32_t CastsApplied = 0;
        std::uint32_t Refusals = 0;
        std::uint32_t StuckEpisodes = 0;
        std::uint64_t LongestFightMs = 0;
        std::uint64_t HealthLost = 0;
    };

    void Seed(Record& record, forever_module::BotSnapshot const& bot, std::uint64_t nowMs);
    void ReadResult(Record& record, forever_module::BotSnapshot const& bot);
    void EndFight(Record& record, forever_module::BotSnapshot const& bot, std::uint64_t nowMs, char const* reason, bool targetPresent);
    void CheckStuck(Record& record, forever_module::BotSnapshot const& bot, std::uint64_t nowMs);
    void CheckQuests(Record& record, forever_module::BotSnapshot const& bot, std::uint64_t nowMs);
    void Write(char const* kind, forever_module::BotSnapshot const& bot, Record const& record, std::uint64_t nowMs,
               std::string const& detail);

    Sink _sink;
    Options _options;
    std::unordered_map<std::uint64_t, Record> _bots; // bot guid -> record
};
