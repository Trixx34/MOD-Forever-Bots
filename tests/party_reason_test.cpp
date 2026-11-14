// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the dynamic parties (party_reason.h): when a member joins, stays in and leaves its world party.
#include "party_reason.h"

#include <cstdio>
#include <string_view>

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

using namespace party_reason;

Settings On()
{
    Settings settings;
    settings.Enabled = true;
    return settings;
}

Facts Near(bool quest)
{
    Facts facts;
    facts.SameMap = true;
    facts.Distance = 20.0f;
    facts.SharedQuest = quest;
    return facts;
}

void NoReasonNoParty()
{
    Memory memory;
    Verdict const verdict = Judge(On(), Near(false), memory, 1000);
    CHECK(!verdict.In && verdict.Cause == Out::NoReason && !verdict.Changed);
}

void ASharedQuestNearbyJoins()
{
    Memory memory;
    Verdict const verdict = Judge(On(), Near(true), memory, 1000);
    CHECK(verdict.In && verdict.Reason == Why::Quest && verdict.Changed);
    CHECK(memory.In && memory.SinceMs == 1000 && memory.BreakDueMs == 1000 + On().ErrandEveryMs);
    // the same facts next tick: still in, no change
    Verdict const again = Judge(On(), Near(true), memory, 2000);
    CHECK(again.In && !again.Changed);
}

void TooFarToJoinButADungeonGathersFromTheWholeMap()
{
    Memory memory;
    Facts far = Near(true);
    far.Distance = 600.0f;
    CHECK(Judge(On(), far, memory, 1000).Cause == Out::TooFar);
    far.SharedQuest = false;
    far.DungeonDue = true;
    Verdict const dungeon = Judge(On(), far, memory, 2000);
    CHECK(dungeon.In && dungeon.Reason == Why::Dungeon);
}

void NeverAcrossMaps()
{
    // Desolace and Ironforge: a shared quest is no reason to group across maps.
    Memory memory;
    Facts away = Near(true);
    away.SameMap = false;
    away.Distance = 1e9f;
    CHECK(!Judge(On(), away, memory, 1000).In);

    // In the party, then the leader changes map: the member leaves at once (a fight does not hold it there) and waits RejoinMs.
    Memory in;
    Judge(On(), Near(true), in, 1000);
    away.Frozen = false;
    Verdict const left = Judge(On(), away, in, 5000);
    CHECK(!left.In && left.Changed && left.Cause == Out::OtherMap);
    CHECK(in.OutUntilMs == 5000 + On().RejoinMs);
    Verdict const wait = Judge(On(), Near(true), in, 6000);   // back on one map, but too soon
    CHECK(!wait.In && wait.Cause == Out::Rejoin);
    CHECK(Judge(On(), Near(true), in, 5000 + On().RejoinMs).In);
}

void TheReasonEndsAfterTheMinimumStay()
{
    Memory memory;
    Judge(On(), Near(true), memory, 1000);
    // the quest is done: the member stays MinStayMs (a hand-in, the next quest taken), then leaves
    Verdict const early = Judge(On(), Near(false), memory, 1000 + On().MinStayMs - 1);
    CHECK(early.In && early.Reason == Why::Quest);
    Verdict const late = Judge(On(), Near(false), memory, 1000 + On().MinStayMs);
    CHECK(!late.In && late.Changed && late.Cause == Out::NoReason);
}

void ErrandsTakeAMemberOut()
{
    Memory memory;
    Judge(On(), Near(true), memory, 1000);
    Facts errand = Near(true);
    errand.OnErrand = true;
    Verdict const verdict = Judge(On(), errand, memory, 2000);
    CHECK(!verdict.In && verdict.Cause == Out::Errand);
    errand.OnErrand = false;
    errand.NeedsErrand = true;
    CHECK(Judge(On(), errand, memory, 2000 + On().RejoinMs).Cause == Out::Errand);   // still needs it: stays out
    CHECK(Judge(On(), Near(true), memory, 2000 + On().RejoinMs).In);                 // done: rejoins
}

