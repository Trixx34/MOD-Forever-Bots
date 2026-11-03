// SPDX-License-Identifier: GPL-2.0-or-later
// Ambient chat: a bot says a short line now and then on the General or Trade channel of its zone, so a city or a field does not look empty
// (feature 4). Off by default (AmbientChat). Pure logic with the clock passed in; unit-tested in tests/ambient_chat_test.cpp. The pump thread
// picks the lines (Speaker::Pick), and the Run() thread sends them with BotSpeakChannel. The core checks its own rate (Bot.Chat.Ambient.*)
// and refuses Trade outside a capital, so a refused line is counted and not retried.
//
// data/ambient_lines.txt holds the lines, one per line, as "channels|min level|max level|event|text":
//   channels  g (General), t (Trade) or * (both)
//   min/max   the level band; 0 and 0 means any level
//   event     - for an ambient line; otherwise the event it answers (level: the bot just gained a level). An event line is said once per bot and event.
//   text      the line; {zone} (the bot's continent or map name), {level} and {class} are filled in. Lines longer than 255 bytes are skipped.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <istream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ambient {

enum class Channel : std::uint32_t
{
    General = 0,
    Trade = 1,
};

// A channel mask: General = 1, Trade = 2.
constexpr std::uint32_t kGeneralBit = 1;
constexpr std::uint32_t kTradeBit = 2;

struct LineDef
{
    std::uint32_t Channels = kGeneralBit; // the channels the line may be said on (a mask)
    std::uint32_t MinLevel = 0;           // 0 and 0: any level
    std::uint32_t MaxLevel = 0;
    std::string Event;                    // empty: an ambient line; else the event it answers
    std::string Text;
};

// The lines of a data file. A line that does not parse is skipped and counted in `errors`.
std::vector<LineDef> ParseLines(std::istream& in, std::size_t& errors);

// What the line is about: the bot's level, class and map, and the event, if any.
struct Context
{
    std::uint32_t Level = 0;
    std::string Class;   // "" when unknown (a line with {class} is then skipped)
    std::string Zone;    // "" when unknown (a line with {zone} is then skipped)
    std::string Event;   // "" for an ambient line
};

// The continent or map name for a map id (0 Eastern Kingdoms, 1 Kalimdor, 530 Outland, 571 Northrend), "" for the rest.
std::string ZoneName(std::uint32_t mapId);
// The class name for a class id (1 Warrior ... 11 Druid), "" for an unknown id.
std::string ClassName(std::uint32_t classId);

// The line with {zone}, {level} and {class} filled in. "" when a token has no value in `ctx`, a brace is left over, or the result is over
// 255 bytes (the core's chat limit).
std::string Fill(std::string const& text, Context const& ctx);

struct Limits
{
    std::uint32_t PerMinute = 2;          // lines a minute over all bots (module side)
    std::uint64_t BotCooldownMs = 300000; // the least time between two lines from one bot
    std::size_t DedupeRing = 50;          // a line said within this many lines is not said again
};

class Speaker
{
public:
    explicit Speaker(std::vector<LineDef> lines, Limits limits = {});

    // The line `bot` says now on `chan`, or nullopt: a rate limit, no line that fits, or an event the bot already had. Remembers what it returns.
    std::optional<std::string> Pick(std::uint64_t bot, Channel chan, Context const& ctx, std::uint64_t nowMs);

    std::size_t Size() const { return _lines.size(); }

private:
    bool RateAllows(std::uint64_t bot, std::uint64_t nowMs);
    bool Usable(LineDef const& line, Channel chan, Context const& ctx) const;

    std::vector<LineDef> _lines;
    Limits _limits;
    std::deque<std::uint64_t> _sent;                  // when the lines of the last minute went out
    std::map<std::uint64_t, std::uint64_t> _lastByBot; // bot -> when it last said a line
    std::map<std::uint64_t, std::string> _lastTextByBot;
    std::deque<std::string> _recent;                  // the last lines said, for the dedupe
    std::set<std::pair<std::uint64_t, std::string>> _eventDone; // (bot, event) already answered
};

// ---- Events from the world (backlog item 7): "boss_kill" (a boss of a bot's dungeon party died) and "local_death" (a bot died where another bot is) ----
// An event line is picked only with a context that carries its event (Speaker::Pick), so these lines are never said as ambient chat.
constexpr char const* kEventBossKill = "boss_kill";
constexpr char const* kEventLocalDeath = "local_death";

struct LifeSample
{
    std::uint64_t Guid = 0;
    std::uint32_t Map = 0;
    std::uint8_t LifeState = 0;     // 0 alive
    bool InCombat = false;
};

// Sees the bots' life states tick by tick. For each bot that has just died it names one other bot on the same map that is alive and out of combat, the one who
// "saw" it (at most one per death, and none when nobody is near), so one death gives one line at most.
class DeathWatch
{
public:
    std::vector<std::uint64_t> Observe(std::vector<LifeSample> const& bots);

private:
    std::map<std::uint64_t, std::uint8_t> _lastLife;
};

// Counts boss kills per key (a dungeon party): Grew() is true once when the count rose above what was last seen. The first sight only records the count.
class BossWatch
{
public:
    bool Grew(std::uint64_t key, std::size_t killed);

private:
    std::map<std::uint64_t, std::size_t> _last;
};

} // namespace ambient
