// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the party planner (party_planner.h). No core needed.
#include "party_planner.h"

#include <cmath>
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

void InterruptFirstCastTakesTheNearestKicker()
{
    Settings s = On();
    Order o;
    // two casters, one kicker
    Enemy plain = Caster(10, 0);
    Enemy listed = Caster(20, 40);
    listed.InterruptFirst = true;
    Plan(s, { Kicker(1, 1) }, { plain, listed }, 1000, o);
    CHECK(o.Interrupts.size() == 1);                       // one kicker, one job: the listed cast gets it although its ref is higher
    CHECK(o.Interrupts[0].Target == 20 && o.Interrupts[0].Rank == 0);
    CHECK(IsAssignedInterrupt(&o, 1, 20));
    CHECK(!IsAssignedInterrupt(&o, 1, 10));
}

Member Dispeller(std::uint64_t guid, float x, std::uint32_t kinds)
{
    Member m;
    m.Guid = guid;
    m.X = x;
    m.DispelKinds = kinds;
    return m;
}

Member Afflicted(std::uint64_t guid, float x, std::uint32_t spell, std::uint8_t kind)
{
    Member m;
    m.Guid = guid;
    m.X = x;
    m.Debuffs.push_back({ spell, kind });
    return m;
}

void DispelsGoToTheNearestCapableMember()
{
    Settings s = On();
    Order o;
    // a mage (curse, bit 2) far, a druid (curse + poison) near; the cursed member is at x=0
    Member mage = Dispeller(1, 25, 1u << 2);
    Member druid = Dispeller(2, 5, (1u << 2) | (1u << 4));
    Member cursed = Afflicted(3, 0, 16098, 2);
    Plan(s, { mage, druid, cursed }, {}, 1000, o);
    CHECK(o.Dispels.size() == 1);
    CHECK(o.Dispels[0].Bot == 2 && o.Dispels[0].Target == 3 && o.Dispels[0].Aura == 16098 && o.Dispels[0].Kind == 2);
    CHECK(DispelFor(&o, 2) != nullptr && DispelFor(&o, 1) == nullptr);
    CHECK(DispelFor(nullptr, 2) == nullptr);
}

void DispelNeedsTheKindARangeAndNoEmergency()
{
    Settings s = On();
    Order o;
    Member poisoned = Afflicted(3, 0, 5, 4);
    // a mage cannot take a poison
    Plan(s, { Dispeller(1, 2, 1u << 2), poisoned }, {}, 1000, o);
    CHECK(o.Dispels.empty());
    // too far
    Plan(s, { Dispeller(1, 80, 1u << 4), poisoned }, {}, 1500, o);
    CHECK(o.Dispels.empty());
    // not ready
    Member slow = Dispeller(1, 2, 1u << 4);
    slow.DispelReady = false;
    Plan(s, { slow, poisoned }, {}, 2000, o);
    CHECK(o.Dispels.empty());
    // a healer on an emergency heal keeps healing: a member at 10 % is next to it
    Member healer = Dispeller(1, 2, 1u << 4);
    healer.CanHeal = true;
    Member dying;
    dying.Guid = 4;
    dying.X = 3;
    dying.HealPct = 10;
    Plan(s, { healer, poisoned, dying }, {}, 2500, o);
    CHECK(o.Heals.size() == 1 && o.Heals[0].Bot == 1 && o.Heals[0].Target == 4);
    CHECK(o.Dispels.empty());
    // one dispeller, two afflicted members: the lower guid first, the other waits
    Plan(s, { Dispeller(1, 2, 1u << 4), Afflicted(3, 0, 5, 4), Afflicted(5, 1, 5, 4) }, {}, 3000, o);
    CHECK(o.Dispels.size() == 1 && o.Dispels[0].Target == 3);
}

Member Tank(std::uint64_t guid, std::uint32_t stacks, std::uint32_t at)
{
    Member m;
    m.Guid = guid;
    m.Tank = true;
    m.SwapStacks = stacks;
    m.SwapAt = at;
    return m;
}