void AnErrandBreakAfterErrandEvery()
{
    Settings const settings = On();
    Memory memory;
    Judge(settings, Near(true), memory, 0);
    CHECK(Judge(settings, Near(true), memory, settings.ErrandEveryMs - 1).In);
    Verdict const pause = Judge(settings, Near(true), memory, settings.ErrandEveryMs);
    CHECK(!pause.In && pause.Cause == Out::ErrandBreak);
    CHECK(!Judge(settings, Near(true), memory, settings.ErrandEveryMs + settings.ErrandBreakMs - 1).In);
    CHECK(Judge(settings, Near(true), memory, settings.ErrandEveryMs + settings.ErrandBreakMs).In);

    Settings never = On();
    never.ErrandEveryMs = 0;
    Memory always;
    Judge(never, Near(true), always, 0);
    CHECK(Judge(never, Near(true), always, 10ull * 60 * 60 * 1000).In);
}

void AFightFreezesTheVerdict()
{
    Memory memory;
    Judge(On(), Near(true), memory, 0);
    Facts fight = Near(false);
    fight.Frozen = true;
    CHECK(Judge(On(), fight, memory, 10 * On().MinStayMs).In);   // no reason, but it is fighting: it stays
    Memory out;
    Facts freeze = Near(true);
    freeze.Frozen = true;
    CHECK(!Judge(On(), freeze, out, 0).In);                      // and nobody joins mid-fight
}

void ABusyLeaderOrAnotherPartyKeepsItOut()
{
    Memory memory;
    Facts busy = Near(true);
    busy.LeaderBusy = true;
    CHECK(Judge(On(), busy, memory, 0).Cause == Out::LeaderBusy);
    CHECK(Judge(On(), Near(true), memory, 1).In);   // no rejoin wait for a leader's errand: the member was never in
    Facts other = Near(true);
    other.OtherParty = true;
    CHECK(Judge(On(), other, memory, 2).Cause == Out::OtherParty);
}

void TheLeaderBreaks()
{
    Settings const settings = On();
    Memory memory;
    CHECK(!LeaderOnBreak(settings, true, memory, 0));
    CHECK(!LeaderOnBreak(settings, true, memory, settings.ErrandEveryMs - 1));
    CHECK(LeaderOnBreak(settings, true, memory, settings.ErrandEveryMs));
    CHECK(LeaderOnBreak(settings, false, memory, settings.ErrandEveryMs + settings.ErrandBreakMs - 1));
    CHECK(!LeaderOnBreak(settings, false, memory, settings.ErrandEveryMs + settings.ErrandBreakMs));
    // a leader that has had no members starts its count again when it leads
    CHECK(!LeaderOnBreak(settings, true, memory, 3 * settings.ErrandEveryMs));
    CHECK(!LeaderOnBreak(settings, true, memory, 4 * settings.ErrandEveryMs - 1));
    CHECK(LeaderOnBreak(settings, true, memory, 4 * settings.ErrandEveryMs));
}

void ReasonOrder()
{
    Facts facts;
    CHECK(ReasonOf(facts) == Why::None);
    facts.GroupQuest = true;
    CHECK(ReasonOf(facts) == Why::GroupQuest);
    facts.SharedQuest = true;
    CHECK(ReasonOf(facts) == Why::Quest);
    facts.DungeonDue = true;
    CHECK(ReasonOf(facts) == Why::Dungeon);
    CHECK(std::string_view(WhyName(Why::GroupQuest)) == "group_quest" && std::string_view(OutName(Out::OtherMap)) == "other_map");
}

} // namespace

int main()
{
    NoReasonNoParty();
    ASharedQuestNearbyJoins();
    TooFarToJoinButADungeonGathersFromTheWholeMap();
    NeverAcrossMaps();
    TheReasonEndsAfterTheMinimumStay();
    ErrandsTakeAMemberOut();
    AnErrandBreakAfterErrandEvery();
    AFightFreezesTheVerdict();
    ABusyLeaderOrAnotherPartyKeepsItOut();
    TheLeaderBreaks();
    ReasonOrder();
    if (failures == 0)
        std::printf("party_reason_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
