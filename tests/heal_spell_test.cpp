// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the heal spell mix (src/heal_spell.h): which spell and rank a healer casts for a deficit. No core needed.
#include "heal_spell.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

using namespace heal_spell;

Input Base(std::uint32_t classId, std::uint32_t level, std::uint32_t health, std::uint32_t maxHealth)
{
    Input in;
    in.ClassId = classId;
    in.Level = level;
    in.Mana = 2000;
    in.MaxMana = 2000;
    in.Health = health;
    in.MaxHealth = maxHealth;
    return in;
}

void PriestPicksByDeficitAndUrgency()
{
    Choice c = Choose(Base(kPriest, 30, 400, 800));                 // safe, deficit 400: Heal rank 2 (406) covers it
    CHECK(c.Spell == 2055 && c.Exact);
    c = Choose(Base(kPriest, 30, 700, 800));                        // deficit 100: Heal rank 1 (292); not Flash Heal
    CHECK(c.Spell == 2054);
    Input urgent = Base(kPriest, 30, 150, 500);                     // 30 %, deficit 350: Flash Heal (from 20), the highest rank at 30 is 9472 (250; the next is level 32)
    c = Choose(urgent);
    CHECK(c.Spell == 9472 && std::string(c.Note) == "fast");
    Input incoming = Base(kPriest, 30, 400, 800);                   // not low, but 200 damage a second empties it in 2 s
    incoming.IncomingPerSec = 200;
    c = Choose(incoming);
    CHECK(c.Spell == 9472);                                         // a Flash Heal rank; none covers 400 at level 30, so the highest
    c = Choose(Base(kPriest, 50, 3000, 6000));                      // deficit 3000, safe, Greater Heal exists from 40: highest rank of the level (1100, 10963; 10964 is level 52)
    CHECK(c.Spell == 10963);
    c = Choose(Base(kPriest, 50, 5000, 6000));                      // deficit 1000: Greater Heal 845 does not cover, 1100 (10963) does
    CHECK(c.Spell == 10963);
    c = Choose(Base(kPriest, 50, 5800, 6000));                      // deficit 200: Heal covers it cheaper than Greater Heal
    CHECK(c.Spell == 2054);
    c = Choose(Base(kPriest, 10, 100, 400));                        // level 10: Lesser Heal only (Flash Heal is level 20)
    CHECK(c.Spell == 2053 || c.Spell == 2052);
    Input urgentLow = Base(kPriest, 10, 20, 400);
    CHECK(Choose(urgentLow).Spell == 2053);                         // no Flash Heal yet: the big heal
}

void OnlySpellsTheLevelHas()
{
    CHECK(Choose(Base(kPaladin, 12, 100, 400)).Spell == 639 || Choose(Base(kPaladin, 12, 100, 400)).Spell == 635);   // no Flash of Light before 20
    Input none = Base(kShaman, 10, 100, 400);
    none.HurtCount = 5;                                              // no Chain Heal before 40
    CHECK(std::string(Choose(none).Note) != "chain");
    CHECK(Choose(Base(kDruid, 8, 50, 400)).Spell != 8936);           // no Regrowth before 12
    Input noMana = Base(kPriest, 30, 100, 800);
    noMana.Mana = 100;                                              // can pay Lesser Heal-size only: Heal r1 costs 155
    CHECK(Choose(noMana).Spell != 2054);
}

void NoOverhealSpam()
{
    Choice c = Choose(Base(kPriest, 30, 790, 800));                 // 10 missing: far below a third of Heal rank 1
    CHECK(c.Skip && c.Spell == 0);
    c = Choose(Base(kPriest, 30, 800, 800));                        // full: nothing
    CHECK(!c.Skip && c.Spell == 0);
    c = Choose(Base(kShaman, 20, 895, 900));
    CHECK(c.Skip);
}

