// SPDX-License-Identifier: GPL-2.0-or-later
// Repair trips: when a bot's broken gear sends it to a repairman at once, outside the routine vendor visit, and how a trip that repaired nothing backs
// off. Pure logic with no SDK dependency, unit-tested in tests/repair_test.cpp. BotPolicy (TurnShopping) feeds it the snapshot facts and sends the
// kVendor action (sell, repair, equip) at the repairman; the core sells first and repairs from what the sale left (ModuleBotActions, kVendor).
//
// Why (prod, 2026-11-14): 232 of 463 equipped pieces were at durability 0. The routine vendor visit (ServiceEverySec) is made only by an idle bot, and a
// bot with quest work is never idle, so a leveling bot never reached a repairman; a party member never walks to one at all. A broken piece gives no stats,
// so the bot fought naked and died, and every death broke more.
//
// The snapshot (layout 9) only carries the lowest durability of the equipped pieces (durability_pct), not their average or the repair cost: the rule
// is on the lowest piece. An average or a cost needs a core change (doc/economy.md, Urgent repair).
#pragma once

#include <cstdint>

namespace repair {

struct Settings
{
    bool Enabled = true;                     // config key RepairUrgent (on by default: a broken bot must repair, with or without AutoGearing)
    std::uint32_t BrokenAtPct = 0;           // config key RepairBrokenPct (0..99): a piece at or below this durability sends the bot to repair at once
    std::uint32_t AfterDeathBelowPct = 50;   // config key RepairAfterDeathPct (0..100, 0 = off): after a death, a piece below this sends it too
    std::uint64_t RetryMs = 300000;          // config key RepairRetrySec (60..86400): no new urgent trip within this of the last one
    std::uint64_t MaxBackoffMs = 3600000;    // a trip that repaired nothing doubles the wait, up to this
    std::uint64_t TripMaxMs = 600000;        // a trip (walks, fights on the way, a death) that has not reached the repairman by then has failed
    std::uint64_t LowCopperPerLevel = 100;   // config key RepairLowCopperPerLevel (0..100000): with less money than level x this, the visit also sells white junk
};

// What the policy knows about the bot.
struct Facts
{
    std::uint32_t LowestPct = 100;  // BotSnapshot::durability_pct (100 when nothing worn wears out)
    bool RepairmanKnown = false;    // repair_valid: a repairman on the bot's map
    bool AfterDeath = false;        // revived since the last repair trip
    std::uint64_t Copper = 0;
    std::uint32_t Level = 0;
};

enum class Why : std::uint8_t
{
    None,
    Broken,      // a piece at or below BrokenAtPct
    AfterDeath,  // revived, and a piece below AfterDeathBelowPct
};

// Whether the bot needs an urgent repair trip (no cooldown checked here: the policy keeps the timers).
Why Needed(Settings const& settings, Facts const& facts);

// Whether the visit should sell the white junk too (kServiceSellJunk) so the bot can pay. The grey items are always sold first.
bool SellJunkFirst(Settings const& settings, Facts const& facts);

// The wait after `failures` trips in a row that repaired nothing (failures >= 1): RetryMs, doubled each time, at most MaxBackoffMs.
std::uint64_t BackoffMs(Settings const& settings, std::uint32_t failures);

// Whether the trip did something: the lowest piece is better than before. A partial repair that leaves the worst piece broken is not
// (and the core repairs all or nothing: DurabilityRepairAll pays the whole bill or repairs no piece), and the bot waits for the backoff and more money.
bool Improved(std::uint32_t beforePct, std::uint32_t afterPct);

char const* WhyName(Why why);

} // namespace repair
