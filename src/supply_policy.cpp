#include "supply_policy.h"

#include <algorithm>

namespace supply {

Needs Assess(Settings const& settings, Facts const& facts)
{
    Needs needs;
    if (!settings.Enabled || facts.FreeSlots == 0 || facts.Copper < settings.MinCopper)
        return needs;

    needs.Ammo = facts.AmmoEntry != 0 && facts.AmmoCount < settings.AmmoLow && facts.AmmoVendor;
    needs.Food = facts.FoodCount < settings.FoodLow && facts.FoodVendor;
    needs.Drink = facts.ManaUser && facts.DrinkCount < settings.DrinkLow && facts.FoodVendor;
    needs.HealthPotion = settings.Potions && facts.HealthPotions < settings.HealthPotionLow && facts.GoodsVendor;
    needs.ManaPotion = settings.Potions && facts.ManaUser && facts.ManaPotions < settings.ManaPotionLow && facts.GoodsVendor;
    needs.Bandage = settings.Bandages && facts.Bandages < settings.BandageLow && facts.GoodsVendor;
    needs.Reagent = settings.Reagents && facts.ReagentEntry != 0 && facts.ReagentVendor;
    return needs;
}

Stop NextStop(Needs const& needs)
{
    if (needs.Ammo)
        return Stop::Ammo;
    if (needs.Food || needs.Drink)
        return Stop::Food;
    if (needs.Goods())
        return Stop::Goods;
    if (needs.Reagent)
        return Stop::Reagent;
    return Stop::None;
}

Request RequestFor(Settings const& settings, Needs const& needs)
{
    Request request;
    if (needs.Ammo)
        request.Ammo = settings.AmmoTarget;
    if (needs.Food)
        request.Food = settings.FoodTarget;
    if (needs.Drink)
        request.Drink = settings.DrinkTarget;
    request.HealthPotion = needs.HealthPotion;
    request.ManaPotion = needs.ManaPotion;
    request.Bandage = needs.Bandage;
    request.Reagent = needs.Reagent;
    return request;
}

std::uint64_t BackoffMs(Settings const& settings, std::uint32_t failures)
{
    std::uint64_t wait = settings.FailBackoffMs;
    for (std::uint32_t i = 1; i < failures && wait < settings.MaxBackoffMs; ++i)
        wait *= 2;
    return std::min(wait, settings.MaxBackoffMs);
}

bool Improved(Needs const& asked, Facts const& before, Facts const& after)
{
    return (asked.Ammo && after.AmmoCount > before.AmmoCount) || (asked.Food && after.FoodCount > before.FoodCount) ||
           (asked.Drink && after.DrinkCount > before.DrinkCount) || (asked.HealthPotion && after.HealthPotions > before.HealthPotions) ||
           (asked.ManaPotion && after.ManaPotions > before.ManaPotions) || (asked.Bandage && after.Bandages > before.Bandages) ||
           (asked.Reagent && before.ReagentEntry == after.ReagentEntry && after.ReagentCount > before.ReagentCount) ||
           (asked.Reagent && before.ReagentEntry != 0 && after.ReagentEntry == 0); // enough now: the core stopped naming it
}

std::uint32_t ConsumeNow(Settings const& settings, Facts const& facts, std::uint32_t healthPct, std::uint32_t manaPct)
{
    if (!settings.Enabled)
        return 0;
    if (facts.ManaUser && facts.DrinkCount > 0 && manaPct < settings.DrinkBelowManaPct)
        return 2;
    if (facts.FoodCount > 0 && healthPct < settings.EatBelowHealthPct)
        return 1;
    return 0;
}

std::uint32_t PotionNow(Settings const& settings, Facts const& facts, std::uint32_t healthPct, std::uint32_t manaPct)
{
    if (!settings.Enabled || !settings.Potions)
        return 0;
    if (facts.HealthPotions > 0 && healthPct < settings.UseHealthPotionBelowPct)
        return 3;
    if (facts.ManaUser && facts.ManaPotions > 0 && manaPct < settings.UseManaPotionBelowPct)
        return 4;
    return 0;
}

std::uint32_t BandageNow(Settings const& settings, Facts const& facts, std::uint32_t healthPct)
{
    if (!settings.Enabled || !settings.Bandages)
        return 0;
    return facts.Bandages > 0 && facts.FoodCount == 0 && healthPct < settings.UseBandageBelowPct ? 5 : 0;
}

} // namespace supply
