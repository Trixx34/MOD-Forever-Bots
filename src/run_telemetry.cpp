// SPDX-License-Identifier: GPL-2.0-or-later
#include "run_telemetry.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>

namespace run_telemetry
{

namespace
{

std::string Fixed1(float value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.1f", double(value));
    return buffer;
}

// A name as one word: spaces become underscores (the lines are split on spaces).
std::string Word(std::string text)
{
    std::replace(text.begin(), text.end(), ' ', '_');
    return text;
}

std::string Last(Progress const& last)
{
    if (last.IsBoss)
        return "boss:" + Word(last.Name) + "@" + std::to_string(last.AtS);
    return "route:" + std::to_string(last.Point) + "@" + std::to_string(last.AtS);
}

std::string Head(char const* kind, PartyId const& id, std::uint64_t tS)
{
    std::string scenario;
    std::uint32_t party = 1;
    SplitName(id.Name, scenario, party);
    return std::string(kind) + ": scenario=" + scenario + " party=" + std::to_string(party) + " variant=" + Word(id.Variant) + " t_s=" + std::to_string(tS);
}

} // namespace

std::string StallCause(StallCounters const& c)
{
    // Scores in thousandths, so the comparison needs no floating point.
    std::uint64_t const window = std::max<std::uint64_t>(c.WindowMs, 1);
    std::uint64_t const share = window / kShareDenominator * 2; // 40 % of the window
    auto const scoreOf = [](std::uint64_t value, std::uint64_t threshold) { return threshold == 0 ? 0 : value * 1000 / threshold; };
    struct Candidate
    {
        char const* Name;
        std::uint64_t Score;
    };
    Candidate const candidates[] = {
        { "dead_members", scoreOf(c.DeadMs, share) },
        { "fight_churn", scoreOf(c.Pulls, kChurnPulls) },
        { "route_refusals", scoreOf(c.RouteRefusals, kRefusalsMin) },
        { "resting", scoreOf(c.RestMs, share) },
    };
    Candidate best{ "unknown", 999 };
    for (Candidate const& candidate : candidates)
        if (candidate.Score >= 1000 && candidate.Score > best.Score)
            best = candidate;
    return best.Name;
}

void StallTracker::Start(std::uint64_t nowMs)
{
    _progressMs = nowMs;
    _verdictMs = 0;
    _count = 0;
}

void StallTracker::NoteProgress(std::uint64_t nowMs)
{
    _progressMs = nowMs;
    _verdictMs = 0;
}

bool StallTracker::Check(std::uint64_t nowMs)
{
    if (_progressMs == 0 || nowMs < _progressMs + kStallAfterMs)
        return false;
    if (_verdictMs != 0 && nowMs < _verdictMs + kStallAfterMs)
        return false;
    _verdictMs = nowMs;
    ++_count;
    return true;
}

void SplitName(std::string const& name, std::string& scenario, std::uint32_t& party)
{
    std::size_t const hash = name.rfind('#');
    scenario = name.substr(0, hash);
    party = 1;
    if (hash != std::string::npos && hash + 1 < name.size())
        party = std::uint32_t(std::strtoul(name.c_str() + hash + 1, nullptr, 10));
}

std::string FormatTimeline(PartyId const& id, Timeline const& l)
{
    return Head("party_tl", id, l.TS) + " leader=" + std::to_string(l.Leader) + " map=" + std::to_string(l.Map) + " pos=" + Fixed1(l.X) + "," + Fixed1(l.Y) + "," +
           Fixed1(l.Z) + " step=" + l.Step + " route=" + std::to_string(l.Point) + "/" + std::to_string(l.Points) + " target=" + std::to_string(l.Target) +
           " alive=" + std::to_string(l.Alive) + "/" + std::to_string(l.Total) + " fight=" + std::to_string(l.InFight) + " last=" + Last(l.Last) + l.Damage;
}

std::string FormatStall(PartyId const& id, Stall const& s)
{
    return Head("party_stalled", id, s.TS) + " cause=" + s.Cause + " no_progress_s=" + std::to_string(s.NoProgressS) + " pulls=" + std::to_string(s.Counters.Pulls) +
           " route_refusals=" + std::to_string(s.Counters.RouteRefusals) + " rest_s=" + std::to_string(s.Counters.RestMs / 1000) +
           " dead_s=" + std::to_string(s.Counters.DeadMs / 1000) + " fight_s=" + std::to_string(s.Counters.FightMs / 1000) + " stalls=" + std::to_string(s.Count) +
           " route=" + std::to_string(s.Point) + "/" + std::to_string(s.Points) + " last=" + Last(s.Last);
}

std::string FormatBossAttempt(PartyId const& id, BossAttempt const& b)
{
    return Head("boss_attempt", id, b.TS) + " boss=" + Word(b.Boss) + " stage=" + (b.End ? "end" : "start") + " dur_s=" + std::to_string(b.DurS) +
           " deaths=" + std::to_string(b.Deaths) + " outcome=" + b.Outcome + b.Damage;
}

std::string FormatResultFields(std::uint32_t stalls, std::string const& lastCause, std::uint64_t lastTS)
{
    if (stalls == 0)
        return {};
    return "stalled=" + std::to_string(stalls) + " last_stall=" + lastCause + "@" + std::to_string(lastTS);
}

std::string RunFile::IsoTime(std::int64_t epochSeconds)
{
    std::time_t const t = std::time_t(epochSeconds);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

void RunFile::Append(std::string const& line, std::uint64_t nowMs, std::int64_t epochSeconds, std::string* warn) noexcept
{
    try
    {
        if (_dir.empty())
            return;
        if (!_tried)
        {
            _tried = true;
            std::string path = _dir;
            if (path.back() != '/')
                path += '/';
            path += "run-telemetry.log";
            _out.open(path, std::ios::app);
            if (!_out && warn)
                *warn = "run telemetry: cannot open " + path + "; the run file stays off";
            _flushedMs = nowMs;
        }
        if (!_out)
            return;
        _out << IsoTime(epochSeconds) << ' ' << line << '\n';
        _dirty = true;
    }
    catch (...)
    {
    }
}

void RunFile::Flush(std::uint64_t nowMs) noexcept
{
    try
    {
        if (!_dirty || !_out || nowMs < _flushedMs + kFlushEveryMs)
            return;
        _out.flush();
        _flushedMs = nowMs;
        _dirty = false;
    }
    catch (...)
    {
    }
}

} // namespace run_telemetry
