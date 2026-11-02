// SPDX-License-Identifier: GPL-2.0-or-later
// Brackets (level spread): the bot population split into level brackets (1-9, 10-19, ..., 50-59, 60), each with a target share. The ramp places new
// bots in the bracket that is furthest short of its share, at a grind spot that is not full, so the population is spread over the levels instead of
// piling up at level 1. Presence revives the bots of short brackets first, a stall mover sends a bot that has not gained a level for a while to a
// spot one bracket down, and RetireLevel60 releases bots that reach 60. Off by default (BracketsEnabled). Pure logic with the time and the random draws
// passed in, unit-tested in tests/bracket_test.cpp.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace brackets {

constexpr std::size_t kCount = 7; // 1-9, 10-19, 20-29, 30-39, 40-49, 50-59, 60

// The share of the population each bracket should hold, as fractions that sum to 1 (all zero = no targets).
using Shares = std::array<double, kCount>;
// Bots counted in each bracket.
using Counts = std::array<std::uint32_t, kCount>;

// What happens to a bot that reaches level 60: nothing, or it is logged out and no longer counted (the bot stays in the database: the core has no
// delete or reset action yet, so it cannot be respawned at level 1 and is not removed).
enum class Retire : std::uint8_t
{
    kOff,
    kRelease,
};

struct Settings
{
    bool Enabled = false;                 // config BracketsEnabled
    Shares Targets{};                     // config BracketTargets (normalized to sum 1)
    std::uint32_t SpotCap = 0;            // config BracketSpotCap: bots per grind spot, 0 = no cap
    std::uint32_t StalledMinutes = 0;     // config BracketStalledMinutes: no level gain for this long marks a bot as stalled, 0 = off
    bool MoveDown = false;                // config BracketMoveDown: a stalled bot is sent to a spot one bracket down
    Retire RetireLevel60 = Retire::kOff;  // config RetireLevel60: off | release
};

struct Range
{
    std::uint32_t Low = 1;
    std::uint32_t High = 9;
};

// Bracket index (0..6) of a level; level 0 (unknown) counts as the first bracket.
std::size_t BracketOf(std::uint32_t level);
Range RangeOf(std::size_t bracket);
// The label used in the config file: "1-9", "10-19", ..., "50-59", "60".
std::string Label(std::size_t bracket);

// "1-9:25,10-19:20,...,60:5": each listed bracket with a non-negative weight (the shares are normalized, so 25,20,... need not sum to 100).
// Brackets not listed get 0. Returns false with `error` for an unknown or repeated bracket, a bad number, or when every weight is zero.
bool ParseTargets(std::string const& text, Shares& out, std::string& error);
// "off" or "release". Anything else, including "retire" (the core has no delete action yet), is an error.
bool ParseRetire(std::string const& text, Retire& out, std::string& error);

// The default split: heavier in the middle levels, where the spots are.
constexpr char const* kDefaultTargets = "1-9:25,10-19:20,20-29:15,30-39:12,40-49:12,50-59:11,60:5";

// How far a bracket is below its share once the bot about to be placed is counted: share * (total + 1) - count. Positive = short.
double Deficit(Shares const& shares, Counts const& counts, std::size_t bracket);
// The bracket to fill next: the lowest one below its share (so the low brackets fill first); when none is below, the one most short. Brackets with a
// zero share are never chosen. Returns kCount when no bracket has a share.
std::size_t NextBracket(Shares const& shares, Counts const& counts);

// A level inside the bracket, for a draw in [0, 1). Bracket 60 is always 60.
std::uint32_t LevelIn(std::size_t bracket, double unit);

// A weighted random pick among the spots whose load is below the cap (cap 0 = no cap). `weights` and `load` are per candidate; a weight of 0 counts as 1.
// Returns the candidate index, or nothing when every candidate is full.
std::optional<std::size_t> WeightedSpot(std::vector<std::uint32_t> const& weights, std::vector<std::uint32_t> const& load, std::uint32_t cap, double unit);

// The spots below the cap, in their order. When every spot is full, all of them are returned and `fellBack` is set (a bot still needs a spot).
std::vector<std::size_t> UnderCap(std::vector<std::size_t> const& spots, std::vector<std::uint32_t> const& load, std::uint32_t cap, bool& fellBack);

// Stall detection: a bot whose level has not moved up for StalledMinutes. The clock starts when the level was last seen to change (or first seen).
class LevelWatch
{
public:
    void Observe(std::uint64_t guid, std::uint32_t level, std::uint64_t nowMs);
    bool Stalled(std::uint64_t guid, std::uint64_t nowMs, std::uint32_t stalledMinutes) const;
    // A stalled bot is moved at most once per period.
    bool MayMoveDown(std::uint64_t guid, std::uint64_t nowMs, std::uint64_t periodMs) const;
    void NoteMoveDown(std::uint64_t guid, std::uint64_t nowMs);
    void Forget(std::uint64_t guid) { _bots.erase(guid); }

private:
    struct Entry
    {
        std::uint32_t Level = 0;
        std::uint64_t LevelSinceMs = 0; // when the level was last seen to change
        std::uint64_t MovedMs = 0;      // when the bot was last moved down a bracket (0 = never)
    };
    std::unordered_map<std::uint64_t, Entry> _bots;
};

} // namespace brackets
