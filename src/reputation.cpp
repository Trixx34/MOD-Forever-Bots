// SPDX-License-Identifier: GPL-2.0-or-later
#include "reputation.h"

#include <algorithm>
#include <charconv>

namespace reputation {

namespace {

constexpr std::int32_t kStarts[kRankCount] = { -42000, -6000, -3000, 0, 3000, 9000, 21000, 42000 };
constexpr char const* kNames[kRankCount] = { "Hated", "Hostile", "Unfriendly", "Neutral", "Friendly", "Honored", "Revered", "Exalted" };

std::string_view Trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

bool ToUint(std::string_view s, std::uint32_t& out)
{
    s = Trim(s);
    if (s.empty())
        return false;
    auto const r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

std::uint64_t Mix(std::uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

} // namespace

std::int32_t RankStart(Rank rank)
{
    return kStarts[std::min<std::size_t>(rank, kRankCount - 1)];
}

Rank RankOf(std::int32_t standing)
{
    int rank = 0;
    for (int i = 1; i < kRankCount; ++i)
        if (standing >= kStarts[i])
            rank = i;
    return Rank(rank);
}

char const* RankName(Rank rank)
{
    return kNames[std::min<std::size_t>(rank, kRankCount - 1)];
}

std::int32_t PointsTo(Rank rank, std::int32_t standing)
{
    return std::max<std::int32_t>(0, RankStart(rank) - standing);
}

std::vector<Goal> ParseGoals(std::string_view text, std::vector<std::string>& errors)
{
    std::vector<Goal> goals;
    std::size_t lineNo = 0;
    while (!text.empty())
    {
        std::size_t const nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        ++lineNo;
        line = Trim(line);
        if (line.empty() || line.front() == '#')
            continue;
        std::vector<std::string_view> f;
        while (true)
        {
            std::size_t const bar = line.find('|');
            f.push_back(line.substr(0, bar));
            if (bar == std::string_view::npos)
                break;
            line = line.substr(bar + 1);
        }
        auto const fail = [&](char const* why) { errors.push_back("line " + std::to_string(lineNo) + ": " + why); };
        if (f.size() != 9)
        {
            fail("expected 9 fields");
            continue;
        }
        Goal g;
        std::uint32_t side = 0, rank = 0;
        if (!ToUint(f[0], g.Faction) || g.Faction == 0 || !ToUint(f[2], side) || side > 2 || !ToUint(f[3], g.MinLevel) || !ToUint(f[4], g.MaxLevel) ||
            !ToUint(f[5], rank) || rank >= kRankCount || !ToUint(f[6], g.Items) || !ToUint(f[7], g.Vendors) || !ToUint(f[8], g.Quests))
        {
            fail("a number is missing or out of range");
            continue;
        }
        if (g.MaxLevel < g.MinLevel)
        {
            fail("max level below min level");
            continue;
        }
        g.Name = std::string(Trim(f[1]));
        g.Side = std::uint8_t(side);
        g.MinRank = Rank(rank);
        goals.push_back(std::move(g));
    }
    return goals;
}

std::optional<std::size_t> ChooseGoal(std::vector<Goal> const& goals, Facts const& facts, std::uint64_t guid, std::uint32_t current)
{
    std::optional<std::size_t> best;
    std::uint64_t bestKey = 0;
    for (std::size_t i = 0; i < goals.size(); ++i)
    {
        Goal const& g = goals[i];
        if (g.Vendors == 0 || g.Items == 0)
            continue;   // nobody sells the gear: nothing to work for
        if (g.Side != 0 && facts.Side != 0 && g.Side != facts.Side)
            continue;
        if (facts.Level + kLevelSlack < g.MinLevel)
            continue;   // its gear is still out of reach
        bool maxed = false;
        for (Standing const& s : facts.Standings)
            if (s.Faction == g.Faction && s.Value >= RankStart(kExalted))
                maxed = true;
        if (maxed)
            continue;
        if (g.Faction == current)
            return i;   // keep the goal while it stays a candidate
        std::uint64_t const key = (std::uint64_t(g.Quests) << 32) | (Mix(guid ^ (std::uint64_t(g.Faction) << 17)) & 0xFFFFFFFFull);
        if (!best || key > bestKey)
        {
            best = i;
            bestKey = key;
        }
    }
    return best;
}

bool TripDue(Settings const& settings, Facts const& facts, Book const& book, std::uint64_t nowMs)
{
    return settings.Enabled && facts.VendorValid && facts.Level >= settings.MinLevel && facts.Copper >= settings.MinCopper && facts.FreeSlots >= 1 &&
           nowMs >= book.NextTripMs;
}

void NoteTrip(Settings const& settings, Book& book, std::uint64_t nowMs)
{
    book.NextTripMs = nowMs + settings.TripEveryMs;
}

void NoteResult(Settings const& settings, Book& book, std::uint64_t nowMs, bool bought)
{
    if (bought)
    {
        book.Failures = 0;
        return;
    }
    ++book.Failures;
    std::uint64_t wait = settings.FailBackoffMs;
    for (std::uint32_t i = 1; i < book.Failures && wait < settings.MaxBackoffMs; ++i)
        wait *= 2;
    book.NextTripMs = nowMs + std::min(wait, settings.MaxBackoffMs);
}

} // namespace reputation
