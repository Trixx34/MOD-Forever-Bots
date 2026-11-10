// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the party planner (party_planner.h). No core needed.
#include "party_planner.h"

#include <cstdio>

using namespace party_planner;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

Settings On()
{
    Settings s;
    s.Enabled = true;
    return s;
}

Member Kicker(std::uint64_t guid, float x)
{
    Member m;
    m.Guid = guid;
    m.CanInterrupt = true;
    m.X = x;
    return m;
}

Enemy Caster(std::uint64_t ref, float x)
{
    Enemy e;
    e.Ref = ref;
    e.X = x;
    e.Casting = true;
    return e;
}

void Disabled()
{
    Settings s;
    Order o;
    CHECK(!Plan(s, { Kicker(1, 0) }, { Caster(10, 5) }, 1000, o));
    CHECK(o.Interrupts.empty());
}

void Throttle()
{
    Settings s = On();
    Order o;
    CHECK(Plan(s, { Kicker(1, 0) }, { Caster(10, 5) }, 1000, o));
    CHECK(!Plan(s, { Kicker(1, 0) }, { Caster(10, 5) }, 1200, o));
    CHECK(Plan(s, { Kicker(1, 0) }, { Caster(10, 5) }, 1400, o));
}

void NearestKickerWinsAndBackupIsNamed()
{
    Settings s = On();
    Order o;
    // bot 1 (earlier in guid order) is far; bot 2 is near the caster
    Plan(s, { Kicker(1, 50), Kicker(2, 3) }, { Caster(10, 0) }, 1000, o);
    CHECK(o.Interrupts.size() == 2);
    CHECK(o.Interrupts[0].Bot == 2 && o.Interrupts[0].Rank == 0 && o.Interrupts[0].Target == 10);
    CHECK(o.Interrupts[1].Bot == 1 && o.Interrupts[1].Rank == 1);
    CHECK(MayInterrupt(s, &o, 2, 10, 1000));
    CHECK(!MayInterrupt(s, &o, 1, 10, 1000));          // the backup waits
    CHECK(MayInterrupt(s, &o, 1, 10, 1000 + s.BackupDelayMs));
    CHECK(IsAssignedInterrupt(&o, 2, 10) && IsAssignedInterrupt(&o, 1, 10));
}

void KickerOnCooldownIsSkippedAndOneJobEach()
{
    Settings s = On();
    Order o;
    Member far = Kicker(1, 50);
    Member near = Kicker(2, 1);
    near.InterruptReady = false;
    Plan(s, { far, near }, { Caster(10, 0), Caster(11, 2) }, 1000, o);
    for (InterruptAssignment const& a : o.Interrupts)
        CHECK(a.Bot == 1);
    CHECK(o.Interrupts.size() == 1);                     // one ready kicker holds one job
}

void UncoveredTargetFallsBackToTheClaimMap()
{
    Settings s = On();
    Order o;
    Plan(s, { Kicker(1, 0) }, { Caster(10, 5) }, 1000, o);
    CHECK(MayInterrupt(s, &o, 1, 99, 1000));             // not in the plan
    CHECK(MayInterrupt(s, nullptr, 7, 10, 1000));        // no plan at all
}

void MainTankIsKept()
{
    Settings s = On();
    Order o;
    Member t1;
    t1.Guid = 5;
    t1.Tank = true;
    Member t2;
    t2.Guid = 3;
    t2.Tank = true;
    Plan(s, { t2, t1 }, {}, 1000, o);
    CHECK(o.MainTank == 3);
    Plan(s, { t1, t2 }, {}, 2000, o);
    CHECK(o.MainTank == 3);                              // not re-elected
    t2.Alive = false;
    Plan(s, { t1, t2 }, {}, 3000, o);
    CHECK(o.MainTank == 5);
}

void CrowdControlDoesNotDoubleBook()
{
    Settings s = On();
    Order o;
    Member a;
    a.Guid = 1;
    a.CcSpell = 118;
    a.X = 0;
    Member b;
    b.Guid = 2;
    b.CcSpell = 118;
    b.X = 1;
    Enemy e1;
    e1.Ref = 20;
    e1.X = 2;
    e1.CcSpells = { 118 };
    Enemy e2;
    e2.Ref = 21;
    e2.X = 30;
    e2.CcSpells = { 118 };
    Enemy engaged;
    engaged.Ref = 22;
    engaged.X = 1;
    engaged.Engaged = true;
    engaged.CcSpells = { 118 };
    Enemy casting = Caster(23, 1);
    casting.CcSpells = { 118 };
    Plan(s, { a, b }, { e1, e2, engaged, casting }, 1000, o);
    CHECK(o.CrowdControl.size() == 2);
    CHECK(CcTargetFor(&o, 1) == 20);
    CHECK(CcTargetFor(&o, 2) == 21);                     // the nearer one is already held
    CHECK(CcTargetFor(&o, 99) == 0);
    CHECK(CcTargetFor(nullptr, 1) == 0);
}

