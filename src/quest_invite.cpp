// SPDX-License-Identifier: GPL-2.0-or-later
#include "quest_invite.h"

namespace quest_invite {

bool Memory::ShouldInvite(Settings const& settings, Facts const& facts, std::uint64_t human, std::uint64_t nowMs) const
{
    if (!settings.Enabled || !facts.Idle || facts.GroupFull || !facts.SameObjective || facts.Distance > settings.RangeYards)
        return false;
    if (auto const it = _declined.find(human); it != _declined.end() && nowMs - it->second < settings.DeclineMs)
        return false;
    if (auto const it = _invited.find(human); it != _invited.end() && nowMs - it->second < settings.RepeatMs)
        return false;
    return true;
}

}  // namespace quest_invite
