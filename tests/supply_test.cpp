// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the supply decisions (src/supply_policy.h). No core or SDK needed.
#include "supply_policy.h"

#include <cstdio>

using namespace supply;

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
    Settings settings;
    settings.Enabled = true;
    return settings;
}

// A hunter that shoots ammunition 2512, with money, room and both kinds of vendor on the map.
Facts Hunter(std::uint32_t ammo, std::uint32_t food = 10, std::uint32_t drink = 0)
{
    Facts facts;
    facts.AmmoEntry = 2512;
    facts.AmmoCount = ammo;
    facts.FoodCount = food;
    facts.DrinkCount = drink;
    facts.Copper = 500;
    facts.FreeSlots = 4;
    facts.AmmoVendor = true;
    facts.FoodVendor = true;
    return facts;
}

void AmmunitionBelowTheLowMarkIsNeeded()
{
    Needs low = Assess(On(), Hunter(99));
    CHECK(low.Ammo && !low.Food && !low.Drink && low.Any());
    CHECK(!Assess(On(), Hunter(100)).Any()); // at the mark is enough
    CHECK(!Assess(On(), Hunter(0, 10)).Food);
}

void ABotWithNoAmmoWeaponNeedsNone()
{
    Facts facts = Hunter(0);
    facts.AmmoEntry = 0;
    CHECK(!Assess(On(), facts).Ammo);
}

void FoodAndDrinkFollowTheClass()
{
    Facts caster = Hunter(500, 0, 0);
    caster.AmmoEntry = 0;
    caster.ManaUser = true;
    Needs both = Assess(On(), caster);
    CHECK(both.Food && both.Drink && !both.Ammo);

    caster.ManaUser = false; // a warrior has no mana to restore
    Needs foodOnly = Assess(On(), caster);
    CHECK(foodOnly.Food && !foodOnly.Drink);
}

void NothingIsNeededWithoutAVendorMoneyOrRoom()
{
    Facts facts = Hunter(0, 0, 0);
    facts.ManaUser = true;
    facts.AmmoVendor = false;
    Needs noAmmoVendor = Assess(On(), facts);
    CHECK(!noAmmoVendor.Ammo && noAmmoVendor.Food && noAmmoVendor.Drink);
    facts.FoodVendor = false;
    CHECK(!Assess(On(), facts).Any());

    Facts poor = Hunter(0, 0, 0);
    poor.Copper = 9;
    CHECK(!Assess(On(), poor).Any());
    Facts full = Hunter(0, 0, 0);
    full.FreeSlots = 0;
    CHECK(!Assess(On(), full).Any());
    CHECK(!Assess(Settings{}, Hunter(0, 0, 0)).Any()); // off by default
}

void AmmunitionComesFirst()
{
    Needs needs;
    needs.Ammo = true;
    needs.Food = true;
    CHECK(NextStop(needs) == Stop::Ammo);
    needs.Ammo = false;
    CHECK(NextStop(needs) == Stop::Food);
    needs.Food = false;
    needs.Drink = true;
    CHECK(NextStop(needs) == Stop::Food);
    needs.Drink = false;
    CHECK(NextStop(needs) == Stop::None);
}

void TheRequestAsksForTheTargetsOfWhatIsNeeded()
{
    Needs needs;
    needs.Ammo = true;
    needs.Drink = true;
    Request request = RequestFor(On(), needs);
    CHECK(request.Ammo == 400 && request.Food == 0 && request.Drink == 10);
}

void BackoffDoublesAndIsCapped()
{
    Settings settings = On();
    CHECK(BackoffMs(settings, 1) == 600000);
    CHECK(BackoffMs(settings, 2) == 1200000);
    CHECK(BackoffMs(settings, 3) == 2400000);
    CHECK(BackoffMs(settings, 4) == 3600000);
    CHECK(BackoffMs(settings, 20) == 3600000);
}

void ATripIsJudgedByTheCounts()
{
    Needs asked;
    asked.Ammo = true;
    Facts before = Hunter(10);
    Facts after = before;
    CHECK(!Improved(asked, before, after));
    after.AmmoCount = 410;
    CHECK(Improved(asked, before, after));
    // More food does not make an ammunition trip a success.
    after.AmmoCount = 10;
    after.FoodCount = 99;
    CHECK(!Improved(asked, before, after));
}

