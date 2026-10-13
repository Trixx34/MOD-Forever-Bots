#include "bot_telemetry.h"

#include <cmath>
#include <span>
#include <sstream>

using forever_module::BotActionStatus;
using forever_module::BotSnapshot;
using forever_module::NearbyUnit;
using forever_module::WorldTickSnapshotView;

namespace {

constexpr float kMovedYards = 0.5f; // a bot that moved less than this in the stuck window has not moved

std::uint32_t Percent(std::uint64_t part, std::uint64_t whole)
{
    return whole ? static_cast<std::uint32_t>(part * 100 / whole) : 0;
}

// Keeps a free-text detail on one CSV field.
std::string Field(std::string text)
{
    for (char& c : text)
        if (c == ',' || c == '\n' || c == '\r')
            c = ';';
    return text;
}

} // namespace

char const* BotTelemetry::Header()
{
    return "time_ms,kind,bot,level,health_pct,power_pct,in_fight,fights,targets_gone,deaths,casts_applied,refusals,"
           "stuck_episodes,longest_fight_ms,health_lost,detail";
}

void BotTelemetry::Observe(WorldTickSnapshotView const& view, std::uint64_t nowMs)
{
    for (std::size_t i = 0; i < view.bots.size(); ++i)
    {
        BotSnapshot const& bot = view.bots[i];
        Record& record = _bots[bot.guid];
        if (!record.Seeded)
            Seed(record, bot, nowMs);

        ReadResult(record, bot);

        // Death and revival. A death ends the fight in progress.
        if (bot.health == 0 && record.Alive)
        {
            record.Alive = false;
            record.Deaths++;
            if (record.Target != 0)
                EndFight(record, bot, nowMs, "died", false);
            // Where it died, and the nearest live hostile creature: the snapshot does not name the killer, but this is most often it.
            std::ostringstream where;
            where << "map=" << bot.map_id << " x=" << std::lround(bot.pos_x) << " y=" << std::lround(bot.pos_y) << " z=" << std::lround(bot.pos_z);
            NearbyUnit const* nearest = nullptr;
            float nearestSq = 0.0f;
            for (NearbyUnit const& unit : view.NearbyOf(i))
            {
                if (!unit.is_hostile || unit.health == 0 || forever_module::UnitRefType(unit.guid) != 8)
                    continue;
                float const dx = unit.pos_x - bot.pos_x;
                float const dy = unit.pos_y - bot.pos_y;
                if (!nearest || dx * dx + dy * dy < nearestSq)
                {
                    nearest = &unit;
                    nearestSq = dx * dx + dy * dy;
                }
            }
            if (nearest)
                where << " nearest_hostile=" << forever_module::UnitRefEntry(nearest->guid) << " level=" << nearest->level
                      << " dist=" << std::lround(std::sqrt(nearestSq)) << (nearest->elite ? " elite" : "");
            where << (bot.in_combat ? " in_combat" : "");
            Write("death", bot, record, nowMs, where.str());
        }
        else if (bot.health != 0 && !record.Alive)
        {
            record.Alive = true;
            Write("alive", bot, record, nowMs, "");
        }

        if (bot.level != 0 && bot.level != record.Level)
        {
            std::ostringstream detail;
            detail << "from " << record.Level << " to " << bot.level;
            record.Level = bot.level;
            Write("level", bot, record, nowMs, detail.str());
        }

        if (record.Alive)
        {
            std::span<NearbyUnit const> nearby = view.NearbyOf(i);
            std::uint64_t const threat = bot.threat_target_guid;
            if (threat != 0 && record.Target == 0)
            {
                record.Target = threat;
                record.FightStartMs = nowMs;
                record.FightStartHealth = bot.health;
                record.LastChangeMs = nowMs;
                record.LastHealth = bot.health;
                record.LastX = bot.pos_x;
                record.LastY = bot.pos_y;
                record.Fights++;
                std::ostringstream detail;
                detail << "target " << threat;
                Write("fight_start", bot, record, nowMs, detail.str());
            }
            else if (threat != 0 && threat != record.Target)
            {
                std::ostringstream detail;
                detail << "from " << record.Target << " to " << threat;
                record.Target = threat;
                Write("target_switch", bot, record, nowMs, detail.str());
            }
            else if (threat == 0 && record.Target != 0)
            {
                bool present = false;
                for (NearbyUnit const& unit : nearby)
                    if (unit.guid == record.Target && unit.health > 0)
                        present = true;
                EndFight(record, bot, nowMs, present ? "no_threat" : "target_gone", present);
            }

            if (record.Target != 0)
                CheckStuck(record, bot, nowMs);
        }

        if (nowMs >= record.NextSummaryMs)
        {
            record.NextSummaryMs = nowMs + _options.SummaryEveryMs;
            Write("summary", bot, record, nowMs, "");
        }
    }
}

