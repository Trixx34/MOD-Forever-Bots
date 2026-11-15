// SPDX-License-Identifier: GPL-2.0-or-later
// Dynamic parties (config DynamicParties, on by default): a bot of a world party (PartySize, PartyPercent) is only in its party while there is a
// reason for it, and only on its leader's map. Pure logic, no core, unit-tested in tests/party_reason_test.cpp. The policy (bot_policy_parties.cpp,
// BotPolicy::ApplyPartyReasons) builds the facts from the snapshot and the bot state each tick and drops the members that are out from the
// tick's party maps, so the existing party code (TurnTrainAndParty) invites the ones that are in and makes the others leave.
//
// The reasons to be in the party: a quest the member and its leader both have and have not finished (shared objectives), a group quest
// ('group <n>') the leader has not finished, or a level dungeon the party is due for. A dungeon run under way, a tank-led route and a scenario
// (ZG, Onyxia, the MC/BWL routes) are assigned groups: the policy does not judge those at all.
// The member is out, whatever the reason, while it is on another map than its leader, too far from it, on a town errand (profession trainer,
// Auction House, vendor, repair, supplies, a flight), when it needs one (broken gear, short of supplies, bags full on a hand-in), and for an
// errand break: after ErrandEveryMs in the party it leaves for ErrandBreakMs so the errand and auction schedulers (which skip grouped bots) can
// take it. A member that left waits RejoinMs before it is invited again. Nothing changes while the member or its leader fights or is dead.
#pragma once

#include <cstdint>

namespace party_reason {

struct Settings
{
    bool Enabled = false;                       // DynamicParties (the policy default is off so the old tests keep the static parties; the config default is on)
    float JoinYards = 150.0f;                   // PartyJoinYards: a member is invited only this near its leader (same map)
    float KeepYards = 400.0f;                   // PartyKeepYards: a member farther than this from its leader leaves
    std::uint64_t RejoinMs = 60000;             // PartyRejoinSec: after leaving, no invite before this
    std::uint64_t MinStayMs = 60000;            // a member stays this long after joining even when the reason ends (quest logs churn on hand-ins)
    std::uint64_t ErrandEveryMs = 30 * 60000;   // PartyErrandEveryMinutes: in the party this long, a member takes an errand break (0 = never)
    std::uint64_t ErrandBreakMs = 5 * 60000;    // PartyErrandBreakMinutes: the break, longer while an errand it started is under way
};

enum class Why : std::uint8_t { None, Quest, GroupQuest, Dungeon };
enum class Out : std::uint8_t { None, NoReason, OtherMap, TooFar, Errand, ErrandBreak, Rejoin, LeaderBusy, OtherParty };

// One member against its party's leader, this tick.
struct Facts
{
    bool SameMap = true;
    float Distance = 0.0f;          // to the leader, in yards (same map only)
    bool SharedQuest = false;       // a quest both have in the log, neither has finished (status 1)
    bool GroupQuest = false;        // the leader has an unfinished 'group <n>' quest (n >= 2)
    bool DungeonDue = false;        // the party is due for a level dungeon
    bool OnErrand = false;          // the member is on a town errand, a vendor or repair walk, a craft walk, a flight
    bool NeedsErrand = false;       // the member needs one now (broken gear, supplies short, refused hand-in)
    bool LeaderBusy = false;        // the leader is on an errand, needs one, or takes its break: the party is off
    bool OtherParty = false;        // the member is in another bot party (a quest group): it is not taken from it
    bool Frozen = false;            // the member or the leader fights or is dead: the last verdict stands
};

// What the policy remembers of one member.
struct Memory
{
    bool In = false;
    Why LastWhy = Why::None;
    Out LastOut = Out::None;
    std::uint64_t SinceMs = 0;          // when it joined (In) or left
    std::uint64_t OutUntilMs = 0;       // no invite before this (RejoinMs, an errand break)
    std::uint64_t BreakDueMs = 0;       // the next errand break (set on joining)
};

struct Verdict
{
    bool In = false;
    Why Reason = Why::None;
    Out Cause = Out::None;
    bool Changed = false;               // In differs from the last tick's
};

// The reason the facts give, in this order: a dungeon, a shared quest, the leader's group quest.
Why ReasonOf(Facts const& facts);

// Judges one member and updates its memory.
Verdict Judge(Settings const& settings, Facts const& facts, Memory& memory, std::uint64_t nowMs);

// A leader's own break: after ErrandEveryMs leading members it is busy for ErrandBreakMs (memory.BreakDueMs and OutUntilMs are used).
// `leading` is whether it had a member in its party last tick. Answers whether the leader takes its break now.
bool LeaderOnBreak(Settings const& settings, bool leading, Memory& memory, std::uint64_t nowMs);

char const* WhyName(Why why);
char const* OutName(Out out);

}  // namespace party_reason
