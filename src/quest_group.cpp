// SPDX-License-Identifier: GPL-2.0-or-later
#include "quest_group.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace quest_group {

namespace
{
float Dist(Candidate const& a, Candidate const& b)
{
    return std::hypot(a.X - b.X, a.Y - b.Y);
}

bool HasQuest(Candidate const& c, std::uint32_t quest)
{
    return std::find(c.Log.begin(), c.Log.end(), quest) != c.Log.end();
}
}  // namespace

std::vector<Invite> PlanInvites(Settings const& settings, std::vector<Candidate> const& candidates)
{
    std::vector<Invite> invites;
    if (!settings.Enabled)
        return invites;
    std::vector<Candidate const*> seekers;
    for (Candidate const& c : candidates)
        if (c.Waiting != 0 && c.WantSize > 1)
            seekers.push_back(&c);
    std::sort(seekers.begin(), seekers.end(), [](Candidate const* a, Candidate const* b) { return a->Guid < b->Guid; });

    std::set<std::uint64_t> taken; // bots already placed in a party this round (leaders and invitees)
    for (Candidate const* leader : seekers)
    {
        if (taken.count(leader->Guid))
            continue;
        // A seeker that sits in somebody else's party (it is no leader) does not lead.
        if (leader->PartySize > 1 && !leader->Leads)
            continue;
        taken.insert(leader->Guid);
        std::uint32_t const have = std::max<std::uint32_t>(1, leader->PartySize);
        if (have >= leader->WantSize)
            continue;
        std::uint32_t need = leader->WantSize - have;

        std::vector<Candidate const*> pool;
        for (Candidate const& c : candidates)
        {
            if (&c == leader || taken.count(c.Guid) || !c.Free || c.Map != leader->Map || Dist(c, *leader) > settings.RangeYards)
                continue;
            bool const sameQuest = HasQuest(c, leader->Waiting);
            bool const guildMate = settings.GuildHelpers && leader->Guild != 0 && c.Guild == leader->Guild;
            if (!sameQuest && !guildMate)
                continue;
            pool.push_back(&c);
        }
        // Same quest first, then the nearest, then the lowest guid (a fixed order, so every tick makes the same choice).
        std::sort(pool.begin(), pool.end(), [&](Candidate const* a, Candidate const* b)
        {
            bool const qa = HasQuest(*a, leader->Waiting), qb = HasQuest(*b, leader->Waiting);
            if (qa != qb)
                return qa;
            float const da = Dist(*a, *leader), db = Dist(*b, *leader);
            if (da != db)
                return da < db;
            return a->Guid < b->Guid;
        });
        for (Candidate const* c : pool)
        {
            if (need == 0)
                break;
            invites.push_back({ leader->Guid, c->Guid, leader->Waiting });
            taken.insert(c->Guid);
            --need;
        }
    }
    return invites;
}

}  // namespace quest_group
