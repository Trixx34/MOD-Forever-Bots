// SPDX-License-Identifier: GPL-2.0-or-later
#include "reputation_plan.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>

namespace reputation {

namespace {

std::uint64_t Mix(std::uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

std::vector<std::string_view> Split(std::string_view line)
{
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (;;)
    {
        std::size_t const bar = line.find('|', start);
        if (bar == std::string_view::npos)
        {
            out.push_back(line.substr(start));
            return out;
        }
        out.push_back(line.substr(start, bar - start));
        start = bar + 1;
    }
}

bool Num(std::string_view s, std::int64_t& v)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    if (s.empty())
        return false;
    auto const r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

bool Flt(std::string_view s, float& v)
{
    std::string const text(s);
    char* end = nullptr;
    v = std::strtof(text.c_str(), &end);
    return end != text.c_str() && *end == '\0';
}

std::uint64_t SkipKey(Source::Kind kind, std::uint32_t id)
{
    return ((std::uint64_t(kind == Source::kKill ? 1 : 0) << 32) | id) + 1;
}

bool Skipped(Track const& track, Source const& s)
{
    std::uint64_t const key = SkipKey(s.What, s.Id);
    return std::find(track.Skipped.begin(), track.Skipped.end(), key) != track.Skipped.end();
}

std::int32_t StandingOf(Facts const& facts, std::uint32_t faction, bool& known)
{
    for (Standing const& s : facts.Standings)
        if (s.Faction == faction)
        {
            known = true;
            return s.Value;
        }
    return 0;
}

// The open world of Eastern Kingdoms and Kalimdor: kill sources elsewhere (dungeons, raids) belong to the dungeon planner, and the creature levels are not in the
// world database, so a kill source is only walked to where the bots already roam.
bool OpenWorld(std::uint32_t map)
{
    return map == 0 || map == 1;
}

} // namespace

std::vector<Target> ParseTargets(std::string_view text, std::vector<std::string>& errors)
{
    std::vector<Target> out;
    std::size_t lineNo = 0;
    while (!text.empty())
    {
        std::size_t const nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
        ++lineNo;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.remove_suffix(1);
        if (line.empty() || line.front() == '#')
            continue;
        auto const f = Split(line);
        std::int64_t faction, side, level, rank, tier;
        if (f.size() < 6 || !Num(f[0], faction) || !Num(f[2], side) || !Num(f[3], level) || !Num(f[4], rank) || !Num(f[5], tier) || faction <= 0 || side < 0 || side > 2 ||
            level < 1 || level > 60 || rank < 3 || rank > 7 || tier < 1)
        {
            errors.push_back("line " + std::to_string(lineNo) + ": not a target line");
            continue;
        }
        Target t;
        t.Faction = std::uint32_t(faction);
        t.Name = std::string(f[1]);
        t.Side = std::uint8_t(side);
        t.MinLevel = std::uint32_t(level);
        t.GoalRank = Rank(rank);
        t.Tier = std::uint32_t(tier);
        out.push_back(std::move(t));
    }
    return out;
}

std::vector<Source> ParseSources(std::string_view text, std::vector<std::string>& errors)
{
    std::vector<Source> out;
    std::size_t lineNo = 0;
    while (!text.empty())
    {
        std::size_t const nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
        ++lineNo;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.remove_suffix(1);
        if (line.empty() || line.front() == '#')
            continue;
        auto const f = Split(line);
        Source s;
        bool ok = false;
        std::int64_t faction, id, points, a, b, c, d;
        if (f[0] == "q" && f.size() >= 8 && Num(f[1], faction) && Num(f[2], id) && Num(f[3], points) && Num(f[4], a) && Num(f[5], b) && Num(f[6], c) && Num(f[7], d) &&
            faction > 0 && id > 0 && points > 0 && b >= 0 && b <= 2 && c >= 0 && c <= 2 && d >= 0 && d <= 7)
        {
            s.What = Source::Kind(c);   // 0 quest, 1 hand-in, 2 kills
            s.Level = std::uint32_t(a);
            s.Side = std::uint8_t(b);
            s.Cap = d == 0 ? kRankCount : Rank(d);
            ok = true;
        }
        else if (f[0] == "k" && f.size() >= 10 && Num(f[1], faction) && Num(f[2], id) && Num(f[3], points) && Num(f[4], a) && Num(f[5], b) && Num(f[9], c) &&
                 Flt(f[6], s.X) && Flt(f[7], s.Y) && Flt(f[8], s.Z) && faction > 0 && id > 0 && points > 0 && a >= 0 && a <= 7 && b >= 0 && c >= 1)
        {
            s.What = Source::kKill;
            s.Cap = a == 0 ? kRankCount : Rank(a);
            s.Map = std::uint32_t(b);
            s.Spawns = std::uint32_t(c);
            ok = true;
        }
        if (!ok)
        {
            errors.push_back("line " + std::to_string(lineNo) + ": not a source line");
            continue;
        }
        s.Faction = std::uint32_t(faction);
        s.Id = std::uint32_t(id);
        s.Points = std::int32_t(points);
        out.push_back(s);
    }
    return out;
}

PlanData BuildPlanData(std::vector<Target> targets, std::vector<Source> sources)
{
    PlanData data;
    data.Targets = std::move(targets);
    auto known = [&](std::uint32_t faction) {
        return std::any_of(data.Targets.begin(), data.Targets.end(), [&](Target const& t) { return t.Faction == faction; });
    };
    sources.erase(std::remove_if(sources.begin(), sources.end(), [&](Source const& s) { return !known(s.Faction); }), sources.end());
    std::stable_sort(sources.begin(), sources.end(), [](Source const& a, Source const& b) { return a.Faction < b.Faction; });
    data.Sources = std::move(sources);
    for (std::size_t i = 0; i < data.Sources.size();)
    {
        std::size_t j = i;
        while (j < data.Sources.size() && data.Sources[j].Faction == data.Sources[i].Faction)
            ++j;
        data.Range[data.Sources[i].Faction] = { i, j };
        i = j;
    }
    return data;
}

std::optional<std::size_t> ChooseTarget(PlanData const& data, Facts const& facts, std::uint64_t guid, std::uint32_t current)
{
    std::optional<std::size_t> best;
    std::uint64_t bestKey = 0;
    for (std::size_t i = 0; i < data.Targets.size(); ++i)
    {
        Target const& t = data.Targets[i];
        if (data.Range.find(t.Faction) == data.Range.end())
            continue;
        if (t.Side != 0 && facts.Side != 0 && t.Side != facts.Side)
            continue;
        if (facts.Level < t.MinLevel)
            continue;
        bool known = false;
        std::int32_t const standing = StandingOf(facts, t.Faction, known);
        if (RankOf(standing) >= t.GoalRank)
            continue;
        std::uint64_t const tierKey = std::uint64_t(0xFFFF - std::min<std::uint32_t>(t.Tier, 0xFFFF)) << 32;
        std::uint64_t const key = tierKey | (Mix(guid ^ (std::uint64_t(t.Faction) << 21)) & 0xFFFFFFFFull);
        if (t.Faction == current)
        {
            // Kept while it stays a candidate (a goal reached or a side change drops it above).
            best = i;
            break;
        }
        if (!best || key > bestKey)
        {
            best = i;
            bestKey = key;
        }
    }
    return best;
}

Step Advance(PlanData const& data, PlanSettings const& settings, Track& track, Facts const& facts, std::uint64_t guid, std::uint64_t nowMs, std::uint32_t* examined)
{
    Step step;
    if (!settings.Enabled)
        return step;

    if (track.State == Phase::kActive)
    {
        auto const range = data.Range.find(track.Faction);
        bool known = false;
        std::int32_t const standing = StandingOf(facts, track.Faction, known);
        Source const* current = nullptr;
        if (range != data.Range.end())
            for (std::size_t i = range->second.first; i < range->second.second; ++i)
                if (data.Sources[i].Id == track.SourceId && (data.Sources[i].What == Source::kKill) == (track.SourceKind == Source::kKill))
                {
                    current = &data.Sources[i];
                    break;
                }
        if (examined)
            *examined += range == data.Range.end() ? 0 : std::uint32_t(range->second.second - range->second.first);
        bool done = !current || (known && current->Cap != kRankCount && RankOf(standing) >= current->Cap);
        if (known && !done)
        {
            if (standing > track.StartStanding)
            {
                track.StartStanding = standing;   // it pays: the stall clock starts again
                track.StartedMs = nowMs;
            }
            else if (nowMs - track.StartedMs >= settings.StallMs)
            {
                track.Skipped[track.SkipNext++ % kSkipMemory] = SkipKey(current->What, current->Id);
                done = true;
            }
        }
        if (!done)
        {
            step.Valid = true;
            step.Src = current;
            step.Faction = track.Faction;
            return step;
        }
        track.State = Phase::kIdle;
        track.NextPlanMs = 0;   // plan again at once
    }

    if (nowMs < track.NextPlanMs)
        return step;
    track.NextPlanMs = nowMs + settings.ReplanMs;

    auto const target = ChooseTarget(data, facts, guid, 0);
    if (!target)
        return step;
    Target const& t = data.Targets[*target];
    auto const range = data.Range.find(t.Faction);
    std::size_t const begin = range->second.first, count = range->second.second - range->second.first;
    bool known = false;
    std::int32_t const standing = StandingOf(facts, t.Faction, known);
    Rank const rank = RankOf(standing);

    Source const* best = nullptr;
    std::uint64_t bestKey = 0;
    std::size_t const look = std::min<std::size_t>(count, settings.Budget);
    std::size_t const start = (Mix(guid ^ t.Faction) + track.Cursor) % count;
    for (std::size_t n = 0; n < look; ++n)
    {
        Source const& s = data.Sources[begin + (start + n) % count];
        if (examined)
            ++*examined;
        if (s.Side != 0 && facts.Side != 0 && s.Side != facts.Side)
            continue;
        if (s.Cap != kRankCount && rank >= s.Cap)
            continue;   // gives nothing at the bot's rank any more
        if (s.What == Source::kKill)
        {
            if (!OpenWorld(s.Map))
                continue;
        }
        else if (s.Level > facts.Level)
            continue;
        if (Skipped(track, s))
            continue;
        // Quests beat kills (a quest pays once and is a sure thing, the mobs take long); then the bigger reward; the hash only breaks ties between bots.
        std::uint64_t const score = (std::uint64_t(s.What == Source::kKill ? 0 : 1) << 40) | (std::uint64_t(s.Points) << 20);
        std::uint64_t const key = score | (Mix(guid ^ (std::uint64_t(s.Id) << 3)) & 0xFFFFF);
        if (!best || key > bestKey)
        {
            best = &s;
            bestKey = key;
        }
    }
    track.Cursor += std::uint32_t(look);
    if (!best)
        return step;
    track.Faction = t.Faction;
    track.State = Phase::kActive;
    track.SourceId = best->Id;
    track.SourceKind = best->What;
    track.StartStanding = standing;
    track.StartedMs = nowMs;
    step.Valid = true;
    step.Changed = true;
    step.Src = best;
    step.Faction = t.Faction;
    return step;
}

} // namespace reputation
