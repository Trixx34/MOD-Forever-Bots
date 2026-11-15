// SPDX-License-Identifier: GPL-2.0-or-later
#include "flex_roles.h"

#include <algorithm>
#include <tuple>

namespace flex_roles {

std::uint32_t TankRank(std::uint32_t classId, std::uint32_t level)
{
    switch (classId)
    {
        case kWarrior: return 1;
        case kPaladin: return level == 0 || level >= kMinFlexTankLevel ? 2u : 0u;
        case kDruid: return level == 0 || level >= kMinFlexTankLevel ? 3u : 0u;
        default: return 0;
    }
}

std::uint32_t HealerRank(std::uint32_t classId)
{
    switch (classId)
    {
        case kPriest: return 1;
        case kShaman: return 2;
        case kDruid: return 3;
        case kPaladin: return 4;
        default: return 0;
    }
}

std::size_t DungeonPullCap(std::uint32_t tankClassId, std::size_t warriorCap)
{
    std::size_t const cap = (tankClassId == kPaladin || tankClassId == kDruid) && warriorCap > 1 ? warriorCap - 1 : warriorCap;
    return std::max<std::size_t>(cap, 1);
}

Result Assign(std::vector<Member> const& party)
{
    Result out;
    bool warrior = false;
    for (Member const& m : party)
    {
        if (m.Explicit == 1)
            out.HasTank = true;
        else if (m.Explicit == 2)
            out.HasHealer = true;
        else if (m.Explicit == 0 && m.ClassId == kWarrior)
            warrior = true;
    }
    out.HasTank = out.HasTank || warrior;

    // The bots a pick may come from: no explicit role.
    std::vector<Member const*> free;
    for (Member const& m : party)
        if (m.Explicit == 0)
            free.push_back(&m);
    std::sort(free.begin(), free.end(), [](Member const* a, Member const* b) { return a->Guid < b->Guid; });

    std::string note;
    auto const describe = [](Member const& m) { return std::to_string(m.Guid) + "(class " + std::to_string(m.ClassId) + ")"; };

    if (out.HasTank)
    {
        // The tank is there: the healer is the best Auto bot that can heal (a warrior never heals).
        if (!out.HasHealer)
        {
            Member const* best = nullptr;
            for (Member const* m : free)
                if (std::uint32_t const rank = HealerRank(m->ClassId); rank != 0 && (!best || rank < HealerRank(best->ClassId)))
                    best = m;
            if (best)
            {
                out.Role[best->Guid] = 2;
                out.HasHealer = true;
                note = "tank=present healer=" + describe(*best);
            }
        }
    }
    else
    {
        // No tank yet: every (tank, healer) pair, the one that fills both roles first, then the best tank rank, then the best healer rank, then the guids.
        using Key = std::tuple<int, std::uint32_t, std::uint32_t, std::uint64_t, std::uint64_t>;
        bool found = false;
        Key bestKey;
        Member const* bestTank = nullptr;
        Member const* bestHealer = nullptr;
        for (Member const* t : free)
        {
            std::uint32_t const tr = TankRank(t->ClassId, t->Level);
            if (tr == 0)
                continue;
            Member const* healer = nullptr;
            if (!out.HasHealer)
                for (Member const* h : free)
                    if (h != t && HealerRank(h->ClassId) != 0 &&
                        (!healer || std::make_pair(HealerRank(h->ClassId), h->Guid) < std::make_pair(HealerRank(healer->ClassId), healer->Guid)))
                        healer = h;
            Key const key{ (healer || out.HasHealer) ? 0 : 1, tr, healer ? HealerRank(healer->ClassId) : 0u, t->Guid, healer ? healer->Guid : 0u };
            if (!found || key < bestKey)
            {
                found = true;
                bestKey = key;
                bestTank = t;
                bestHealer = healer;
            }
        }
        if (found)
        {
            out.Role[bestTank->Guid] = 1;
            out.HasTank = true;
            note = "tank=" + describe(*bestTank);
            if (bestHealer)
            {
                out.Role[bestHealer->Guid] = 2;
                out.HasHealer = true;
                note += " healer=" + describe(*bestHealer);
            }
            else if (!out.HasHealer)
            {
                // No second healer-capable bot. Say it when the tank pick took the only one.
                if (HealerRank(bestTank->ClassId) != 0)
                    note += " healer=none reason=tank_over_healer";
                else
                    note += " healer=none";
            }
        }
        else
        {
            // No tank candidate. A healer is still picked.
            Member const* best = nullptr;
            if (!out.HasHealer)
                for (Member const* m : free)
                    if (std::uint32_t const rank = HealerRank(m->ClassId); rank != 0 && (!best || rank < HealerRank(best->ClassId)))
                        best = m;
            note = "tank=none";
            if (best)
            {
                out.Role[best->Guid] = 2;
                out.HasHealer = true;
                note += " healer=" + describe(*best);
            }
            else if (!out.HasHealer)
                note += " healer=none";
        }
    }
    out.Reason = note.empty() ? std::string("tank=") + (out.HasTank ? "present" : "none") + " healer=" + (out.HasHealer ? "present" : "none") : note;
    return out;
}

} // namespace flex_roles
