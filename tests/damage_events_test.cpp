// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests of the damage event bookkeeping (damage_events.h): the ring of 16, the killing blow, the died: fields, the aggregate lists. No core needed.
#include "damage_events.h"

#include <cstdio>

using namespace damage_events;

namespace
{

int failures = 0;
#define CHECK(cond)                                                       \
    do                                                                    \
    {                                                                     \
        if (!(cond))                                                      \
        {                                                                 \
            ++failures;                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                 \
    } while (0)

Hit Make(std::uint64_t victim, std::uint32_t entry, std::uint32_t spell, std::uint32_t amount, bool killing = false, std::uint64_t atMs = 1000)
{
    Hit h;
    h.AtMs = atMs;
    h.Victim = victim;
    h.Entry = entry;
    h.Spell = spell;
    h.Amount = amount;
    h.Killing = killing;
    h.MaxHealth = 1000;
    h.HealthAfter = killing ? 0 : 500;
    h.Overkill = killing ? 40 : 0;
    return h;
}

void RingKeepsTheLastSixteenNewestFirst()
{
    HitLog log;
    for (std::uint32_t i = 1; i <= 20; ++i)
        log.Add(Make(7, 100, 10 + i, i));
    std::vector<Hit> const all = log.Recent(7);
    CHECK(all.size() == kRingSize);
    CHECK(all.front().Amount == 20 && all.back().Amount == 5);
    CHECK(log.Recent(7, 3).size() == 3 && log.Recent(7, 3)[2].Amount == 18);
    CHECK(log.Recent(8).empty());
    log.Add(Make(8, 1, 1, 1));
    CHECK(log.Victims() == 2 && log.Recent(7).size() == kRingSize); // rings are per bot
    log.Forget(7);
    CHECK(log.Recent(7).empty() && log.Victims() == 1);
}

void VictimsAreBounded()
{
    HitLog log;
    for (std::uint64_t v = 1; v <= kMaxVictims + 10; ++v)
        log.Add(Make(v, 1, 1, 1));
    CHECK(log.Victims() == kMaxVictims);
    CHECK(log.Recent(kMaxVictims + 5).empty());
    log.Add(Make(1, 1, 2, 2)); // a known bot still gets its hits
    CHECK(log.Recent(1).size() == 2);
}

void KillingBlowIsFreshAndTakenOnce()
{
    HitLog log;
    log.Add(Make(7, 100, 5, 300, false, 1000));
    CHECK(!log.TakeKillingBlow(7, 2000).has_value()); // no killing blow among the hits
    log.Add(Make(7, 200, 6, 900, true, 2000));
    log.Add(Make(7, 100, 5, 100, false, 2100));       // a later non-lethal hit does not hide the blow
    std::optional<Hit> const blow = log.TakeKillingBlow(7, 3000);
    CHECK(blow && blow->Entry == 200 && blow->Spell == 6);
    CHECK(!log.TakeKillingBlow(7, 3000).has_value()); // taken: a second death does not reuse it
    log.Add(Make(7, 300, 9, 900, true, 4000));
    CHECK(log.TakeKillingBlow(7, 5000)->Entry == 300);
    log.Add(Make(7, 400, 9, 900, true, 6000));
    CHECK(!log.TakeKillingBlow(7, 6000 + kBlowFreshMs + 1).has_value()); // too old
    CHECK(!log.TakeKillingBlow(99, 1).has_value());
}

void HpPctAndMinPct()
{
    Hit big = Make(1, 1, 1, 300);
    big.HealthAfter = 200; // 500 before of 1000
    CHECK(HpPctBefore(big) == 50);
    Hit kill = Make(1, 1, 1, 900, true);
    kill.Overkill = 400;   // 500 before
    CHECK(HpPctBefore(kill) == 50);
    kill.MaxHealth = 0;
    CHECK(HpPctBefore(kill) == 0);
    Hit small = Make(1, 1, 1, 150);
    CHECK(!PassesMinPct(small, 20) && PassesMinPct(small, 15) && PassesMinPct(Make(1, 1, 1, 200), 20));
    CHECK(PassesMinPct(Make(1, 1, 1, 1, true), 100)); // a killing blow always passes
    Hit unknown = small;
    unknown.MaxHealth = 0;
    CHECK(PassesMinPct(unknown, 100));
}

void DiedFieldsFormat()
{
    HitLog log;
    log.Add(Make(7, 100, 0, 150));
    log.Add(Make(7, 200, 20566, 960, true));
    std::optional<Hit> const blow = log.TakeKillingBlow(7, 1500);
    CHECK(DiedFields(blow, log.Recent(7)) == " killer_spell=20566 killer_src=event overkill=40 hit_amount=960 hit_hp_pct=92 last_hits=20566:960,0:150");
    CHECK(DiedFields(std::nullopt, {}) == " killer_spell=none killer_src=guess overkill=none hit_amount=none hit_hp_pct=none last_hits=none");
    HitLog many;
    for (std::uint32_t i = 1; i <= 9; ++i)
        many.Add(Make(1, 1, i, i * 10));
    CHECK(FormatLastHits(many.Recent(1)) == "9:90,8:80,7:70,6:60,5:50");
}

void AggregateListsAreSortedAndBounded()
{
    Aggregate agg;
    CHECK(agg.Empty() && agg.Format().empty());
    agg.Add(Make(1, 100, 0, 400));
    agg.Add(Make(1, 100, 0, 500));
    agg.Add(Make(1, 200, 20566, 3000));
    agg.Add(Make(2, 200, 20566, 1500, true)); // overkill 40
    agg.Add(Make(2, 300, 7, 3000));
    CHECK(agg.Format() == " dmg_by_spell=20566:4500:2:1,7:3000:1:0,0:900:2:0 dmg_by_attacker=200:4500:2:1,300:3000:1:0,100:900:2:0 death_by_spell=20566:1:40 death_by_attacker=200:1:40");
    agg.Clear();
    CHECK(agg.Empty());
    // no death: no death_by_ fields; at most kTopN entries; ties keep the lower key first; keys are capped
    Aggregate wide;
    for (std::uint32_t s = 1; s <= 8; ++s)
        wide.Add(Make(1, 50, s, 100));
    CHECK(wide.Format() == " dmg_by_spell=1:100:1:0,2:100:1:0,3:100:1:0,4:100:1:0,5:100:1:0 dmg_by_attacker=50:800:8:0");
    Aggregate cap;
    for (std::uint32_t s = 1; s <= kMaxKeys + 5; ++s)
        cap.Add(Make(1, 50, s, 10));
    CHECK(cap.BySpell().size() == kMaxKeys && cap.Dropped() == 5);
}

} // namespace

int main()
{
    RingKeepsTheLastSixteenNewestFirst();
    VictimsAreBounded();
    KillingBlowIsFreshAndTakenOnce();
    HpPctAndMinPct();
    DiedFieldsFormat();
    AggregateListsAreSortedAndBounded();
    if (failures == 0)
        std::printf("damage_events_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
