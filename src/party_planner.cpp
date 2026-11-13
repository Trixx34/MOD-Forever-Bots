#include "party_planner.h"

#include "ground_avoid.h"

#include <algorithm>
#include <cmath>
#include <utility>
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
    // Tank swap: the main tank holds the swap aura at its stack count, another tank with fewer stacks takes over.
    order.TankSwapped = false;
    if (keep)
        for (Member const& m : members)
            if (m.Guid == order.MainTank && m.SwapAt != 0 && m.SwapStacks >= m.SwapAt)
            {
                Member const* next = nullptr;
                for (Member const& other : members)
                    if (other.Alive && other.Tank && other.Guid != m.Guid && other.SwapStacks < other.SwapAt &&
                        (!next || other.SwapStacks < next->SwapStacks || (other.SwapStacks == next->SwapStacks && other.Guid < next->Guid)))
                        next = &other;
                if (next)
                {
                    order.MainTank = next->Guid;
                    order.TankSwapped = true;
                }
                break;
            }
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
    // The rule table's interrupt-first casts take the nearest kickers first.
    std::sort(casters.begin(), casters.end(), [](Enemy const* a, Enemy const* b)
    { return std::make_pair(a->InterruptFirst ? 0 : 1, a->Ref) < std::make_pair(b->InterruptFirst ? 0 : 1, b->Ref); });
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

    // Heals: the members who need one, the emergencies first (lowest share), then the tank in combat, then the lowest share; each goes to the
    // nearest free healer in range. A healer holds one member, so two healers never pick the same one in the same tick.
    order.Heals.clear();
    std::vector<Member const*> needy;
    for (Member const& m : members)
    {
        bool const tanking = (m.Tank || m.Leader) && m.InCombat;
        if (m.Alive && (m.HealPct < settings.EmergencyPct || m.HealPct < (tanking ? settings.TankHealBelowPct : settings.HealBelowPct)))
            needy.push_back(&m);
    }
    auto const urgency = [&settings](Member const* m)
    {
        int const tier = m->HealPct < settings.EmergencyPct ? 0 : ((m->Tank || m->Leader) && m->InCombat && m->HealPct < settings.TankHealBelowPct) ? 1 : 2;
        return std::make_tuple(tier, m->HealPct, m->Guid);
    };
    std::sort(needy.begin(), needy.end(), [&urgency](Member const* a, Member const* b) { return urgency(a) < urgency(b); });
    std::unordered_set<std::uint64_t> busyHealers;
    for (Member const* patient : needy)
    {
        Member const* best = nullptr;
        float bestD = 0.0f;
        for (Member const& h : members)
        {
            if (!h.Alive || !h.CanHeal || busyHealers.count(h.Guid))
                continue;
            float const d = std::hypot(h.X - patient->X, h.Y - patient->Y);
            if (d > settings.HealRangeYards)
                continue;
            if (!best || d < bestD || (d == bestD && h.Guid < best->Guid))
            {
                best = &h;
                bestD = d;
            }
        }
        if (!best)
            continue;
        busyHealers.insert(best->Guid);
        order.Heals.push_back({ best->Guid, patient->Guid });
    }

    // Dispels: each member with a debuff the rule table wants dispelled (the lowest guid first, its first such debuff) goes to the nearest ready member that can dispel its
    // kind and is not on an emergency heal. A dispeller holds one member.
    order.Dispels.clear();
    std::unordered_set<std::uint64_t> emergencyHealers;
    for (HealAssignment const& heal : order.Heals)
        for (Member const& m : members)
            if (m.Guid == heal.Target && m.HealPct < settings.EmergencyPct)
                emergencyHealers.insert(heal.Bot);
    std::vector<Member const*> afflicted;
    for (Member const& m : members)
        if (m.Alive && !m.Debuffs.empty())
            afflicted.push_back(&m);
    std::sort(afflicted.begin(), afflicted.end(), [](Member const* a, Member const* b) { return a->Guid < b->Guid; });
    std::unordered_set<std::uint64_t> dispellers;
    for (Member const* patient : afflicted)
        for (Member::Debuff const& debuff : patient->Debuffs)
        {
            Member const* best = nullptr;
            float bestD = 0.0f;
            for (Member const& d : members)
            {
                if (!d.Alive || !d.DispelReady || debuff.Kind >= 32 || (d.DispelKinds & (1u << debuff.Kind)) == 0 || dispellers.count(d.Guid) || emergencyHealers.count(d.Guid))
                    continue;
                float const dist = std::hypot(d.X - patient->X, d.Y - patient->Y);
                if (dist > settings.HealRangeYards)
                    continue;
                if (!best || dist < bestD || (dist == bestD && d.Guid < best->Guid))
                {
                    best = &d;
                    bestD = dist;
                }
            }
            if (!best)
                continue;
            dispellers.insert(best->Guid);
            order.Dispels.push_back({ best->Guid, patient->Guid, debuff.Spell, debuff.Kind });
            break;      // one job per afflicted member per plan
        }

    // Purge: one enemy aura of a purge row at a time (the lowest unit reference): the nearest ready purger in reach, then the next nearest as backup. A member
    // that already has a dispel job, or is on an emergency heal, is left out.
    order.Purges.clear();
    {
        Enemy const* target = nullptr;
        for (Enemy const& e : enemies)
            if (e.PurgeAura != 0 && e.Health != 0 && (!target || e.Ref < target->Ref))
                target = &e;
        std::unordered_map<std::uint64_t, std::uint64_t> purgeSeen;
        if (target)
        {
            auto const at = order.PurgeSeenMs.find(target->Ref);
            purgeSeen[target->Ref] = at != order.PurgeSeenMs.end() ? at->second : nowMs;
            std::unordered_set<std::uint64_t> taken;
            for (std::uint8_t rank = 0; rank < 2; ++rank)
            {
                Member const* best = nullptr;
                float bestD = 0.0f;
                for (Member const& m : members)
                {
                    if (!m.Alive || m.PurgeSpell == 0 || !m.DispelReady || dispellers.count(m.Guid) || emergencyHealers.count(m.Guid) || taken.count(m.Guid))
                        continue;
                    float const d = Dist(m, *target);
                    if (d > settings.PurgeRangeYards)
                        continue;
                    if (!best || d < bestD || (d == bestD && m.Guid < best->Guid))
                    {
                        best = &m;
                        bestD = d;
                    }
                }
                if (!best)
                    break;
                taken.insert(best->Guid);
                order.Purges.push_back({ best->Guid, target->Ref, target->PurgeAura, best->PurgeSpell, rank });
            }
        }
        order.PurgeSeenMs = std::move(purgeSeen);
    }

    // Mind control: a member that carries a mindcontrol aura is listed (nobody attacks it) and held by the nearest ready member with a hold spell (rank 0), the next
    // nearest as backup (rank 1). A caster holds one controlled member.
    order.Controlled.clear();
    order.Control.clear();
    {
        std::vector<Member const*> controlled;
        for (Member const& m : members)
            if (m.Alive && m.Controlled)
                controlled.push_back(&m);
        std::sort(controlled.begin(), controlled.end(), [](Member const* a, Member const* b) { return a->Guid < b->Guid; });
        std::unordered_map<std::uint64_t, std::uint64_t> controlSeen;
        std::unordered_set<std::uint64_t> taken;
        for (Member const* victim : controlled)
        {
            order.Controlled.push_back(victim->Guid);
            auto const at = order.ControlSeenMs.find(victim->Guid);
            controlSeen[victim->Guid] = at != order.ControlSeenMs.end() ? at->second : nowMs;
            for (std::uint8_t rank = 0; rank < 2; ++rank)
            {
                Member const* best = nullptr;
                float bestD = 0.0f;
                for (Member const& m : members)
                {
                    if (!m.Alive || m.Controlled || m.ControlCcSpell == 0 || !m.ControlReady || taken.count(m.Guid))
                        continue;
                    float const d = std::hypot(m.X - victim->X, m.Y - victim->Y);
                    if (d > settings.PurgeRangeYards)
                        continue;
                    if (!best || d < bestD || (d == bestD && m.Guid < best->Guid))
                    {
                        best = &m;
                        bestD = d;
                    }
                }
                if (!best)
                    break;
                taken.insert(best->Guid);
                order.Control.push_back({ best->Guid, victim->Guid, best->ControlCcSpell, rank });
            }
        }
        order.ControlSeenMs = std::move(controlSeen);
    }

    // Buffs: for each party buff spell some member offers, each living member gets it from a living member that offers it and is within reach, the one with the fewest members so far, then the nearest (a member's own cast on itself is not planned).
    order.Buffs.clear();
    std::vector<std::uint32_t> buffSpells;
    for (Member const& m : members)
        if (m.Alive)
            for (std::uint32_t spell : m.BuffSpells)
                if (std::find(buffSpells.begin(), buffSpells.end(), spell) == buffSpells.end())
                    buffSpells.push_back(spell);
    std::sort(buffSpells.begin(), buffSpells.end());
    for (std::uint32_t spell : buffSpells)
    {
        std::unordered_map<std::uint64_t, int> load;    // members already given to a caster, so the casts spread over the casters and go out in the same tick
        for (Member const& recipient : members)
        {
            if (!recipient.Alive)
                continue;
            Member const* best = nullptr;
            float bestD = 0.0f;
            for (Member const& caster : members)
            {
                if (!caster.Alive || caster.Guid == recipient.Guid || std::find(caster.BuffSpells.begin(), caster.BuffSpells.end(), spell) == caster.BuffSpells.end())
                    continue;
                float const d = std::hypot(caster.X - recipient.X, caster.Y - recipient.Y);
                if (d > settings.BuffRangeYards)
                    continue;
                if (!best || std::make_tuple(load[caster.Guid], d, caster.Guid) < std::make_tuple(load[best->Guid], bestD, best->Guid))
                {
                    best = &caster;
                    bestD = d;
                }
            }
            if (best)
            {
                ++load[best->Guid];
                order.Buffs.push_back({ best->Guid, recipient.Guid, spell });
            }
        }
    }

    order.NoAoe = false;
    for (Enemy const& e : enemies)
        if (e.NoAoe && e.Health != 0)
            order.NoAoe = true;

    // Spread: for each rule an engaged enemy puts in force, the members of the rule's groups are settled one by one (a member that cannot move first, then by guid); one
    // that stands closer than the rule's yards to a settled member is given the nearest point that is clear of all of them, within SpreadMaxMoveYards of where it is.
    order.Spread.clear();
    {
        std::vector<SpreadRule> rules;
        for (Enemy const& e : enemies)
            if (e.Health != 0)
                for (SpreadRule const& rule : e.Spread)
                {
                    bool known = false;
                    for (SpreadRule const& have : rules)
                        known = known || (have.Yards == rule.Yards && have.Who == rule.Who);
                    if (!known)
                        rules.push_back(rule);
                }
        std::unordered_set<std::uint64_t> nudged;
        for (SpreadRule const& rule : rules)
        {
            std::vector<Member const*> group;
            for (Member const& m : members)
                if (m.Alive && (m.SpreadKind & rule.Who) != 0)
                    group.push_back(&m);
            std::sort(group.begin(), group.end(), [](Member const* a, Member const* b)
            { return std::make_tuple(a->Movable ? 1 : 0, a->Guid) < std::make_tuple(b->Movable ? 1 : 0, b->Guid); });
            std::vector<ground_avoid::Circle> settled;
            for (Member const* m : group)
            {
                float x = m->X, y = m->Y;
                if (m->Movable && !nudged.count(m->Guid) && ground_avoid::InsideAny(x, y, settled))
                {
                    if (auto const out = ground_avoid::EscapePoint(x, y, settled);
                        out && std::hypot(out->X - x, out->Y - y) <= settings.SpreadMaxMoveYards)
                    {
                        order.Spread.push_back({ m->Guid, out->X, out->Y });
                        nudged.insert(m->Guid);
                        x = out->X;
                        y = out->Y;
                    }
                }
                settled.push_back({ x, y, std::max(0.0f, rule.Yards - 1.0f) });   // inside = closer than radius + 1 yard
            }
        }
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

bool HealAssignedElsewhere(Order const* order, std::uint64_t bot, std::uint64_t member)
{
    if (!order)
        return false;
    bool other = false;
    for (HealAssignment const& a : order->Heals)
    {
        if (a.Target != member)
            continue;
        if (a.Bot == bot)
            return false;
        other = true;
    }
    return other;
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

bool BuffAssignedElsewhere(Order const* order, std::uint64_t bot, std::uint32_t spell, std::uint64_t recipient)
{
    if (!order)
        return false;
    for (BuffAssignment const& a : order->Buffs)
        if (a.Spell == spell && a.Recipient == recipient)
            return a.Bot != bot;
    return false;
}

DispelAssignment const* DispelFor(Order const* order, std::uint64_t bot)
{
    if (!order)
        return nullptr;
    for (DispelAssignment const& a : order->Dispels)
        if (a.Bot == bot)
            return &a;
    return nullptr;
}

bool NoAoeNow(Order const* order)
{
    return order && order->NoAoe;
}

SpreadAssignment const* SpreadFor(Order const* order, std::uint64_t bot)
{
    if (order)
        for (SpreadAssignment const& a : order->Spread)
            if (a.Bot == bot)
                return &a;
    return nullptr;
}

PurgeAssignment const* PurgeFor(Settings const& settings, Order const* order, std::uint64_t bot, std::uint64_t nowMs)
{
    if (!order)
        return nullptr;
    for (PurgeAssignment const& a : order->Purges)
    {
        if (a.Bot != bot)
            continue;
        if (a.Rank == 0)
            return &a;
        auto const seen = order->PurgeSeenMs.find(a.Target);
        return seen != order->PurgeSeenMs.end() && nowMs >= seen->second + settings.BackupDelayMs ? &a : nullptr;
    }
    return nullptr;
}

bool IsControlled(Order const* order, std::uint64_t member)
{
    return order && std::find(order->Controlled.begin(), order->Controlled.end(), member) != order->Controlled.end();
}

ControlAssignment const* ControlFor(Settings const& settings, Order const* order, std::uint64_t bot, std::uint64_t nowMs)
{
    if (!order)
        return nullptr;
    for (ControlAssignment const& a : order->Control)
    {
        if (a.Bot != bot)
            continue;
        if (a.Rank == 0)
            return &a;
        auto const seen = order->ControlSeenMs.find(a.Target);
        return seen != order->ControlSeenMs.end() && nowMs >= seen->second + settings.BackupDelayMs ? &a : nullptr;
    }
    return nullptr;
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
