// SPDX-License-Identifier: GPL-2.0-or-later
// The shared danger map (2026-11-19, DangerMap, default off; doc/survival.md). The sim arm C (2026-11-19, 400 bots, 7.9 h) had 3.6 deaths per bot-hour, flat over three
// arms, and the deaths gathered at a few places: Raven Hill in Duskwood (481 deaths of two level 8-10 bots to one Skeletal Horror, entry 202) and four Skyborne killers
// on map 2991 (entries 251707, 254626, 251245, 256092: 1,692 deaths). Every bot learned each place alone, by dying there.
//
// One map for all bots, owned by the policy: every open-world death is noted in a cell of kCellYards (40 yards) of its map with the killer's entry and the dead bot's
// level band (levels 1-5, 6-10, 11-15, ...). A death's weight falls linearly from 1 to 0 over DecayMs (2 hours), so a place that stops killing is forgotten. The rate of a
// cell for a bot is the weighted deaths of bots of its band or a higher one (a place that kills level 10 bots kills level 6 bots as well), as deaths per hour: for a
// steady rate r the weights add up to r * decay_hours / 2. A point is judged by its own cell and the eight around it (the highest rate), so a place at a cell edge is
// not missed. A point is dangerous for a bot when that rate reaches DeathsPerHour (default 6).
//
// The policy uses it to skip dangerous grind spots, quest walks and explore walks (DangerMap) and to keep ghosts from reviving at a dangerous graveyard (SafeRevive).
// Pure logic, unit-tested in tests/survival_test.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace danger_map
{

constexpr float kCellYards = 40.0f;
constexpr std::uint32_t kBandLevels = 5;           // a level band: 1-5, 6-10, 11-15, ...
constexpr std::size_t kDeathsPerCell = 64;         // the newest deaths kept per cell
constexpr std::size_t kMaxCells = 20000;           // bounded: the cells with the oldest last death go first
constexpr std::uint64_t kHotLogEveryMs = 3600000;  // a cell's danger_cell line per band at most once per hour

struct Options
{
    bool Enabled = false;                 // the walks and spots of bots skip dangerous cells (DangerMap); off here (the tests) and in the module settings
    std::uint64_t DecayMs = 7200000;      // a death's weight falls to 0 over this (DangerMapDecayMin, default 120)
    std::uint32_t DeathsPerHour = 6;      // a cell is dangerous for a band from this rate on (DangerMapDeathsPerHour, default 6)
};

// The band of a level (0 for levels 1-5, 1 for 6-10, ...); level 0 (unknown) is band 0.
std::uint32_t BandOf(std::uint32_t level);
// "6-10" for band 1.
std::string BandName(std::uint32_t band);

// A cell that just became dangerous for a band (NoteDeath's answer), for the danger_cell line.
struct HotCell
{
    std::uint32_t Map = 0;
    float X = 0.0f;                 // the cell's centre
    float Y = 0.0f;
    std::uint32_t Band = 0;
    float Rate = 0.0f;              // deaths per hour for that band
    std::uint32_t Deaths = 0;       // deaths kept in the cell (all bands)
    std::uint32_t TopEntry = 0;     // the killer entry with the most deaths kept in the cell (0 unknown)
    std::uint32_t TopShare = 0;     // its share of the cell's deaths, percent
};

class Map
{
public:
    explicit Map(Options options = {}) : _options(options) {}
    void SetOptions(Options const& options) { _options = options; }
    Options const& Settings() const { return _options; }

    // A bot of `level` died at (x, y) on `map` to `killerEntry` (0 unknown). Answers the cell when this death made it dangerous for the dead bot's band (once per cell
    // and band per kHotLogEveryMs).
    std::optional<HotCell> NoteDeath(std::uint32_t map, float x, float y, std::uint32_t level, std::uint32_t killerEntry, std::uint64_t nowMs);
    // Deaths per hour at (x, y) for a bot of `level`: the highest of its cell and the eight around it.
    float Rate(std::uint32_t map, float x, float y, std::uint32_t level, std::uint64_t nowMs) const;
    // Rate >= DeathsPerHour. A bot of level 0 (unknown) is never judged.
    bool Dangerous(std::uint32_t map, float x, float y, std::uint32_t level, std::uint64_t nowMs) const;
    // The first point of the straight walk from (fromX, fromY) to (toX, toY) that is dangerous for `level`, checked every kCellYards past the first `skipNear`
    // yards and at the end. Nothing when the walk is clear.
    struct Hit
    {
        float X = 0.0f;
        float Y = 0.0f;
        float Rate = 0.0f;
    };
    std::optional<Hit> FirstDangerous(std::uint32_t map, float fromX, float fromY, float toX, float toY, std::uint32_t level, std::uint64_t nowMs, float skipNear) const;
    // The killer entry with the most deaths kept around (x, y) (its cell and the eight around it), 0 when none.
    std::uint32_t TopKiller(std::uint32_t map, float x, float y) const;

    // A skip by the policy (what: grind, quest, explore, revive), for the hourly fields.
    void NoteSkip(std::string const& what) { ++_skips[what]; }
    // " danger_cells=C danger_hot=H danger_deaths=D danger_skips=grind:a,quest:b danger_top=map:x,y:entry:rate,..." (leading space; the three hottest cells for any
    // band, rate for band 0). Prunes decayed cells. The skip counts start again after it.
    std::string TakeFields(std::uint64_t nowMs);
    std::size_t Cells() const { return _cells.size(); }
    void Prune(std::uint64_t nowMs);

private:
    struct Death
    {
        std::uint64_t AtMs = 0;
        std::uint32_t Entry = 0;
        std::uint32_t Band = 0;
    };
    struct Cell
    {
        std::vector<Death> Deaths;                              // oldest first
        std::map<std::uint32_t, std::uint64_t> HotLoggedMs;     // band -> the last danger_cell line
        std::uint64_t LastMs = 0;
    };
    using Key = std::tuple<std::uint32_t, int, int>;
    struct KeyHash
    {
        std::size_t operator()(Key const& key) const;
    };
    static Key KeyOf(std::uint32_t map, float x, float y);
    float CellRate(Cell const& cell, std::uint32_t band, std::uint64_t nowMs) const;
    Options _options;
    std::unordered_map<Key, Cell, KeyHash> _cells;
    std::map<std::string, std::uint64_t> _skips;
    std::uint64_t _deathsNoted = 0;
};

} // namespace danger_map