void RestingBotsEatAndDrinkByThresholds()
{
    Settings settings = On();
    Facts facts = Hunter(500, 5, 5);
    facts.ManaUser = true;
    CHECK(ConsumeNow(settings, facts, 50, 100) == 1);   // hurt: eat
    CHECK(ConsumeNow(settings, facts, 90, 100) == 0);   // fine
    CHECK(ConsumeNow(settings, facts, 90, 20) == 2);    // dry: drink
    CHECK(ConsumeNow(settings, facts, 50, 20) == 2);    // both: drink first
    facts.DrinkCount = 0;
    CHECK(ConsumeNow(settings, facts, 90, 20) == 0);    // nothing to drink
    facts.FoodCount = 0;
    CHECK(ConsumeNow(settings, facts, 50, 100) == 0);   // nothing to eat
    CHECK(ConsumeNow(Settings{}, Hunter(500, 5, 5), 10, 10) == 0); // off
}

Settings WithGoods()
{
    Settings settings = On();
    settings.Potions = true;
    settings.Bandages = true;
    return settings;
}

Facts Shopper()
{
    Facts facts = Hunter(500, 10, 0);   // ammunition and food are fine
    facts.ManaUser = true;
    facts.DrinkCount = 10;
    facts.GoodsVendor = true;
    return facts;
}

void PotionsAndBandagesAreOffUnlessAsked()
{
    Facts facts = Shopper();
    CHECK(!Assess(On(), facts).Any());                       // the defaults ask for none
    Needs needs = Assess(WithGoods(), facts);
    CHECK(needs.HealthPotion && needs.ManaPotion && needs.Bandage && needs.Goods() && needs.Any());
    Settings potionsOnly = On();
    potionsOnly.Potions = true;
    Needs onlyPotions = Assess(potionsOnly, facts);
    CHECK(onlyPotions.HealthPotion && onlyPotions.ManaPotion && !onlyPotions.Bandage);
}

void GoodsFollowTheClassTheStockAndTheCounts()
{
    Facts facts = Shopper();
    facts.ManaUser = false;                                   // a warrior needs no mana potion
    Needs warrior = Assess(WithGoods(), facts);
    CHECK(warrior.HealthPotion && !warrior.ManaPotion && warrior.Bandage);
    facts.GoodsVendor = false;                                // nowhere to buy them
    CHECK(!Assess(WithGoods(), facts).Any());
    facts = Shopper();
    facts.HealthPotions = 1;                                  // at the low mark of 1 is enough
    facts.ManaPotions = 1;
    facts.Bandages = 3;
    CHECK(!Assess(WithGoods(), facts).Any());
    facts.Bandages = 2;
    CHECK(Assess(WithGoods(), facts).Bandage);
    Facts poor = Shopper();
    poor.Copper = 9;
    CHECK(!Assess(WithGoods(), poor).Any());                  // the money floor applies to goods too
}

void GoodsComeAfterAmmunitionAndFood()
{
    Needs needs;
    needs.Bandage = true;
    CHECK(NextStop(needs) == Stop::Goods);
    needs.Food = true;
    CHECK(NextStop(needs) == Stop::Food);
    needs.Ammo = true;
    CHECK(NextStop(needs) == Stop::Ammo);
    Needs potion;
    potion.ManaPotion = true;
    CHECK(NextStop(potion) == Stop::Goods);
    Request request = RequestFor(WithGoods(), potion);
    CHECK(request.ManaPotion && !request.HealthPotion && !request.Bandage && request.Ammo == 0);
}

void AGoodsTripIsJudgedByItsOwnCounts()
{
    Needs asked;
    asked.HealthPotion = true;
    Facts before = Shopper();
    Facts after = before;
    after.Bandages = 10;                                      // another kind does not count
    CHECK(!Improved(asked, before, after));
    after.HealthPotions = 3;
    CHECK(Improved(asked, before, after));
}

