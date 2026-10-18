// Bot supplies (layout 15): when a bot is short of ammunition, food or drink, where it shops, what it asks the vendor for, and how a failed trip
// backs off. Pure logic with no SDK dependency, unit-tested in tests/supply_test.cpp. BotPolicy feeds it the snapshot facts and sends the
// kVendor buy action and the kConsume action; the core chooses the items from the vendor's stock.
#pragma once

#include <cstdint>

namespace supply {

struct Settings
{
    bool Enabled = false;                      // config key Supplies (needs Bot.Actions.Supplies in the core, on by default there)
    std::uint32_t AmmoLow = 100;               // a bot with fewer pieces of ammunition than this goes shopping
    std::uint32_t AmmoTarget = 400;            // and buys up to this many (config keys SupplyAmmoLow, SupplyAmmoTarget)
    std::uint32_t FoodLow = 3;                 // food: a bot with fewer pieces than this goes shopping ...
    std::uint32_t FoodTarget = 10;             // ... for this many
    std::uint32_t DrinkLow = 3;                // drink, for a bot with mana
    std::uint32_t DrinkTarget = 10;
    std::uint64_t TripEveryMs = 120000;        // a bot that needs supplies makes a trip at most this often (config key SupplyTripSec)
    std::uint64_t FailBackoffMs = 600000;      // after a trip that bought nothing, the first wait; it doubles with each failure in a row
    std::uint64_t MaxBackoffMs = 3600000;
    std::uint64_t MinCopper = 10;              // a bot with less money than this does not shop
    std::uint32_t EatBelowHealthPct = 70;      // a resting bot eats below this health ...
    std::uint32_t DrinkBelowManaPct = 40;      // ... and drinks below this mana
    std::uint64_t ConsumeEveryMs = 24000;      // food and drink work over about 20 to 30 seconds: not used again sooner
};

// The facts of a bot's snapshot that matter here.
struct Facts
{
    std::uint32_t AmmoEntry = 0;    // 0: the bot's ranged weapon uses no ammunition
    std::uint32_t AmmoCount = 0;
    std::uint32_t FoodCount = 0;
    std::uint32_t DrinkCount = 0;
    bool ManaUser = false;
    std::uint64_t Copper = 0;
    std::uint32_t FreeSlots = 0;
    bool AmmoVendor = false;        // the snapshot names a vendor that sells ammunition the bot can use
    bool FoodVendor = false;        // ... and one that sells food or drink it can use
};

struct Needs
{
    bool Ammo = false;
    bool Food = false;
    bool Drink = false;
    bool Any() const { return Ammo || Food || Drink; }
};

// What the bot is short of and can do something about: a vendor that sells it exists, there is a free bag slot, and a little money.
Needs Assess(Settings const& settings, Facts const& facts);

// The vendor to walk to first: ammunition before food (a shooter without ammunition cannot fight at range).
enum class Stop : std::uint8_t { None, Ammo, Food };
Stop NextStop(Needs const& needs);

// What a visit asks for: the counts to bring the bot up to (0 = nothing of that kind). A vendor without the goods simply sells none.
struct Request
{
    std::uint32_t Ammo = 0;
    std::uint32_t Food = 0;
    std::uint32_t Drink = 0;
};
Request RequestFor(Settings const& settings, Needs const& needs);

// The wait before the next trip after `failures` trips in a row that bought nothing (failures >= 1).
std::uint64_t BackoffMs(Settings const& settings, std::uint32_t failures);

// Whether a trip bought what it asked for: any kind that was requested has more pieces than before.
bool Improved(Needs const& asked, Facts const& before, Facts const& after);

// Whether a resting bot should eat (1) or drink (2) now, or 0. Drinking comes first for a mana user below the mana threshold.
std::uint32_t ConsumeNow(Settings const& settings, Facts const& facts, std::uint32_t healthPct, std::uint32_t manaPct);

} // namespace supply
