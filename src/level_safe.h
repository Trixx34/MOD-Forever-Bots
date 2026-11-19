// SPDX-License-Identifier: GPL-2.0-or-later
// Level-safe places (2026-11-18, doc/level-safe.md): a low-level bot must not walk to, be revived in, or stay in a place far above its level. The 2026-11-18 sim
// (9 h, 400 bots): bot 13, a level 6 warrior, died 941 times at the Raven Hill graveyard in Duskwood (Skeletal Horror, level ~24): a quest walk took it from
// Northshire toward Duskwood-side givers, the ghost fallback put it at the nearest graveyard (Raven Hill) and the spirit healer revived it among level 24 creatures.
//
// The module has no zone level table. The level of a place is read from the grind spots (data/grind_spots.txt: creature level ranges with positions, made
// from the world database): the lowest minimum level of the spots within RadiusYards of the place. A place with fewer than kMinSpots spots that near is
// unknown, and an unknown place is never judged (today's behaviour): a town whose only spot is its guards (Stormwind 45, Darkshire 29 in the data) is not
// "high". A place is too high for a bot when that lowest minimum is above the bot's level plus a margin: MarginLevels for a walk to something new (a quest
// giver, a hunting area), EscapeMarginLevels (wider) for moving a bot that is already there (the escape, the graveyard, a hand-in walk), so a bot is never
// moved out of a place a walk would take it to. The lowest of the near spots (not their mean) keeps a place with a few strong creatures in a low zone (an
// elite, a rare) from counting as high. Checked against the data (2026-11-18): for a bot at the quest's own level, 309 of 9354 quest giver and ender points
// are too high with margin 5, 212 with margin 10; Raven Hill is 18, the Elwynn places of bot 13's deaths are 5.
//
// Graveyards: the core puts a ghost at the graveyard it chooses (the nearest for its zone) and the module has no graveyard table. Every place a ghost appears
// at is remembered, per map and side, as a graveyard (Graveyards). A ghost whose corpse or graveyard lies too high for it is taken to the nearest remembered
// graveyard whose place fits its level and asks the spirit healer there (the existing ghost fallback, with that graveyard instead of where it appeared).
//
// Pure logic, unit-tested in tests/level_safe_test.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

struct GrindData;

namespace level_safe
{

struct Options
{
    // All off here (the policy tests); the module settings default Walks, Graveyard and Escape to on (LevelSafeWalks, LevelSafeGraveyard, LevelSafeEscape).
    bool Walks = false;               // a quest walk to (or through) a place too high is dropped: the start or quest is blocked as for no route
    bool Graveyard = false;           // a ghost is revived at a graveyard that fits its level (ghost fallback), not at one far above it
    bool Escape = false;              // a solo bot standing in a place too high (out of combat) is teleported to a grind spot for its level
    std::uint32_t MarginLevels = 5;   // a walk to something new: a place is too high when its lowest creature level is above the bot's level plus this
    std::uint32_t EscapeMarginLevels = 10; // the escape, the graveyard and a hand-in walk: the same with this margin (never below MarginLevels)
    float RadiusYards = 250.0f;       // the grind spots within this of a place give its level
    float StepYards = 100.0f;         // a walk is checked every this many yards along the straight line, and at its end
    float SkipNearYards = 300.0f;     // the first part of a walk (where the bot stands now) is not checked, at least RadiusYards + 50: a bot in a high place may walk out
    std::uint64_t EscapeCooldownMs = 600000; // no second escape for the same bot before this
};

// The lowest creature level of the places of each map, from the grind spots.
class LevelMap
{
public:
    static constexpr std::size_t kMinSpots = 2;   // a place needs this many spots within the radius to be known
    void Build(GrindData const& data, float radiusYards);
    bool Empty() const { return _cells.empty(); }
    // The lowest MinLevel of the spots within the radius of (x, y) on `map`; 0 when there are fewer than kMinSpots (unknown).
    std::uint32_t PlaceMin(std::uint32_t map, float x, float y) const;
    // True when the place is known and its lowest level is above level + margin. A bot of level 0 (unknown) is never judged.
    bool TooHigh(std::uint32_t map, float x, float y, std::uint32_t level, std::uint32_t margin) const;
    // True when the place is known and its lowest level is at most level + margin.
    bool Fits(std::uint32_t map, float x, float y, std::uint32_t level, std::uint32_t margin) const;