void TankSwapOnTheDebuff()
{
    Settings s = On();
    Order o;
    Plan(s, { Tank(1, 0, 3), Tank(2, 0, 3) }, {}, 1000, o);
    CHECK(o.MainTank == 1 && !o.TankSwapped);
    Plan(s, { Tank(1, 2, 3), Tank(2, 0, 3) }, {}, 1500, o);
    CHECK(o.MainTank == 1 && !o.TankSwapped);               // below the threshold
    Plan(s, { Tank(1, 3, 3), Tank(2, 0, 3) }, {}, 2000, o);
    CHECK(o.MainTank == 2 && o.TankSwapped);                // at the threshold: handed over
    Plan(s, { Tank(1, 1, 3), Tank(2, 1, 3) }, {}, 2500, o);
    CHECK(o.MainTank == 2 && !o.TankSwapped);               // the new tank stays until it has the stacks itself
    Plan(s, { Tank(1, 0, 3), Tank(2, 3, 3) }, {}, 3000, o);
    CHECK(o.MainTank == 1 && o.TankSwapped);
    // no second tank: nobody to hand it to
    Order solo;
    Plan(s, { Tank(1, 5, 3) }, {}, 1000, solo);
    CHECK(solo.MainTank == 1 && !solo.TankSwapped);
    // no rule (SwapAt 0): never swaps
    Order plain;
    Plan(s, { Tank(1, 9, 0), Tank(2, 0, 0) }, {}, 1000, plain);
    Plan(s, { Tank(1, 9, 0), Tank(2, 0, 0) }, {}, 2000, plain);
    CHECK(plain.MainTank == 1 && !plain.TankSwapped);
}

void NoAoeFlag()
{
    Settings s = On();
    Order o;
    Enemy e = Caster(10, 0);
    e.Health = 100;
    e.NoAoe = true;
    Plan(s, { Kicker(1, 0) }, { e }, 1000, o);
    CHECK(NoAoeNow(&o));
    Plan(s, { Kicker(1, 0) }, { Caster(10, 0) }, 1500, o);
    CHECK(!NoAoeNow(&o));
    CHECK(!NoAoeNow(nullptr));
}

Member Buffer(std::uint64_t guid, float x, std::vector<std::uint32_t> spells)
{
    Member m;
    m.Guid = guid;
    m.X = x;
    m.BuffSpells = spells;
    return m;
}

void BuffsGoToTheNearestOffer()
{
    Settings s = On();
    Order o;
    // two priests (Fortitude 1243) at x=0 and x=100, a warrior at x=98 that offers nothing, a mage at 1
    Plan(s, { Buffer(1, 0, { 1243 }), Buffer(2, 100, { 1243 }), Buffer(3, 98, {}), Buffer(4, 1, {}) }, {}, 1000, o);
    CHECK(!BuffAssignedElsewhere(&o, 1, 1243, 4));          // priest 1 buffs the mage
    CHECK(BuffAssignedElsewhere(&o, 2, 1243, 4));           // priest 2 leaves it
    CHECK(!BuffAssignedElsewhere(&o, 2, 1243, 3));          // priest 2 buffs the warrior next to it
    CHECK(BuffAssignedElsewhere(&o, 1, 1243, 3));           // priest 1 is out of reach of the warrior: it is priest 2's
    CHECK(!BuffAssignedElsewhere(nullptr, 1, 1243, 4));
    CHECK(!BuffAssignedElsewhere(&o, 1, 5555, 4));          // a spell nobody offers: not covered
}

void BuffsSpreadOverTheCasters()
{
    Settings s = On();
    Order o;
    Plan(s, { Buffer(1, 0, { 1126 }), Buffer(2, 0, { 1126 }), Buffer(3, 0, { 1126 }), Buffer(4, 0, { 1126 }), Buffer(5, 0, { 1126 }) }, {}, 1000, o);
    CHECK(o.Buffs.size() == 5);
    std::unordered_map<std::uint64_t, int> perCaster;
    for (BuffAssignment const& a : o.Buffs)
        ++perCaster[a.Bot];
    for (auto const& entry : perCaster)
        CHECK(entry.second <= 2);                           // five members over five casters: nobody carries them all
    CHECK(perCaster.size() >= 3);
}