void CrowdControlRespectsHeldUnitsAndSpellType()
{
    Settings s = On();
    Order o;
    Member a;
    a.Guid = 1;
    a.CcSpell = 118;
    Member b;
    b.Guid = 2;
    b.CcSpell = 118;
    b.Holding = 20;                                      // already holds unit 20
    Enemy held;
    held.Ref = 20;
    held.CcSpells = { 118 };
    Enemy fresh;
    fresh.Ref = 21;
    fresh.X = 5;
    fresh.CcSpells = { 118 };
    Enemy wrongType;
    wrongType.Ref = 22;
    wrongType.CcSpells = { 9999 };                       // a spell this member does not have
    Plan(s, { a, b }, { held, wrongType, fresh }, 1000, o);
    CHECK(o.CrowdControl.size() == 1);
    CHECK(CcTargetFor(&o, 1) == 21);
    CHECK(CcTargetFor(&o, 2) == 0);                      // b is busy holding
    // the limit counts what is already held
    Settings one = On();
    one.MaxCcAssignments = 1;
    Order p;
    Plan(one, { a, b }, { held, fresh }, 1000, p);
    CHECK(p.CrowdControl.empty());
}

void CastSeenIsForgotten()
{
    Settings s = On();
    Order o;
    Plan(s, { Kicker(1, 0) }, { Caster(10, 5) }, 1000, o);
    CHECK(o.CastSeenMs.count(10) == 1);
    Plan(s, { Kicker(1, 0) }, {}, 2000, o);
    CHECK(o.CastSeenMs.empty());
}

// Kill target: the pack's best kill rank first, then a caster, then the weakest; a held unit and a unit outside the pack are never picked.
void KillTargetOrder()
{
    Enemy weak;  weak.Ref = 10; weak.InPack = true; weak.Health = 50;
    Enemy strong; strong.Ref = 11; strong.InPack = true; strong.Health = 500;
    Enemy caster; caster.Ref = 12; caster.InPack = true; caster.Health = 400; caster.Casting = true;
    Enemy listed; listed.Ref = 13; listed.InPack = true; listed.Health = 900; listed.KillRank = 2;
    Enemy far;   far.Ref = 14; far.Health = 1;
    Order o;
    Plan(On(), {}, { weak, strong, far }, 1000, o);
    CHECK(o.KillTarget == 10);                       // the weakest of the pack, not the far unit
    Plan(On(), {}, { weak, strong, caster, far }, 2000, o);
    CHECK(o.KillTarget == 12);                       // a caster before the weakest
    Plan(On(), {}, { weak, strong, caster, listed }, 3000, o);
    CHECK(o.KillTarget == 13);                       // the dungeon's kill priority before all
    Member holder; holder.Guid = 1; holder.Holding = 13;
    Plan(On(), { holder }, { weak, strong, caster, listed }, 4000, o);
    CHECK(o.KillTarget == 12);                       // a held unit is left alone
    CHECK(KillTargetOf(&o) == 12 && KillTargetOf(nullptr) == 0);
    Plan(On(), {}, {}, 5000, o);
    CHECK(o.KillTarget == 0);
}

// Heals: the neediest member first (an emergency before the tank), each to the nearest free healer in range; one member per healer.
void HealsAreAssignedOneToEach()
{
    Member healerA; healerA.Guid = 1; healerA.CanHeal = true; healerA.X = 0;
    Member healerB; healerB.Guid = 2; healerB.CanHeal = true; healerB.X = 20;
    Member tank; tank.Guid = 3; tank.Tank = true; tank.InCombat = true; tank.HealPct = 50; tank.X = 1;
    Member hurt; hurt.Guid = 4; hurt.HealPct = 10; hurt.X = 21;
    Member fine; fine.Guid = 5; fine.HealPct = 95; fine.X = 2;
    Order o;
    Plan(On(), { healerA, healerB, tank, hurt, fine }, {}, 1000, o);
    CHECK(o.Heals.size() == 2);
    CHECK(o.Heals[0].Target == 4 && o.Heals[0].Bot == 2);   // the emergency goes to the nearest healer
    CHECK(o.Heals[1].Target == 3 && o.Heals[1].Bot == 1);   // then the tank to the one left
    CHECK(!HealAssignedElsewhere(&o, 1, 3));                // healer A holds the tank
    CHECK(HealAssignedElsewhere(&o, 1, 4));                 // and leaves the emergency to B
    CHECK(!HealAssignedElsewhere(&o, 1, 5));                // nobody needs the full-health member: no claim
    CHECK(!HealAssignedElsewhere(nullptr, 1, 4));
    // a healer out of range of the patient does not take it
    Member farHealer = healerA; farHealer.X = 200;
    Plan(On(), { farHealer, hurt }, {}, 2000, o);
    CHECK(o.Heals.empty());
}

} // namespace

int main()
{
    Disabled();
    Throttle();
    NearestKickerWinsAndBackupIsNamed();
    KickerOnCooldownIsSkippedAndOneJobEach();
    UncoveredTargetFallsBackToTheClaimMap();
    MainTankIsKept();
    CrowdControlDoesNotDoubleBook();
    CrowdControlRespectsHeldUnitsAndSpellType();
    CastSeenIsForgotten();
    KillTargetOrder();
    HealsAreAssignedOneToEach();
    if (failures)
    {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("party_planner_test: all passed\n");
    return 0;
}
