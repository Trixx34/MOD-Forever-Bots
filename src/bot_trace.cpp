// SPDX-License-Identifier: GPL-2.0-or-later
#include "bot_trace.h"

#include "ab_split.h"
#include "bot_policy.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>

namespace bot_trace
{

namespace
{

std::string LocalStamp()
{
    auto const now = std::chrono::system_clock::now();
    std::time_t const seconds = std::chrono::system_clock::to_time_t(now);
    long const ms = long(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    char buffer[80];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03ld", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min,
                  local.tm_sec, ms);
    return buffer;
}

std::uint16_t ReasonOf(forever_module::BotSnapshot const& bot)
{
    for (auto const& result : bot.results)
        if (result.sequence != 0 && result.sequence == bot.last_action_sequence)
            return result.reason;
    return 0;
}

} // namespace

Hostiles SummarizeHostiles(forever_module::BotSnapshot const& bot, std::span<forever_module::NearbyUnit const> nearby, float yards)
{
    Hostiles out;
    float best = 0.0f;
    for (forever_module::NearbyUnit const& unit : nearby)
    {
        if (!unit.is_hostile || unit.health == 0)
            continue;
        float const dx = unit.pos_x - bot.pos_x;
        float const dy = unit.pos_y - bot.pos_y;
        float const dz = unit.pos_z - bot.pos_z;
        float const d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d > yards)
            continue;
        ++out.Count;
        out.MaxLevel = std::max(out.MaxLevel, unit.level);
        if (unit.level >= bot.level + 3)
            ++out.AboveLevel;
        if (unit.elite)
            ++out.Elites;
        if (out.Nearest == 0 || d < best)
        {
            best = d;
            out.Nearest = forever_module::UnitRefEntry(unit.guid);
            out.NearestLevel = unit.level;
            out.NearestYards = d;
        }
    }
    return out;
}

std::string StateLine(forever_module::BotSnapshot const& bot, Hostiles const& hostiles)
{
    unsigned const hp = bot.max_health ? unsigned(std::uint64_t(bot.health) * 100 / bot.max_health) : 0u;
    char buffer[400];
    std::snprintf(buffer, sizeof(buffer),
                  "state life=%u hp=%u%% level=%u map=%u pos=%d,%d,%d combat=%u target=%u moving=%u casting=%u group=%u hostiles=%u hostile_max=%u above=%u elites=%u nearest=",
                  unsigned(bot.life_state), hp, bot.level, bot.map_id, int(bot.pos_x), int(bot.pos_y), int(bot.pos_z), unsigned(bot.in_combat),
                  forever_module::UnitRefEntry(bot.threat_target_guid), unsigned(bot.moving), bot.casting_spell, bot.group_size, hostiles.Count, hostiles.MaxLevel,
                  hostiles.AboveLevel, hostiles.Elites);
    std::string line = buffer;
    if (hostiles.Nearest == 0)
        line += "none";
    else
        line += std::to_string(hostiles.Nearest) + ":" + std::to_string(hostiles.NearestLevel) + "@" + std::to_string(int(std::lround(hostiles.NearestYards)));
    return line;
}

Tracer::Tracer(Options options) : _options(std::move(options))
{
    if (!_options.Stamp)
        _options.Stamp = LocalStamp;
    if (!_options.Bots.empty() && !_options.Dir.empty())
    {
        std::error_code error;
        std::filesystem::create_directories(_options.Dir, error);
    }
}

std::string Tracer::PathOf(std::uint64_t guid) const
{
    return (std::filesystem::path(_options.Dir) / ("bot-" + std::to_string(guid) + ".trace")).string();
}

Tracer::PerBot& Tracer::BotOf(std::uint64_t guid)
{
    PerBot& per = _bots[guid];
    if (!per.File)
        per.File = std::make_unique<RotatingFile>(PathOf(guid), std::max<std::uint64_t>(_options.MaxBytesPerBot / 2, 4096), 1);
    return per;
}

void Tracer::Write(std::uint64_t guid, std::string const& text)
{
    PerBot& per = BotOf(guid);
    if (per.File->IsOpen())
        per.File->WriteLine(_options.Stamp() + " " + text);
}

void Tracer::Event(std::string const& line)
{
    std::uint64_t const guid = ab_split::BotOfLine(line);
    if (guid == 0 || !Wants(guid))
        return;
    std::lock_guard<std::mutex> lock(_lock);
    Write(guid, "event " + line);
}

void Tracer::Note(std::uint64_t guid, std::string const& text)
{
    if (!Wants(guid))
        return;
    std::lock_guard<std::mutex> lock(_lock);
    Write(guid, text);
}

void Tracer::Observe(forever_module::WorldTickSnapshotView const& view, std::vector<forever_module::BotAction> const& actions, std::uint64_t nowMs)
{
    if (Empty())
        return;
    std::lock_guard<std::mutex> lock(_lock);
    for (std::size_t i = 0; i < view.bots.size(); ++i)
    {
        forever_module::BotSnapshot const& bot = view.bots[i];
        if (!Wants(bot.guid))
            continue;
        PerBot& per = BotOf(bot.guid);
        // The core's answer to the last action, once per sequence: applied or refused, with the reason (layout 25).
        if (bot.last_action_sequence != 0 && bot.last_action_sequence != per.SeenSequence)
        {
            per.SeenSequence = bot.last_action_sequence;
            bool const applied = bot.last_action_status == forever_module::BotActionStatus::kApplied;
            Write(bot.guid, std::string(applied ? "result" : "refused") + " seq=" + std::to_string(bot.last_action_sequence) + " type=" +
                                std::to_string(unsigned(bot.last_action_type)) + " spell=" + std::to_string(bot.last_action_spell) + " status=" +
                                ActionStatusName(bot.last_action_status) + " reason=" + std::to_string(ReasonOf(bot)));
        }
        std::string const state = StateLine(bot, SummarizeHostiles(bot, view.NearbyOf(i), _options.HostileYards));
        if (state != per.LastState || nowMs - per.LastStateMs >= _options.HeartbeatMs)
        {
            Write(bot.guid, state);
            per.LastState = state;
            per.LastStateMs = nowMs;
        }
    }
    for (forever_module::BotAction const& action : actions)
    {
        if (!Wants(action.bot_guid))
            continue;
        char buffer[200];
        std::snprintf(buffer, sizeof(buffer), "action type=%u spell=%u target=%u dest=%d,%d,%d", unsigned(action.type), action.spell_id,
                      forever_module::UnitRefEntry(action.target_guid), int(action.dest_x), int(action.dest_y), int(action.dest_z));
        Write(action.bot_guid, buffer);
    }
}

} // namespace bot_trace
