// SPDX-License-Identifier: GPL-2.0-or-later
// Route filter (config RouteFilter, off by default): the explore and grind states stop choosing destinations the core has just refused for "no route".
// The core answers a refused move with BotActionStatus::kNoRoute (event no_route); until now the module only ended the walk, so the next pick was a fresh point
// that failed the same way (sim 3 h: 10,119 no_route events to 8,690 distinct destinations; prod 60 min: 13,590 events; doc/route-filter.md).
//
// Memory of refused keys, with time as a parameter (testable):
//   per bot   a refused key is not chosen by that bot again until its TTL runs out; each repeat doubles the TTL (TtlSec .. MaxTtlSec); a few entries per bot
//   shared    when SharedBots different bots were refused at the same key within the window, every bot skips it (TTL with the same backoff)
//   keys      a grind spot (SpotKey, the spot index: whatever the jitter around it) or a point cell (PointKey, CellYards squares of one map: explore points are new
//             each time, a cell stands for the area). The shared key of a grind spot also holds the cell the walk started in (SpotStartKey, StartCellYards): on prod
//             (2026-11-17, 90 min) 128 of the 146 spots that failed for one bot were reached by another, so "the spot is unreachable" is a statement about a start
//             region, not about the spot; a spot-only shared key blocked 3.4 good picks for each failed one it saved
//   reached   a key a bot arrived at is proven reachable: its entries are erased
//   reset     a bot's own entries are dropped when its level or map changes (the caller; ForgetBot)
// Bounded: PerBotMax entries per bot, MaxEntries shared; expired entries are dropped on the way.
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace route_filter {

struct Settings
{
    bool Enabled = false;                    // RouteFilter
    std::uint32_t TtlSec = 300;              // RouteFilterTtlSec (10..3600): first lifetime of a refused key; doubles with each repeat
    std::uint32_t MaxTtlSec = 3600;          // RouteFilterMaxTtlSec (60..86400): longest lifetime
    std::uint32_t CellYards = 8;             // RouteFilterCellYards (2..64): side of the square a refused explore point stands for
    std::uint32_t SharedBots = 3;            // RouteFilterSharedBots (0..20): different bots refused at a key before every bot skips it; 0 = never shared
    std::uint32_t StartCellYards = 200;      // RouteFilterStartCellYards (0..2000): a grind spot is shared per start cell of this size (the bots in one region); 0 = per spot only
    std::uint32_t MaxEntries = 4000;         // RouteFilterMaxEntries (100..100000): shared keys kept
    std::uint32_t ArrivedYards = 8;          // RouteFilterArrivedYards (2..30): a refusal this close to the destination is an arrival, not a failure
    std::uint32_t ExploreTries = 6;          // RouteFilterExploreTries (1..16): explore candidates looked at before the walk is given up for this time
    std::uint32_t ExploreBackoffMaxSec = 600;// RouteFilterExploreBackoffMaxSec (0..3600): the pause between explore walks doubles every 3 refusals in a row up to this; 0 = off
};

constexpr std::size_t kPerBotMax = 24;
constexpr std::uint64_t kSpotBit = 1ull << 63;

inline std::uint64_t SpotKey(std::size_t spot) { return kSpotBit | std::uint64_t(spot); }
// A grind spot as seen from a start cell (the shared key of a grind walk; startCellYards 0 gives SpotKey).
std::uint64_t SpotStartKey(std::size_t spot, float fromX, float fromY, std::uint32_t startCellYards);
// The cell of a point on a map: floor division, so cells on both sides of zero are the same size.
std::uint64_t PointKey(std::uint32_t map, float x, float y, std::uint32_t cellYards);

class Memory
{
public:
    explicit Memory(Settings settings = {}) : _settings(settings) {}
    void Configure(Settings const& settings) { _settings = settings; }
    Settings const& Config() const { return _settings; }

    // Is the key refused for this bot (its own entry, or a shared block)? Always false when the filter is off.
    // `shared` is the key of the shared entry (0 = the same as `key`): a grind walk is remembered per spot for the bot and per spot and start cell for everybody.
    bool Blocked(std::uint64_t bot, std::uint64_t key, std::uint64_t nowMs, std::uint64_t shared = 0);
    // The bot was refused at the key. True when this report made the shared key shared (every bot skips it from now on).
    bool Refuse(std::uint64_t bot, std::uint64_t key, std::uint64_t nowMs, std::uint64_t shared = 0);
    // The bot reached the key: its own entry and the shared one are erased (the place is reachable).
    void Reached(std::uint64_t bot, std::uint64_t key, std::uint64_t shared = 0);
    // The bot's own memory is dropped (level or map change, logout).
    void ForgetBot(std::uint64_t bot);

    std::size_t BotEntries(std::uint64_t bot) const;
    std::size_t SharedSize() const { return _shared.size(); }

private:
    struct Own
    {
        std::uint64_t Key = 0;
        std::uint64_t UntilMs = 0;   // blocked until
        std::uint64_t ForgetMs = 0;  // the failure count is kept until here
        std::uint32_t Fails = 0;
    };
    struct Shared
    {
        std::vector<std::uint64_t> Bots; // distinct bots refused, at most SharedBots
        std::uint64_t UntilMs = 0;
        std::uint64_t ForgetMs = 0;
        std::uint32_t Fails = 0;
    };
    std::uint64_t Lifetime(std::uint32_t fails) const;
    void PruneShared(std::uint64_t nowMs);

    Settings _settings;
    std::unordered_map<std::uint64_t, std::vector<Own>> _own;
    std::unordered_map<std::uint64_t, Shared> _shared;
};

// The pause before the next explore walk after `streak` refusals in a row: baseMs doubled every 3 refusals, at most capSec. baseMs when off or streak 0.
std::uint64_t ExploreIdleMs(Settings const& settings, std::uint64_t baseMs, std::uint32_t streak);

} // namespace route_filter
