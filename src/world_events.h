// SPDX-License-Identifier: GPL-2.0-or-later
// World events and holidays (WorldEvents = 1, off by default): while a game event (the core's game_event table: Lunar Festival, Hallow's End, Winter
// Veil ...) is active, a small share of the idle bots walks to a site of that event (an event NPC or area from data/world_events.txt), takes the
// event's talk quests there and hands them in, and stays a moment. Pure logic: no core, no clock, no file access (tests/world_events_test.cpp). The
// module feeds in the bots that are free and the active event ids and carries out the starts and ends this returns.
//
// Which events are active is not in the snapshot or in any event the core sends today. The module takes the list from the key WorldEventsActive (event
// ids, set by the operator) and, once the core reports it (doc/world-events.md, "What the core would need"), from the core: ActiveEvents::Set.
//
// The caps (all keys): at most WorldEventsPct percent of the bots (and at most WorldEventsMaxBots) are on a visit at once, at most WorldEventsPerSite per
// site, at most WorldEventsStartsPerTick starts a tick; a bot makes a visit no sooner than WorldEventsGapMin minutes after its last one, never visits a
// site twice while the event runs, and a visit is cut off after WorldEventsMaxMin minutes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace world_events {

// Sides as the data writes them: 0 both, 1 Alliance, 2 Horde (the same ids as errand::kAlliance and kHorde).
struct Site
{
    std::uint32_t Event = 0;               // the game_event entry
    std::uint32_t Side = 0;
    std::uint32_t Map = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    std::uint32_t MinLevel = 0;
    std::uint32_t MaxLevel = 0;
    std::string Name;
    std::vector<std::uint32_t> Quests;     // quests the bot accepts and hands in at the site (talk quests: no objectives); may be empty
};

// Parses the table: one site per line, "event|side|map|x|y|z|minLevel|maxLevel|name|quests" with quests a comma separated list of quest ids or empty.
// Blank lines and lines starting with '#' are skipped. A bad line is left out and described in `errors` ("line N: ..."), so one bad row does not drop
// the table.
std::vector<Site> ParseSites(std::string_view text, std::vector<std::string>& errors);

// Parses a list of event ids ("7, 12" or "7,12"). A bad item is left out and described in `errors`. Duplicates are dropped; order is kept.
std::vector<std::uint32_t> ParseEventIds(std::string_view text, std::vector<std::string>& errors);

// The active events: the operator's list (key WorldEventsActive) together with the core's report when there is one. The report replaces the previous
// report as a whole (an event the core no longer lists is over). Not thread safe: the module keeps it on the thread that runs the scheduler.
class ActiveEvents
{
public:
    void SetConfigured(std::vector<std::uint32_t> ids) { _configured = std::move(ids); }
    void SetReported(std::vector<std::uint32_t> ids) { _reported = std::move(ids); _hasReport = true; }
    bool HasReport() const { return _hasReport; }
    // The ids to act on: the report when the core gave one, else the configured list.
    std::vector<std::uint32_t> Current() const { return _hasReport ? _reported : _configured; }

private:
    std::vector<std::uint32_t> _configured;
    std::vector<std::uint32_t> _reported;
    bool _hasReport = false;
};

struct Settings
{
    bool Enabled = false;                  // WorldEvents
    std::uint32_t SharePct = 2;            // WorldEventsPct: percent of the bots on a visit at once (0 = none)
    std::uint32_t MaxBots = 10;            // WorldEventsMaxBots: never more than this many at once
    std::uint32_t MaxPerSite = 3;          // WorldEventsPerSite
    std::uint32_t MaxMinutes = 10;         // WorldEventsMaxMin: one visit is cut off after this
    std::uint32_t GapMinutes = 180;        // WorldEventsGapMin: a bot's next visit starts no sooner after its last one ended
    std::uint32_t MaxStartsPerTick = 1;    // WorldEventsStartsPerTick
    std::uint64_t Seed = 1;                // RandomSeed
};

// One bot as the module sees it this tick.
struct Bot
{
    std::uint64_t Guid = 0;
    bool Available = false;                // online, alive, out of combat, not in flight, not on an errand
    bool Called = false;                   // in a party or called to a dungeon: a visit is abandoned
    std::uint32_t Side = 0;                // 0 = unknown: only a site for both sides suits
    std::uint32_t Level = 0;
    std::uint32_t Map = 0;
    float X = 0.0f;
    float Y = 0.0f;
};

struct Start
{
    std::uint64_t Guid = 0;
    std::size_t Site = 0;                  // index into the site table
};

enum class EndReason : std::uint8_t { kDone, kTimeLimit, kCalled, kUnavailable, kEventOver };

struct End
{
    std::uint64_t Guid = 0;
    std::size_t Site = 0;
    EndReason Why = EndReason::kDone;
    std::uint64_t TookMs = 0;
};

struct Decisions
{
    std::vector<Start> Starts;
    std::vector<End> Ends;
};

// How many bots may be on a visit at once for `botCount` bots in the feed: SharePct percent rounded up (a share above 0 allows at least one), at
// most MaxBots; 0 when the feature is off or the share is 0.
std::uint32_t AllowedAtOnce(Settings const& settings, std::size_t botCount);

// Whether a site suits a bot: its event is in `active`, the side fits (a site for both sides suits every bot, a one-sided site needs a known side that
// matches), the level is in range and the bot is on the site's map (a bot never changes continent for a visit).
bool Suits(Site const& site, Bot const& bot, std::vector<std::uint32_t> const& active);

class Scheduler
{
public:
    explicit Scheduler(Settings const& settings) : _settings(settings) {}
    void SetSettings(Settings const& settings) { _settings = settings; }

    // One step: ends the visits that are over (time limit, the bot called or no longer free, the event over), then starts new ones within the caps.
    Decisions Tick(std::uint64_t nowMs, std::vector<Site> const& sites, std::vector<std::uint32_t> const& active, std::vector<Bot> const& bots);

    // A visit the bot finished (it did its quests and stayed its time, or the walk failed): the gap and the visited site are recorded.
    // Returns how long the visit took (0 when the bot was not on one).
    std::uint64_t Finish(std::uint64_t guid, std::uint64_t nowMs);

    bool Active(std::uint64_t guid) const { return _running.count(guid) != 0; }
    // The site index the bot is visiting (npos when it is on no visit).
    std::size_t SiteOf(std::uint64_t guid) const { auto const it = _running.find(guid); return it == _running.end() ? std::size_t(-1) : it->second.Site; }
    std::size_t RunningCount() const { return _running.size(); }

private:
    struct Running
    {
        std::size_t Site = 0;
        std::uint64_t StartMs = 0;
    };

    Settings _settings;
    std::unordered_map<std::uint64_t, Running> _running;
    std::unordered_map<std::uint64_t, std::uint64_t> _lastEndMs;
    std::set<std::pair<std::uint64_t, std::size_t>> _visited;  // (bot, site) pairs done while the site's event runs
};

// The journal lines ("key=value" with no commas, logged as "forever-bots: event <line>"):
//   world_event_start bot=<guid> event=<id> site=<name> quests=<n>
//   world_event_done bot=<guid> event=<id> site=<name> took_s=<n>
//   world_event_abandoned reason=timeout|called|unavailable|event_over|failed bot=<guid> event=<id> site=<name> took_s=<n>
std::string StartLine(std::uint64_t guid, Site const& site);
std::string EndLine(std::uint64_t guid, Site const& site, EndReason why, bool failed, std::uint64_t tookMs);

} // namespace world_events
