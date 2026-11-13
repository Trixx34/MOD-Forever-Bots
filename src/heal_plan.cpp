// Planned healing: see heal_plan.h.
#include "heal_plan.h"

#include <algorithm>

namespace heal_plan {

bool Update(Settings const& settings, std::vector<party_planner::Member> const& members, Plan& plan)
{
    std::vector<std::uint64_t> healers;
    for (party_planner::Member const& m : members)
        if (m.Alive && m.CanHeal)
            healers.push_back(m.Guid);
    std::sort(healers.begin(), healers.end());

    Plan next;
    if (settings.Enabled && !healers.empty())
    {
        next.Healers = healers;
        bool const sameHealers = healers == plan.Healers;
        next.MainHealer = std::find(healers.begin(), healers.end(), plan.MainHealer) != healers.end() ? plan.MainHealer : healers.front();

        std::vector<party_planner::Member const*> sorted;
        for (party_planner::Member const& m : members)
            sorted.push_back(&m);
        std::sort(sorted.begin(), sorted.end(), [](auto const* a, auto const* b) { return a->Guid < b->Guid; });

        std::unordered_map<std::uint64_t, std::uint32_t> load;
        for (std::uint64_t h : healers)
            load[h] = 0;
        // Tanks: a member playing the tank, or the leader (who tanks in a dungeon run). They go to the main healer.
        for (auto const* m : sorted)
            if ((m->Tank || m->Leader) && m->Alive)
            {
                next.Tanks.push_back(m->Guid);
                next.HealerOf[m->Guid] = next.MainHealer;
                load[next.MainHealer] += 2;
            }
        // The rest: kept while the healer lives and the healer set is unchanged, otherwise the least loaded healer (not itself when another is there).
        std::vector<party_planner::Member const*> open;
        for (auto const* m : sorted)
        {
            if (next.HealerOf.count(m->Guid))
                continue;
            auto const old = plan.HealerOf.find(m->Guid);
            if (sameHealers && old != plan.HealerOf.end() && load.count(old->second))
            {
                next.HealerOf[m->Guid] = old->second;
                ++load[old->second];
            }
            else
                open.push_back(m);
        }
        for (auto const* m : open)
        {
            std::uint64_t best = 0;
            bool have = false;
            for (std::uint64_t h : healers)
            {
                if (h == m->Guid && healers.size() > 1)
                    continue;
                if (!have || load[h] < load[best])
                {
                    best = h;
                    have = true;
                }
            }
            next.HealerOf[m->Guid] = best;
            ++load[best];
        }
    }

    next.Signature = next.Healers.empty() ? std::string() : Describe(next);   // no healer: no plan, and no event for the nothing that was there before
    bool const changed = next.Signature != plan.Signature;
    plan = std::move(next);
    return changed;
}

std::string Describe(Plan const& plan)
{
    if (plan.Healers.empty())
        return "healers=0";
    std::string out = "healers=" + std::to_string(plan.Healers.size()) + " main=" + std::to_string(plan.MainHealer);
    for (std::uint64_t h : plan.Healers)
    {
        std::vector<std::uint64_t> mine;
        for (auto const& pair : plan.HealerOf)
            if (pair.second == h)
                mine.push_back(pair.first);
        std::sort(mine.begin(), mine.end());
        out += " " + std::to_string(h) + ":[";
        for (std::size_t i = 0; i < mine.size() && i < 10; ++i)
            out += (i ? "," : "") + std::to_string(mine[i]);
        if (mine.size() > 10)
            out += ",+" + std::to_string(mine.size() - 10);
        out += "]";
    }
    return out;
}

bool Covers(Plan const* plan, std::uint64_t healer)
{
    if (!plan)
        return false;
    for (auto const& pair : plan->HealerOf)
        if (pair.second == healer)
            return true;
    return false;
}

bool IsAssigned(Plan const* plan, std::uint64_t healer, std::uint64_t member)
{
    if (!plan)
        return false;
    auto const at = plan->HealerOf.find(member);
    return at != plan->HealerOf.end() && at->second == healer;
}

Verdict Judge(Plan const* plan, std::uint64_t healer, std::uint64_t member, bool assignedNeed, bool ownerReady, std::uint32_t pct, std::uint32_t emergencyPct)
{
    if (!plan || !Covers(plan, healer) || IsAssigned(plan, healer, member) || pct < emergencyPct || plan->HealerOf.find(member) == plan->HealerOf.end())
        return Verdict::Own;
    return assignedNeed || ownerReady ? Verdict::Skip : Verdict::Switch;
}

std::uint32_t TankThreshold(Settings const& settings, Plan const* plan, std::uint64_t healer, std::uint64_t tank, std::uint32_t base, std::uint64_t nowMs, std::uint64_t fightStartMs)
{
    if (!settings.Enabled || settings.PreHealMs == 0 || !plan || fightStartMs == 0 || nowMs < fightStartMs || nowMs - fightStartMs > settings.PreHealMs)
        return base;
    if (healer != plan->MainHealer || !IsAssigned(plan, healer, tank))
        return base;
    return std::max(base, settings.PreHealBelowPct);
}

}   // namespace heal_plan
