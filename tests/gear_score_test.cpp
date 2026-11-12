// SPDX-License-Identifier: GPL-2.0-or-later
#include "gear_score.h"

#include <cstdio>
#include <cstdlib>
#include <string_view>

using namespace gear_score;

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failed; } } while (0)

static Item Make(std::uint32_t level, std::uint32_t type = 7, std::uint8_t quality = 2)
{
    Item i;
    i.Level = level;
    i.InventoryType = type;
    i.Quality = quality;
    i.ArmorSubclass = 2;
    return i;
}

int main()
{
    CHECK(ArchetypeOf(1, 0) == Archetype::Tank && ArchetypeOf(1, 3) == Archetype::MeleeStrength && ArchetypeOf(5, 0) == Archetype::Healer);
    CHECK(ArchetypeOf(5, 3) == Archetype::Caster && ArchetypeOf(3, 0) == Archetype::Hunter && ArchetypeOf(4, 0) == Archetype::MeleeAgility);
    CHECK(ArchetypeOf(11, 1) == Archetype::Tank && ArchetypeOf(2, 2) == Archetype::Healer);

    Weights const caster = WeightsOf(Archetype::Caster);
    Weights const rogue = WeightsOf(Archetype::MeleeAgility);
    Item intItem = Make(40);
    intItem.Stats[kIntellect] = 20;
    Item agiItem = Make(40);
    agiItem.Stats[kAgility] = 20;
    CHECK(Score(caster, intItem) == 400 + 60);
    CHECK(Score(caster, agiItem) == 400 - 60);
    CHECK(Score(rogue, agiItem) > Score(rogue, intItem));

    // Worn vs candidate by score: a lower item level with the right stats beats a higher one with the wrong stats, a tie keeps the worn item.
    Item wrongHigh = Make(42);
    wrongHigh.Stats[kAgility] = 20;                 // 420 - 60 = 360 for a caster
    Item rightLow = Make(40);
    rightLow.Stats[kIntellect] = 20;                // 460
    CHECK(!KeepWorn(caster, wrongHigh, rightLow));
    CHECK(KeepWorn(caster, rightLow, wrongHigh));
    CHECK(KeepWorn(caster, rightLow, rightLow));
    // Nothing depends on an item id: Item has none, so an item added later with a higher level just scores higher.
    Item newer = Make(80);
    CHECK(!KeepWorn(caster, rightLow, newer));

    // Selling bag gear.
    Item worn = rightLow;                           // score 460, level 40
    Item junk = Make(30);
    junk.Stats[kAgility] = 10;                      // 300 - 30 = 270
    auto judge = [&](Item const& bag, std::uint32_t botLevel = 40) { return JudgeBagGear(caster, botLevel, bag, &worn, 1, 1); };
    CHECK(judge(junk) == SellVerdict::Sell);
    Item q = junk; q.QuestItem = true;
    CHECK(judge(q) == SellVerdict::KeepQuestItem);
    Item set = junk; set.SetItem = true;
    CHECK(judge(set) == SellVerdict::KeepSetItem);
    Item blue = junk; blue.Quality = 3;
    CHECK(judge(blue) == SellVerdict::KeepQuality);
    Item unusable = junk; unusable.Usable = false;
    CHECK(judge(unusable) == SellVerdict::KeepNotUsable);
    Item notGear = junk; notGear.InventoryType = 0;
    CHECK(judge(notGear) == SellVerdict::KeepNotEquipment);
    Item later = junk; later.RequiredLevel = 45;
    CHECK(judge(later, 40) == SellVerdict::KeepLevelLater && judge(later, 45) == SellVerdict::Sell);
    Item otherType = junk; otherType.InventoryType = 8;
    CHECK(judge(otherType) == SellVerdict::KeepOtherSlotType);
    Item mail = junk; mail.ArmorSubclass = 3;
    CHECK(judge(mail) == SellVerdict::KeepHeavierArmor);
    CHECK(JudgeBagGear(caster, 40, junk, nullptr, 0, 1) == SellVerdict::KeepNothingWorn);
    // Higher item level than worn: could be an upgrade for the core's pass, never sold, even with bad stats.
    Item higher = Make(41);
    higher.Stats[kAgility] = 20;
    CHECK(judge(higher) == SellVerdict::KeepCouldBeBetter);
    // Inside the margin: kept.
    Item close = Make(38);
    close.Stats[kIntellect] = 18;                   // 380 + 54 = 434, worn 460: gap 26 < 30
    CHECK(judge(close) == SellVerdict::KeepCouldBeBetter);
    // Two ring slots: sold only when both worn rings outclass it.
    Item rings[2] = { rightLow, junk };
    Item weakRing = Make(30);
    weakRing.Stats[kAgility] = 10;
    CHECK(JudgeBagGear(caster, 40, weakRing, rings, 2, 2) == SellVerdict::KeepCouldBeBetter);   // the second ring is as weak as it
    rings[1] = rightLow;
    CHECK(JudgeBagGear(caster, 40, weakRing, rings, 2, 2) == SellVerdict::Sell);
    CHECK(JudgeBagGear(caster, 40, weakRing, rings, 1, 2) == SellVerdict::KeepNothingWorn);   // one ring slot empty
    CHECK(std::string_view(SellVerdictName(SellVerdict::Sell)) == "sell");

    if (g_failed == 0)
        std::printf("gear_score: all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
