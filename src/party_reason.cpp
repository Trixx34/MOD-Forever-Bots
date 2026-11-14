// SPDX-License-Identifier: GPL-2.0-or-later
// Dynamic parties: whether a member is in its party this tick (party_reason.h).
#include "party_reason.h"

#include <algorithm>

namespace party_reason {

Why ReasonOf(Facts const& facts)
{
    if (facts.DungeonDue)
        return Why::Dungeon;
    if (facts.SharedQuest)
        return Why::Quest;
    if (facts.GroupQuest)
        return Why::GroupQuest;
    return Why::None;
}

Verdict Judge(Settings const& settings, Facts const& facts, Memory& memory, std::uint64_t nowMs)
{
    Verdict verdict;
    bool const was = memory.In;
    if (facts.Frozen)
    {
        verdict.In = was;
        verdict.Reason = was ? memory.LastWhy : Why::None;
        verdict.Cause = was ? Out::None : memory.LastOut;
        return verdict;
    }

    // Out: a member that was in leaves now and waits `holdMs` (RejoinMs, or the break) before it is invited again.
    auto const out = [&](Out cause, std::uint64_t holdMs)
    {
        if (was)
        {
            memory.SinceMs = nowMs;
            memory.OutUntilMs = std::max(memory.OutUntilMs, nowMs + holdMs);
        }
        memory.In = false;
        memory.LastWhy = Why::None;
        memory.LastOut = cause;
        verdict.In = false;
        verdict.Cause = cause;
        verdict.Changed = was;
        return verdict;
    };

    if (facts.OtherParty)
        return out(Out::OtherParty, settings.RejoinMs);
    if (facts.LeaderBusy)
        return out(Out::LeaderBusy, 0);
    if (!facts.SameMap)
        return out(Out::OtherMap, settings.RejoinMs);
    if (facts.OnErrand || facts.NeedsErrand)
        return out(Out::Errand, settings.RejoinMs);
    if (was && settings.ErrandEveryMs != 0 && memory.BreakDueMs != 0 && nowMs >= memory.BreakDueMs)
        return out(Out::ErrandBreak, settings.ErrandBreakMs);
    if (!was && nowMs < memory.OutUntilMs)
        return out(Out::Rejoin, 0);

    Why const why = ReasonOf(facts);
    if (was)
    {
        bool const staying = why != Why::None || nowMs < memory.SinceMs + settings.MinStayMs;
        if (facts.Distance > settings.KeepYards && why != Why::Dungeon)
            return out(Out::TooFar, settings.RejoinMs);
        if (!staying)
            return out(Out::NoReason, settings.RejoinMs);
        if (why != Why::None)
            memory.LastWhy = why;
        verdict.In = true;
        verdict.Reason = memory.LastWhy;
        return verdict;
    }

    // Joining: a reason, and near enough (a dungeon the party is due for gathers it from anywhere on the map: the leader's teleport takes it in).
    if (why == Why::None)
        return out(Out::NoReason, 0);
    if (facts.Distance > settings.JoinYards && why != Why::Dungeon)
        return out(Out::TooFar, 0);
    memory.In = true;
    memory.LastWhy = why;
    memory.LastOut = Out::None;
    memory.SinceMs = nowMs;
    memory.BreakDueMs = settings.ErrandEveryMs != 0 ? nowMs + settings.ErrandEveryMs : 0;
    verdict.In = true;
    verdict.Reason = why;
    verdict.Changed = true;
    return verdict;
}

bool LeaderOnBreak(Settings const& settings, bool leading, Memory& memory, std::uint64_t nowMs)
{
    if (settings.ErrandEveryMs == 0)
        return false;
    if (nowMs < memory.OutUntilMs)
        return true;
    if (!leading)
    {
        memory.BreakDueMs = 0;
        return false;
    }
    if (memory.BreakDueMs == 0)
        memory.BreakDueMs = nowMs + settings.ErrandEveryMs;
    if (nowMs < memory.BreakDueMs)
        return false;
    memory.OutUntilMs = nowMs + settings.ErrandBreakMs;
    memory.BreakDueMs = 0;
    return true;
}

char const* WhyName(Why why)
{
    switch (why)
    {
        case Why::Quest: return "quest";
        case Why::GroupQuest: return "group_quest";
        case Why::Dungeon: return "dungeon";
        default: return "none";
    }
}

char const* OutName(Out out)
{
    switch (out)
    {
        case Out::NoReason: return "no_reason";
        case Out::OtherMap: return "other_map";
        case Out::TooFar: return "too_far";
        case Out::Errand: return "errand";
        case Out::ErrandBreak: return "errand_break";
        case Out::Rejoin: return "rejoin_wait";
        case Out::LeaderBusy: return "leader_busy";
        case Out::OtherParty: return "other_party";
        default: return "none";
    }
}

}  // namespace party_reason
