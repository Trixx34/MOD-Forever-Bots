// SPDX-License-Identifier: GPL-2.0-or-later
// Quest group invites (config QuestInvite, off by default): a bot and a human player on the same quest objective, the human within 30 yards, and the bot
// invites the human to its group through the existing group action. At most once per human per 30 minutes, and never to a human who declined in the last
// 2 hours. Pure logic, tests/quest_invite_test.cpp.
#pragma once

#include <cstdint>
#include <map>

namespace quest_invite {

struct Settings
{
    bool Enabled = false;                 // QuestInvite
    float RangeYards = 30.0f;
    std::uint64_t RepeatMs = 30ull * 60 * 1000;
    std::uint64_t DeclineMs = 2ull * 60 * 60 * 1000;
};

struct Facts
{
    bool Idle = false;                    // out of combat, no target, no hostile in view
    bool BotInGroup = false;              // a bot that is in a full group does not invite; a bot alone or leading a party may
    bool GroupFull = false;
    bool SameObjective = false;           // the bot and the human are on the same objective of the same quest, neither done with it
    float Distance = 1e9f;
};

class Memory
{
public:
    bool ShouldInvite(Settings const& settings, Facts const& facts, std::uint64_t human, std::uint64_t nowMs) const;
    void NoteInvited(std::uint64_t human, std::uint64_t nowMs) { _invited[human] = nowMs ? nowMs : 1; }
    void NoteDeclined(std::uint64_t human, std::uint64_t nowMs) { _declined[human] = nowMs ? nowMs : 1; }

private:
    std::map<std::uint64_t, std::uint64_t> _invited, _declined;
};

}  // namespace quest_invite