void BuffsIgnoreTheDeadAndTheFar()
{
    Settings s = On();
    Order o;
    Member dead = Buffer(1, 0, { 1243 });
    dead.Alive = false;
    Plan(s, { dead, Buffer(2, 5, { 1243 }), Buffer(3, 0, {}) }, {}, 1000, o);
    CHECK(BuffAssignedElsewhere(&o, 1, 1243, 3));           // the dead caster is not the one
    CHECK(!BuffAssignedElsewhere(&o, 2, 1243, 3));
    Order far;
    Plan(s, { Buffer(1, 0, { 1243 }), Buffer(3, 500, {}) }, {}, 1000, far);
    CHECK(!BuffAssignedElsewhere(&far, 2, 1243, 3));        // nobody in reach: not covered, anyone may
    CHECK(far.Buffs.empty());
}

Member Spreadable(std::uint64_t guid, float x, float y, std::uint8_t kind)
{
    Member m;
    m.Guid = guid;
    m.X = x;
    m.Y = y;
    m.SpreadKind = kind;
    return m;
}

Enemy SpreadBoss(float yards, std::uint8_t who)
{
    Enemy e;
    e.Ref = 900;
    e.Health = 1000;
    e.Engaged = true;
    e.Spread.push_back({ yards, who });
    return e;
}

float Apart(Member const& a, SpreadAssignment const& n)
{
    return std::hypot(a.X - n.X, a.Y - n.Y);
}

void SpreadMovesTheHigherGuidAwayFromTheLower()
{
    Settings s = On();
    Order o;
    std::vector<Member> members{ Spreadable(1, 0, 0, 2), Spreadable(2, 3, 0, 2), Spreadable(3, 50, 0, 2) };
    Plan(s, members, { SpreadBoss(10, 2) }, 1000, o);
    CHECK(o.Spread.size() == 1);
    SpreadAssignment const* nudge = SpreadFor(&o, 2);
    CHECK(nudge != nullptr && SpreadFor(&o, 1) == nullptr && SpreadFor(&o, 3) == nullptr);
    if (nudge)
    {
        CHECK(std::hypot(nudge->X - 0, nudge->Y - 0) >= 10.0f);     // at least `yards` from the member that stays
        CHECK(Apart(members[1], *nudge) <= s.SpreadMaxMoveYards);
    }
}

void SpreadClumpEndsUpClear()
{
    Settings s = On();
    Order o;
    std::vector<Member> members;
    for (std::uint64_t g = 1; g <= 6; ++g)
        members.push_back(Spreadable(g, float(g) * 0.5f, 0, 2));       // six ranged within 3 yards
    Plan(s, members, { SpreadBoss(8, 2) }, 1000, o);
    std::vector<std::pair<float, float>> at;
    for (Member const& m : members)
    {
        SpreadAssignment const* n = SpreadFor(&o, m.Guid);
        at.push_back(n ? std::make_pair(n->X, n->Y) : std::make_pair(m.X, m.Y));
    }
    for (std::size_t i = 0; i < at.size(); ++i)
        for (std::size_t j = i + 1; j < at.size(); ++j)
            CHECK(std::hypot(at[i].first - at[j].first, at[i].second - at[j].second) >= 8.0f - 0.01f);
    CHECK(SpreadFor(&o, 1) == nullptr);                                 // the lowest guid stays
}

void SpreadOnlyTheNamedGroupsAndNeverTanksOrDeadOrWithoutAnEnemy()
{
    Settings s = On();
    std::vector<Member> members{ Spreadable(1, 0, 0, 2), Spreadable(2, 2, 0, 1), Spreadable(3, 1, 1, 0), Spreadable(4, 1, 0, 2) };
    members[3].Alive = false;
    Order healersOnly;
    Plan(s, members, { SpreadBoss(10, 4) }, 1000, healersOnly);
    CHECK(healersOnly.Spread.empty());                                  // nobody is a healer
    Order ranged;
    Plan(s, members, { SpreadBoss(10, 2) }, 1000, ranged);
    CHECK(ranged.Spread.empty());                                       // one ranged alive, the melee and the tank (kind 0) are not in the group
    Order none;
    Plan(s, members, {}, 1000, none);
    CHECK(none.Spread.empty());                                         // no enemy puts a rule in force
    Enemy dead = SpreadBoss(10, 7);
    dead.Health = 0;
    Order gone;
    Plan(s, { Spreadable(1, 0, 0, 2), Spreadable(2, 1, 0, 2) }, { dead }, 1000, gone);
    CHECK(gone.Spread.empty());
}