void TankGetsShieldAndHotBeforeTheHit()
{
    Input pull = Base(kPriest, 30, 800, 800);
    pull.IsTank = true;
    pull.InCombat = true;
    pull.Proactive = true;
    Choice c = Choose(pull);
    CHECK(c.Spell == kRenew);                                        // no damage yet: no shield (it is the worst heal per mana), Renew goes on
    pull.Health = 650;
    pull.IncomingPerSec = 100;                                       // real damage and the tank under 85 %: the shield
    c = Choose(pull);
    CHECK(c.Spell == kPowerWordShield && !c.Exact);
    pull.ShieldLocked = true;
    CHECK(Choose(pull).Spell == kRenew);
    pull.HotActive = true;
    CHECK(Choose(pull).Spell == 0);                                  // both on: nothing
    pull.IsTank = false;
    pull.HotActive = false;
    pull.ShieldLocked = false;
    CHECK(Choose(pull).Spell == 0);                                  // only the tank is pre-covered
    Input druid = Base(kDruid, 30, 800, 800);
    druid.IsTank = druid.InCombat = druid.Proactive = true;
    CHECK(Choose(druid).Spell == kRejuvenation);
    Input paladin = Base(kPaladin, 30, 800, 800);
    paladin.IsTank = paladin.InCombat = paladin.Proactive = true;
    CHECK(Choose(paladin).Spell == 0);                               // no shield and no heal over time
    Input lowMana = Base(kPriest, 30, 800, 800);
    lowMana.IsTank = lowMana.InCombat = lowMana.Proactive = true;
    lowMana.Mana = 200;
    lowMana.Health = 600;
    lowMana.IncomingPerSec = 100;
    CHECK(Choose(lowMana).Spell == kRenew);                          // no shield at low mana, Renew is cheap
}

void TankBetweenPullsGetsAHotForASmallDeficit()
{
    Input tank = Base(kPriest, 40, 1700, 2000);                     // deficit 300, out of combat
    tank.IsTank = true;
    Choice c = Choose(tank);
    CHECK(c.Spell == kRenew);
    tank.HotActive = true;
    CHECK(Choose(tank).Spell != kRenew);                             // the Renew still runs: a direct heal only if worth it
    Input big = Base(kPriest, 40, 200, 2000);
    big.IsTank = true;
    CHECK(Choose(big).Spell != kRenew);                              // a big deficit is a direct heal
    Input druid = Base(kDruid, 30, 1700, 2000);
    druid.IsTank = true;
    CHECK(Choose(druid).Spell == kRejuvenation);
}

void PaladinFlashOfLightForSmallLayOnHandsForDying()
{
    Choice c = Choose(Base(kPaladin, 30, 360, 400));                // deficit 40: FoL rank 1 (47) covers it
    CHECK(c.Spell == 19750);
    c = Choose(Base(kPaladin, 30, 100, 700));                       // deficit 600, safe: Holy Light rank that covers it (1042, 453 is not enough; best at 30)
    CHECK(c.Spell == 1042);
    Input dying = Base(kPaladin, 30, 50, 1000);                     // 5 %, taking damage
    dying.IncomingPerSec = 100;
    c = Choose(dying);
    CHECK(c.Spell == kLayOnHands && !c.Exact);
    dying.LayOnHandsReady = false;
    CHECK(Choose(dying).Spell != kLayOnHands);                       // on cooldown: a heal
    Input dyingQuiet = Base(kPaladin, 30, 50, 1000);                // nearly dead but nothing hits it: a heal does it
    CHECK(Choose(dyingQuiet).Spell != kLayOnHands);
}

void ShamanChainHealForManyHurtLesserHealingWaveWhenLow()
{
    Input group = Base(kShaman, 50, 500, 1000);
    group.HurtCount = 3;
    Choice c = Choose(group);
    CHECK(std::string(c.Note) == "chain" && (c.Spell == 1064 || c.Spell == 10622));
    group.HurtCount = 2;
    CHECK(std::string(Choose(group).Note) != "chain");
    Input low = Base(kShaman, 50, 200, 1000);
    low.HurtCount = 1;
    c = Choose(low);
    CHECK(c.Spell == 10467 || c.Spell == 10466);                     // Lesser Healing Wave (44: 427, 52 is out of level at 50)
    CHECK(c.Spell == 10466);
    Input safe = Base(kShaman, 50, 400, 1000);                      // deficit 600, safe: the Healing Wave rank that covers it (8005, 686)
    c = Choose(safe);
    CHECK(c.Spell == 8005);
    Input manaLow = group;
    manaLow.Mana = 300;                                              // 15 %: no Chain Heal
    CHECK(std::string(Choose(manaLow).Note) != "chain");
}