void PotionsAreDrunkInAFightByThresholds()
{
    Settings settings = WithGoods();
    Facts facts = Shopper();
    facts.HealthPotions = 2;
    facts.ManaPotions = 2;
    CHECK(PotionNow(settings, facts, 34, 100) == 3);
    CHECK(PotionNow(settings, facts, 35, 100) == 0);          // at the mark: not yet
    CHECK(PotionNow(settings, facts, 80, 14) == 4);
    CHECK(PotionNow(settings, facts, 20, 10) == 3);           // both low: health first
    facts.HealthPotions = 0;
    CHECK(PotionNow(settings, facts, 20, 100) == 0);          // none to drink
    CHECK(PotionNow(settings, facts, 20, 10) == 4);
    facts.ManaUser = false;
    CHECK(PotionNow(settings, facts, 90, 5) == 0);            // no mana to restore
    CHECK(PotionNow(On(), Shopper(), 10, 10) == 0);           // potions are off by default
    CHECK(PotionNow(Settings{}, Shopper(), 10, 10) == 0);
}

void ABotWithNoFoodBandagesWhenHurt()
{
    Settings settings = WithGoods();
    Facts facts = Shopper();
    facts.Bandages = 4;
    facts.FoodCount = 0;
    CHECK(BandageNow(settings, facts, 59) == 5);
    CHECK(BandageNow(settings, facts, 60) == 0);
    facts.FoodCount = 3;                                      // food is the first choice
    CHECK(BandageNow(settings, facts, 40) == 0);
    facts.FoodCount = 0;
    facts.Bandages = 0;
    CHECK(BandageNow(settings, facts, 40) == 0);
    facts.Bandages = 4;
    CHECK(BandageNow(On(), facts, 40) == 0);                  // off by default
}

void AReagentTripNeedsTheSettingAnItemAndAVendor()
{
    Facts facts = Shopper();
    facts.ReagentEntry = 17026;
    facts.ReagentCount = 0;
    facts.ReagentVendor = true;
    Settings reagents = On();
    reagents.Reagents = true;
    CHECK(Assess(reagents, facts).Reagent && Assess(reagents, facts).Any() && !Assess(reagents, facts).Goods());
    CHECK(!Assess(On(), facts).Any());                       // off by default
    Facts noVendor = facts;
    noVendor.ReagentVendor = false;
    CHECK(!Assess(reagents, noVendor).Any());
    Facts none = facts;
    none.ReagentEntry = 0;                                   // the core names nothing: nothing is missing
    CHECK(!Assess(reagents, none).Any());

    Needs needs = Assess(reagents, facts);
    CHECK(NextStop(needs) == Stop::Reagent && RequestFor(reagents, needs).Reagent);
    needs.Bandage = true;                                    // goods come first
    CHECK(NextStop(needs) == Stop::Goods);
}

void AReagentTripIsJudgedByTheCountOrByTheCoreStoppingTheNote()
{
    Needs asked;
    asked.Reagent = true;
    Facts before = Shopper();
    before.ReagentEntry = 17026;
    before.ReagentCount = 1;
    Facts after = before;
    CHECK(!Improved(asked, before, after));
    after.ReagentCount = 6;
    CHECK(Improved(asked, before, after));
    after = before;
    after.ReagentEntry = 0;                                  // carries enough now
    after.ReagentCount = 0;
    CHECK(Improved(asked, before, after));
}

} // namespace

int main()
{
    AmmunitionBelowTheLowMarkIsNeeded();
    ABotWithNoAmmoWeaponNeedsNone();
    FoodAndDrinkFollowTheClass();
    NothingIsNeededWithoutAVendorMoneyOrRoom();
    AmmunitionComesFirst();
    TheRequestAsksForTheTargetsOfWhatIsNeeded();
    BackoffDoublesAndIsCapped();
    ATripIsJudgedByTheCounts();
    RestingBotsEatAndDrinkByThresholds();
    PotionsAndBandagesAreOffUnlessAsked();
    GoodsFollowTheClassTheStockAndTheCounts();
    GoodsComeAfterAmmunitionAndFood();
    AGoodsTripIsJudgedByItsOwnCounts();
    PotionsAreDrunkInAFightByThresholds();
    ABotWithNoFoodBandagesWhenHurt();
    AReagentTripNeedsTheSettingAnItemAndAVendor();
    AReagentTripIsJudgedByTheCountOrByTheCoreStoppingTheNote();
    if (failures == 0)
        std::printf("supply_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
