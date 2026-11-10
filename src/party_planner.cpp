#include "party_planner.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <unordered_set>

namespace party_planner {

namespace {

float Dist(Member const& m, Enemy const& e)
{
    return std::hypot(m.X - e.X, m.Y - e.Y);
}

} // namespace

bool Plan(Settings const& settings, std::vector<Member> const& members, std::vector<Enemy> const& enemies, std::uint64_t nowMs, Order& order)
{
    if (!settings.Enabled)
        return false;
    if (order.LastPlannedMs != 0 && nowMs < order.LastPlannedMs + settings.PlanIntervalMs)
        return false;
    order.LastPlannedMs = nowMs;

    // Main tank: kept while it is alive and still a member, so the identity does not flap with small changes.
    bool keep = false;
    for (Member const& m : members)
        if (m.Guid == order.MainTank && m.Alive && m.Tank)
            keep = true;
    if (!keep)
    {
        order.MainTank = 0;
        for (Member const& m : members)
            if (m.Alive && m.Tank)
            {
                order.MainTank = m.Guid;
                break;
            }
    }

    // When each casting target was first seen casting; forget the ones no longer casting.
    std::unordered_map<std::uint64_t, std::uint64_t> seen;
    for (Enemy const& e : enemies)
        if (e.Casting)
        {
            auto const at = order.CastSeenMs.find(e.Ref);
            seen[e.Ref] = at != order.CastSeenMs.end() ? at->second : nowMs;
        }
    order.CastSeenMs = std::move(seen);

    // Interrupts: for each casting enemy, the nearest ready capable member, then the next nearest as backup. A member holds one job.
    order.Interrupts.clear();
    std::unordered_set<std::uint64_t> busy;
    std::vector<Enemy const*> casters;
    for (Enemy const& e : enemies)
        if (e.Casting)
            casters.push_back(&e);
    std::sort(casters.begin(), casters.end(), [](Enemy const* a, Enemy const* b) { return a->Ref < b->Ref; });
    for (std::uint8_t rank = 0; rank < 2; ++rank)
        for (Enemy const* caster : casters)
        {
            Member const* best = nullptr;
            for (Member const& m : members)
            {
                if (!m.Alive || !m.CanInterrupt || !m.InterruptReady || busy.count(m.Guid))
                    continue;
                if (!best || Dist(m, *caster) < Dist(*best, *caster) || (Dist(m, *caster) == Dist(*best, *caster) && m.Guid < best->Guid))
                    best = &m;
            }
            if (!best)
                continue;
            busy.insert(best->Guid);
            order.Interrupts.push_back({ best->Guid, caster->Ref, rank });
        }

    // Crowd control: each ready caster takes the nearest eligible unit nobody else holds. A unit that casts is the interrupters' job, a
    // protected or engaged one is never held.
    order.CrowdControl.clear();
    std::unordered_set<std::uint64_t> held;
    std::size_t holding = 0;
    for (Member const& m : members)
        if (m.Alive && m.Holding != 0)
        {
            held.insert(m.Holding);     // a unit already held is not given to a second caster
            ++holding;
        }
    std::vector<Member const*> casterMembers;
    for (Member const& m : members)
        if (m.Alive && m.CcSpell != 0 && m.CcReady && m.Holding == 0)
            casterMembers.push_back(&m);
    std::sort(casterMembers.begin(), casterMembers.end(), [](Member const* a, Member const* b) { return a->Guid < b->Guid; });
    for (Member const* m : casterMembers)
    {
        if (holding + order.CrowdControl.size() >= settings.MaxCcAssignments)
            break;
        Enemy const* best = nullptr;
        for (Enemy const& e : enemies)
        {
            if (e.Casting || e.Engaged || e.Protected || held.count(e.Ref) ||
                std::find(e.CcSpells.begin(), e.CcSpells.end(), m->CcSpell) == e.CcSpells.end())
                continue;
            if (!best || Dist(*m, e) < Dist(*m, *best) || (Dist(*m, e) == Dist(*m, *best) && e.Ref < best->Ref))
                best = &e;
        }
        if (!best)
            continue;
        held.insert(best->Ref);
        order.CrowdControl.push_back({ m->Guid, best->Ref });
    }

    // Kill target: of the pack's units that nobody holds, the best kill rank, then a caster, then the weakest (lowest ref on a tie).
    order.KillTarget = 0;
    Enemy const* kill = nullptr;
    auto const keyOf = [](Enemy const& e) { return std::make_tuple(e.KillRank, e.Casting ? 0 : 1, e.Health, e.Ref); };
    for (Enemy const& e : enemies)
    {
        if (!e.InPack || e.Health == 0 || held.count(e.Ref))
            continue;
        if (!kill || keyOf(e) < keyOf(*kill))
            kill = &e;
    }
    if (kill)
        order.KillTarget = kill->Ref;
    return true;
}

std::uint64_t KillTargetOf(Order const* order)
{
    return order ? order->KillTarget : 0;
}

bool IsAssignedInterrupt(Order const* order, std::uint64_t bot, std::uint64_t target)
{
    if (!order)
        return false;
    for (InterruptAssignment const& a : order->Interrupts)
        if (a.Bot == bot && a.Target == target)
            return true;
    return false;
}

bool MayInterrupt(Settings const& settings, Order const* order, std::uint64_t bot, std::uint64_t target, std::uint64_t nowMs)
{
    if (!order)
        return true;
    bool planned = false;
    for (InterruptAssignment const& a : order->Interrupts)
    {
        if (a.Target != target)
            continue;
        planned = true;
        if (a.Bot != bot)
            continue;
        if (a.Rank == 0)
            return true;
        auto const seen = order->CastSeenMs.find(target);
        return seen != order->CastSeenMs.end() && nowMs >= seen->second + settings.BackupDelayMs;
    }
    return !planned;    // a target the plan did not cover is left to the claim map
}

std::uint64_t CcTargetFor(Order const* order, std::uint64_t bot)
{
    if (!order)
        return 0;
    for (CcAssignment const& a : order->CrowdControl)
        if (a.Bot == bot)
            return a.Target;
    return 0;
}

} // namespace party_planner