void BotTelemetry::Seed(Record& record, BotSnapshot const& bot, std::uint64_t nowMs)
{
    record.Seeded = true;
    record.Level = bot.level;
    record.Alive = bot.health != 0;
    record.NextSummaryMs = nowMs + _options.SummaryEveryMs;
    record.LastSequence = bot.last_action_sequence;
    record.LastHealth = bot.health;
    record.LastX = bot.pos_x;
    record.LastY = bot.pos_y;
    record.LastChangeMs = nowMs;
}

// Counts the result of the bot's last action, once per sequence number.
void BotTelemetry::ReadResult(Record& record, BotSnapshot const& bot)
{
    if (bot.last_action_sequence == 0 || bot.last_action_sequence == record.LastSequence)
        return;
    record.LastSequence = bot.last_action_sequence;
    if (bot.last_action_status == BotActionStatus::kApplied)
        record.CastsApplied++;
    else if (bot.last_action_status != BotActionStatus::kNone)
        record.Refusals++;
}

// Closes the fight in progress. targetPresent says whether the target was still in view and alive, which tells a
// target that was killed or fled apart from one that dropped the threat.
void BotTelemetry::EndFight(Record& record, BotSnapshot const& bot, std::uint64_t nowMs, char const* reason, bool targetPresent)
{
    std::uint64_t const duration = nowMs - record.FightStartMs;
    std::uint64_t const lost = record.FightStartHealth > bot.health ? record.FightStartHealth - bot.health : 0;
    if (duration > record.LongestFightMs)
        record.LongestFightMs = duration;
    record.HealthLost += lost;

    // The target was not in view when the fight ended: it was killed, or it left the bot's view.
    if (!targetPresent && std::string(reason) == "target_gone")
        record.TargetsGone++;

    std::ostringstream detail;
    detail << "reason " << reason << " target " << record.Target << " duration_ms " << duration << " health_lost " << lost;
    record.Target = 0;
    record.Stuck = false;
    Write("fight_end", bot, record, nowMs, detail.str());
}

// A fight with no change in health, position or target for StuckAfterMs is reported once per episode.
void BotTelemetry::CheckStuck(Record& record, BotSnapshot const& bot, std::uint64_t nowMs)
{
    bool const moved = std::fabs(bot.pos_x - record.LastX) > kMovedYards || std::fabs(bot.pos_y - record.LastY) > kMovedYards;
    if (moved || bot.health != record.LastHealth || bot.threat_target_guid != record.Target)
    {
        record.LastChangeMs = nowMs;
        record.LastHealth = bot.health;
        record.LastX = bot.pos_x;
        record.LastY = bot.pos_y;
        if (record.Stuck)
        {
            record.Stuck = false;
            Write("unstuck", bot, record, nowMs, "");
        }
        return;
    }

    if (!record.Stuck && nowMs - record.LastChangeMs >= _options.StuckAfterMs)
    {
        record.Stuck = true;
        record.StuckEpisodes++;
        std::ostringstream detail;
        detail << "no change for " << (nowMs - record.LastChangeMs) << " ms against target " << record.Target;
        Write("stuck", bot, record, nowMs, detail.str());
    }
}

void BotTelemetry::Write(char const* kind, BotSnapshot const& bot, Record const& record, std::uint64_t nowMs, std::string const& detail)
{
    std::ostringstream line;
    line << nowMs << ',' << kind << ',' << bot.guid << ',' << record.Level << ',' << Percent(bot.health, bot.max_health) << ','
         << Percent(bot.power, bot.max_power) << ',' << (record.Target != 0 ? 1 : 0) << ',' << record.Fights << ','
         << record.TargetsGone << ',' << record.Deaths << ',' << record.CastsApplied << ',' << record.Refusals << ','
         << record.StuckEpisodes << ',' << record.LongestFightMs << ',' << record.HealthLost << ',' << Field(detail);
    if (_sink)
        _sink(line.str());
    if (_events)
        _events(kind, bot.guid);
}