void DruidRegrowthWhenLowRejuvenationForSmall()
{
    Choice c = Choose(Base(kDruid, 30, 250, 800));                  // 31 %, deficit 550: Regrowth best rank at 30 (8940, 312)
    CHECK(c.Spell == 8940);
    c = Choose(Base(kDruid, 30, 600, 800));                         // 75 %, a bit hurt, not the tank: Rejuvenation
    CHECK(c.Spell == kRejuvenation && !c.Exact);
    c = Choose(Base(kDruid, 30, 300, 800));                         // 500 missing: Healing Touch rank covering it (5189, 547)
    CHECK(c.Spell == 5189);
}

void LowManaTakesTheCheapestUsefulRank()
{
    Input in = Base(kPriest, 50, 4000, 6000);                       // deficit 2000: safe + enough mana would be Greater Heal 10964
    in.Mana = 300;                                                   // 15 %: cheap ladder, Heal best affordable (r3 255 at 28; 305 is more than 300)
    Choice c = Choose(in);
    CHECK(c.Spell == 6063 && std::string(c.Note) == "cheap");
}

void ManaReserveAndTiers()
{
    Input in = Base(kPriest, 30, 640, 800);                         // 80 %, deficit 160
    in.Mana = 400;                                                  // 20 %: no top-up of a member above 70 %
    CHECK(Choose(in).Skip && std::string(Choose(in).Note) == "mana_reserve");
    in.IsTank = true;                                               // a tank is topped up until 85 %
    CHECK(!Choose(in).Skip);
    Input fight = Base(kPaladin, 30, 560, 800);                     // 70 %, in a fight, 40 % mana: reserve for those under 65 %
    fight.InCombat = true;
    fight.Mana = 800;
    CHECK(Choose(fight).Skip);
    fight.Health = 480;                                             // 60 %: healed
    CHECK(!Choose(fight).Skip && Choose(fight).Spell != 0);
    Input spike = Base(kPriest, 30, 200, 600);                      // an emergency ignores the reserve
    spike.Mana = 300;
    spike.InCombat = true;
    CHECK(!Choose(spike).Skip && Choose(spike).Spell == 9472);
    Input tankHalf = Base(kShaman, 30, 340, 700);                  // tank under half: the fast tier (Lesser Healing Wave 8008 at 28)
    tankHalf.IsTank = true;
    CHECK(Choose(tankHalf).Spell == 8008);
    Input other = tankHalf;                                         // anyone else at 40 %: the efficient Healing Wave (959 at 32 not yet, 939 covers 600? 343 no: best 913..939)
    other.IsTank = false;
    CHECK(Choose(other).Spell == 939);   // 360 missing: rank 939 (343, level 24) is the best at 30
}

void FirstRankOfAndExactRanks()
{
    CHECK(FirstRankOf(9473) == 2061 && FirstRankOf(10395) == 331 && FirstRankOf(585) == 585 && FirstRankOf(25314) == 2060);
    CHECK(heal_ranks::IsLadderSpell(9472) && heal_ranks::IsLadderSpell(10468) && heal_ranks::IsLadderSpell(25314));
}

}   // namespace

int main()
{
    PriestPicksByDeficitAndUrgency();
    OnlySpellsTheLevelHas();
    NoOverhealSpam();
    TankGetsShieldAndHotBeforeTheHit();
    TankBetweenPullsGetsAHotForASmallDeficit();
    PaladinFlashOfLightForSmallLayOnHandsForDying();
    ShamanChainHealForManyHurtLesserHealingWaveWhenLow();
    DruidRegrowthWhenLowRejuvenationForSmall();
    LowManaTakesTheCheapestUsefulRank();
    ManaReserveAndTiers();
    FirstRankOfAndExactRanks();
    if (failures == 0)
        std::printf("heal_spell_test: ok\n");
    return failures == 0 ? 0 : 1;
}