void SpreadKeepsAHumanInPlaceAndMovesTheBotAroundIt()
{
    Settings s = On();
    Order o;
    Member human = Spreadable(9, 0, 0, 7);
    human.Movable = false;                                              // a higher guid, but it cannot be moved: it is settled first
    Plan(s, { Spreadable(1, 2, 0, 2), human }, { SpreadBoss(10, 7) }, 1000, o);
    CHECK(SpreadFor(&o, 9) == nullptr && SpreadFor(&o, 1) != nullptr);
}

void SpreadIsRecomputedEachPlanAndGoneWhenClear()
{
    Settings s = On();
    Order o;
    std::vector<Member> members{ Spreadable(1, 0, 0, 2), Spreadable(2, 3, 0, 2) };
    Plan(s, members, { SpreadBoss(10, 2) }, 1000, o);
    CHECK(o.Spread.size() == 1);
    members[1].X = 30;
    Plan(s, members, { SpreadBoss(10, 2) }, 2000, o);
    CHECK(o.Spread.empty());
}

Member Purger(std::uint64_t guid, float x, std::uint32_t spell)
{
    Member m;
    m.Guid = guid;
    m.X = x;
    m.PurgeSpell = spell;
    return m;
}

Enemy Buffed(std::uint64_t ref, float x, std::uint32_t aura)
{
    Enemy e;
    e.Ref = ref;
    e.X = x;
    e.Health = 1000;
    e.PurgeAura = aura;
    return e;
}

void PurgeGoesToTheNearestWithABackup()
{
    Settings s = On();
    Order o;
    Plan(s, { Purger(1, 20, 527), Purger(2, 5, 370), Purger(3, 8, 527), Member{} }, { Buffed(77, 0, 23895) }, 1000, o);
    CHECK(o.Purges.size() == 2);
    CHECK(o.Purges[0].Bot == 2 && o.Purges[0].Rank == 0 && o.Purges[0].Spell == 370 && o.Purges[0].Target == 77 && o.Purges[0].Aura == 23895);
    CHECK(o.Purges[1].Bot == 3 && o.Purges[1].Rank == 1);
    CHECK(PurgeFor(s, &o, 2, 1000) != nullptr);
    CHECK(PurgeFor(s, &o, 3, 1000) == nullptr);                          // the backup waits
    CHECK(PurgeFor(s, &o, 3, 1000 + s.BackupDelayMs) != nullptr);
    CHECK(PurgeFor(s, &o, 4, 1000) == nullptr && PurgeFor(s, nullptr, 2, 1000) == nullptr);
}

void PurgeNeedsAnAuraReachAndAFreeCaster()
{
    Settings s = On();
    Order none;
    Plan(s, { Purger(1, 5, 527) }, { Buffed(77, 0, 0) }, 1000, none);
    CHECK(none.Purges.empty());                                          // no listed aura on the enemy
    Order far;
    Plan(s, { Purger(1, 100, 527) }, { Buffed(77, 0, 23895) }, 1000, far);
    CHECK(far.Purges.empty());                                           // out of reach
    Order busy;
    Member dispeller = Purger(1, 5, 527);
    dispeller.DispelKinds = 1u << 1;
    Member patient;
    patient.Guid = 2;
    patient.Debuffs.push_back({ 23952, 1 });
    Plan(s, { dispeller, patient, Purger(3, 6, 370) }, { Buffed(77, 0, 23895) }, 1000, busy);
    CHECK(DispelFor(&busy, 1) != nullptr);                               // the nearer priest cleans the member
    CHECK(busy.Purges.size() == 1 && busy.Purges[0].Bot == 3);           // the shaman takes the enemy buff
    Order notReady;
    Member cooling = Purger(1, 5, 527);
    cooling.DispelReady = false;
    Plan(s, { cooling }, { Buffed(77, 0, 23895) }, 1000, notReady);
    CHECK(notReady.Purges.empty());
}

void PurgeKeepsOnlyOneEnemyAndTheSeenTime()
{
    Settings s = On();
    Order o;
    Plan(s, { Purger(1, 5, 527), Purger(2, 6, 527) }, { Buffed(90, 1, 23895), Buffed(70, 2, 23895) }, 1000, o);
    CHECK(!o.Purges.empty() && o.Purges[0].Target == 70);
    Plan(s, { Purger(1, 5, 527), Purger(2, 6, 527) }, { Buffed(70, 2, 23895) }, 2000, o);
    CHECK(o.PurgeSeenMs[70] == 1000);                                    // kept across plans
    Plan(s, { Purger(1, 5, 527) }, {}, 3000, o);
    CHECK(o.Purges.empty() && o.PurgeSeenMs.empty());
}

