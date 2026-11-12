// SPDX-License-Identifier: GPL-2.0-or-later
// Group quests (config QuestGroups, off by default): who invites whom when a bot waits for a party for a quest with a 'group <n>' marker (quest_data.h).
// Pure logic, no core, unit-tested in tests/quest_group_test.cpp. The policy (bot_policy_features.cpp, DriveQuestGroups) builds the candidates from the
// snapshot, sends the invites and the accepts, registers the leaders so their party follows them, and disbands the party when the quest is done.
//
// A bot in the planner's WaitGroup step is a seeker (Waiting = the quest). Seekers on the same map are taken in guid order: the first one not yet taken
// into another seeker's party leads. It invites, until its party has the size the quest wants, the free bots in range that have the same quest in their
// log (first, nearest first), then, with GuildHelpers, its guild mates. The party walks the quest's areas behind its leader; the leader hands in.
#pragma once

#include <cstdint>
#include <vector>

namespace quest_group {

struct Settings
{
    bool Enabled = false;                       // QuestGroups
    std::uint64_t WaitMs = 600000;              // QuestGroupWaitMinutes: the wait for a party ends here (the quest is abandoned)
    float RangeYards = 150.0f;                  // QuestGroupRangeYards: a partner must be this near (same map)
    bool GuildHelpers = true;                   // QuestGroupGuildHelpers: guild mates without the quest join too
};

struct Candidate
{
    std::uint64_t Guid = 0;
    std::uint32_t Map = 0;
    float X = 0.0f, Y = 0.0f;
    std::uint32_t Level = 0;
    std::uint64_t Guild = 0;                    // 0 = none
    std::vector<std::uint32_t> Log;             // the quests in its log
    std::uint32_t Waiting = 0;                  // the group quest it waits for (WaitGroup step), else 0
    std::uint32_t WantSize = 0;                 // the party size that quest wants
    std::uint32_t PartySize = 0;                // its party's size now (0 or 1 = alone)
    bool Free = false;                          // alive, not fighting, not in a party, no invite pending, not on a route or in a dungeon: may be invited
    bool Leads = false;                         // already leads a party (of PartySize)
};

struct Invite
{
    std::uint64_t Leader = 0;
    std::uint64_t Invitee = 0;
    std::uint32_t Quest = 0;
};

// The invites to send now. A leader that already leads a party only invites for the places left (WantSize - PartySize). Invites that are still
// pending are the caller's to hold back (it invites a bot again only after a retry time).
std::vector<Invite> PlanInvites(Settings const& settings, std::vector<Candidate> const& candidates);

}  // namespace quest_group
