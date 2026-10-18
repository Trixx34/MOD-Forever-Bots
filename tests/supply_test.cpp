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
    if (failures == 0)
        std::printf("supply_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