Member Sheeper(std::uint64_t guid, float x)
{
    Member m;
    m.Guid = guid;
    m.X = x;
    m.ControlCcSpell = 118;
    return m;
}

Member Charmed(std::uint64_t guid, float x)
{
    Member m;
    m.Guid = guid;
    m.X = x;
    m.Controlled = true;
    return m;
}

void MindControlIsHeldByTheNearestCasterWithABackup()
{
    Settings s = On();
    Order o;
    Plan(s, { Charmed(5, 0), Sheeper(1, 25), Sheeper(2, 10), Sheeper(3, 100) }, {}, 1000, o);
    CHECK(IsControlled(&o, 5) && !IsControlled(&o, 1) && !IsControlled(nullptr, 5));
    CHECK(o.Control.size() == 2);                                        // the third is out of reach
    CHECK(o.Control[0].Bot == 2 && o.Control[0].Target == 5 && o.Control[0].Rank == 0 && o.Control[0].Spell == 118);
    CHECK(o.Control[1].Bot == 1 && o.Control[1].Rank == 1);
    CHECK(ControlFor(s, &o, 2, 1000) != nullptr && ControlFor(s, &o, 1, 1000) == nullptr);
    CHECK(ControlFor(s, &o, 1, 1000 + s.BackupDelayMs) != nullptr && ControlFor(s, &o, 3, 5000) == nullptr);
}

void MindControlCasterRulesAndRelease()
{
    Settings s = On();
    Order o;
    Member cooling = Sheeper(1, 5);
    cooling.ControlReady = false;
    Member alsoCharmed = Sheeper(2, 6);
    alsoCharmed.Controlled = true;                                       // a controlled caster does not cast at its fellow
    Plan(s, { Charmed(5, 0), cooling, alsoCharmed }, {}, 1000, o);
    CHECK(o.Controlled.size() == 2 && o.Control.empty());                // both carry the aura; nobody can hold them
    Member dead = Charmed(5, 0);
    dead.Alive = false;
    Plan(s, { dead, Sheeper(1, 5) }, {}, 2000, o);
    CHECK(o.Controlled.empty() && o.Control.empty());                    // a dead member is not held
    Plan(s, { Sheeper(1, 5), Sheeper(2, 6) }, {}, 3000, o);
    CHECK(o.Controlled.empty() && o.ControlSeenMs.empty());              // the aura ended
}

void NoRulesNothingChanges()
{
    Settings s = On();
    Order o;
    Plan(s, { Spreadable(1, 0, 0, 2), Spreadable(2, 1, 0, 2), Sheeper(3, 0), Purger(4, 0, 527) }, { Buffed(1, 0, 0) }, 1000, o);
    CHECK(o.Spread.empty() && o.Purges.empty() && o.Controlled.empty() && o.Control.empty());
}

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
    InterruptFirstCastTakesTheNearestKicker();
    DispelsGoToTheNearestCapableMember();
    DispelNeedsTheKindARangeAndNoEmergency();
    TankSwapOnTheDebuff();
    NoAoeFlag();
    BuffsGoToTheNearestOffer();
    BuffsSpreadOverTheCasters();
    BuffsIgnoreTheDeadAndTheFar();
    SpreadMovesTheHigherGuidAwayFromTheLower();
    SpreadClumpEndsUpClear();
    SpreadOnlyTheNamedGroupsAndNeverTanksOrDeadOrWithoutAnEnemy();
    SpreadKeepsAHumanInPlaceAndMovesTheBotAroundIt();
    SpreadIsRecomputedEachPlanAndGoneWhenClear();
    PurgeGoesToTheNearestWithABackup();
    PurgeNeedsAnAuraReachAndAFreeCaster();
    PurgeKeepsOnlyOneEnemyAndTheSeenTime();
    MindControlIsHeldByTheNearestCasterWithABackup();
    MindControlCasterRulesAndRelease();
    NoRulesNothingChanges();
    if (failures)
    {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("party_planner_test: all passed\n");
    return 0;
}