    struct Hit
    {
        float X = 0.0f;
        float Y = 0.0f;
        std::uint32_t PlaceMin = 0;
    };
    // The first point of the straight walk from (fromX, fromY) to (toX, toY) that is too high, checked every `step` yards past the first `skipNear` yards
    // and at the end. Nothing when the walk is clear (unknown places count as clear).
    std::optional<Hit> FirstTooHigh(std::uint32_t map, float fromX, float fromY, float toX, float toY, std::uint32_t level, std::uint32_t margin, float step,
                                    float skipNear) const;

private:
    struct Entry
    {
        float X = 0.0f;
        float Y = 0.0f;
        std::uint32_t MinLevel = 0;
    };
    using CellKey = std::tuple<std::uint32_t, int, int>;
    CellKey KeyOf(std::uint32_t map, float x, float y) const;
    float _radius = 250.0f;
    std::map<CellKey, std::vector<Entry>> _cells;
};

// The graveyards seen: where ghosts appeared, per map. Side bits: 1 Alliance, 2 Horde (a ghost of unknown side marks none and fits nobody but side 0).
class Graveyards
{
public:
    struct Yard
    {
        std::uint32_t Map = 0;
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        std::uint8_t Sides = 0;
    };
    static constexpr float kSameYards = 40.0f;      // a ghost within this of a known graveyard appeared at that one
    static constexpr std::size_t kMaxPerMap = 400;  // bounded: more than any map has

    // A ghost of `side` (0 unknown, 1 Alliance, 2 Horde) appeared here.
    void Note(std::uint32_t map, float x, float y, float z, std::uint8_t side);
    // The graveyard on `map` nearest to (x, y) that a bot of `side` may use (seen by its side; side 0 takes any) and whose place fits `level` (known and not too
    // high). Null when none is known.
    // `skip` (DeathLoopBreaker): a graveyard it answers true for is not chosen (marked unsafe, or beside the loop's killer).
    Yard const* Fitting(LevelMap const& levels, std::uint32_t map, float x, float y, std::uint32_t level, std::uint32_t margin, std::uint8_t side,
                        std::function<bool(Yard const&)> const& skip = {}) const;
    // DeathLoopBreaker without level data: the graveyard on `map` nearest to (x, y) that a bot of `side` may use and `skip` does not refuse. Null when none.
    Yard const* Nearest(std::uint32_t map, float x, float y, std::uint8_t side, std::function<bool(Yard const&)> const& skip = {}) const;
    std::size_t Size() const;

private:
    std::unordered_map<std::uint32_t, std::vector<Yard>> _maps;
};

// Deaths per bot for the hourly `deaths_top` line: how many deaths, the median per bot, and how much of it the worst ten bots carry (the 2026-11-18 sim:
// two level 6 bots had 1446 of the deaths). Counts since the last line.
class DeathTally
{
public:
    void Note(std::uint64_t bot) { ++_deaths[bot]; ++_total; }
    std::uint64_t Total() const { return _total; }
    // The line (without the "forever-bots: " prefix): "deaths_top: deaths=N bots_online=B bots_died=D median=M median_died=MD top10=T top10_pct=P top=guid:n,...".
    // `online` is the number of bots in view (the median counts the ones that did not die as 0). The counts start again after it.
    std::string TakeLine(std::size_t online, std::uint64_t windowMs);

private:
    std::unordered_map<std::uint64_t, std::uint32_t> _deaths;
    std::uint64_t _total = 0;
};

} // namespace level_safe
