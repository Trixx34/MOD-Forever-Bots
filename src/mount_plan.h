// SPDX-License-Identifier: GPL-2.0-or-later
// Mounts (MountBuy = 1 with RidingTraining = 1 and Errands = 1, off by default): when a bot that is refused a mount should go to a riding trainer, and when to a
// mount vendor, and what it buys. Pure logic: no core, no clock, no file access (tests/mount_plan_test.cpp).
//
// The module cannot see a bot's riding skill or its bags. What it does see: the core refused the bot's mount request (policy MountFails > 0: no riding skill or
// no mount learned) and the bot's level and copper. So a bot is sent to the riding trainer or the mount vendor only after a refusal, in this order: riding
// rank first (riding_training.h), then a mount of its race from the vendor table (data/mount_vendors.txt). The riding skill it has learned is the module's own
// book (SkillAfterTraining), kept in memory: after a restart a bot is refused again if it still has nothing, and the trip is repeated at most once.
#pragma once

#include "riding_training.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mounts {

struct Vendor
{
    std::uint32_t RaceMask = 0;     // the race whose mounts it sells: 1 << (race id - 1) (1 Human, 2 Orc, 4 Dwarf, 8 Night Elf, 16 Undead, 32 Tauren, 64 Gnome, 128 Troll)
    std::uint32_t Map = 0;
    std::uint32_t Entry = 0;
    std::uint64_t Guid = 0;         // the spawn guid
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    std::string Name;
    std::string Subname;
};

struct Item
{
    std::uint32_t VendorEntry = 0;
    std::uint32_t Entry = 0;
    std::uint32_t Level = 0;        // the level the mount needs
    std::uint32_t Skill = 0;        // the riding skill the mount needs (75, 150)
    std::uint64_t Copper = 0;
    std::string Name;
};

struct Table
{
    std::vector<Vendor> Vendors;
    std::vector<Item> Items;
};

// Parses data/mount_vendors.txt: "vendor|raceMask|map|entry|guid|x y z|name|subname" and "item|vendorEntry|itemEntry|level|skill|copper|name" lines; blank lines and '#'
// lines are skipped; a malformed line is left out and described in `errors` ("line N: ..."). An item whose vendor line is missing is dropped.
Table ParseTable(std::string_view text, std::vector<std::string>& errors);

// The race bit of a race id (1..8 -> 1..128, 0 for any other race).
std::uint32_t RaceBit(std::uint32_t race);

// Copper a bot may spend: what it carries above `reservePerLevel` copper per level (the core keeps its own reserve for supplies; this one keeps gold for the bot's
// other needs, so a bot is not left with nothing after the trainer).
std::uint64_t Spendable(std::uint64_t copper, std::uint32_t level, std::uint64_t reservePerLevel);

struct Pick
{
    std::size_t Item = 0;           // index into Table::Items
    std::size_t Vendor = 0;         // index into Table::Vendors
};

// The mount to buy: an item of a vendor of the bot's race on its map that its level, its riding skill and its spendable copper allow. The highest level wins,
// then the cheaper, then the nearer vendor (then the lower entry). nullopt when none (the bot is not sent).
std::optional<Pick> ChooseMount(Table const& table, std::uint32_t race, std::uint32_t level, std::uint32_t ridingSkill, std::uint64_t spendable, std::uint32_t map,
                                float x, float y);

// The cheapest mount of the bot's race that its level allows with the skill rank it would have (so the riding step keeps that much back). 0 when there is none.
std::uint64_t CheapestMount(Table const& table, std::uint32_t race, std::uint32_t level, std::uint32_t ridingSkill);

// The riding skill after a trainer visit at `level` with `copper` to spend: every rank that is due and paid for, in table order (the trainer's kTrain learns all it
// can). `skill` is the skill before.
std::uint32_t SkillAfterTraining(std::vector<riding::Rank> const& ranks, std::uint32_t level, std::uint32_t skill, std::uint64_t spendable);

enum class Step : std::uint8_t
{
    None,       // nothing to do (no refusal, no money, no trainer or vendor to go to)
    Train,      // walk to a riding trainer and train
    Buy         // walk to a mount vendor and buy `Pick`
};

struct Plan
{
    Step What = Step::None;
    std::size_t Rank = 0;           // Train: the next rank (index into the rank table)
    Pick Mount;                     // Buy: the item and its vendor
};

// What a bot refused a mount should do now. `ridingSkill` is the module's book for it. Needs `refused` (the core said no), a level of at least the first rank, and a
// vendor of its race on its map. A riding rank is due when riding::NextRank finds one with the copper for it AND for the cheapest mount after it.
Plan Decide(std::vector<riding::Rank> const& ranks, Table const& table, bool refused, std::uint32_t race, std::uint32_t level, std::uint32_t ridingSkill,
            std::uint64_t copper, std::uint64_t reservePerLevel, std::uint32_t map, float x, float y);

} // namespace mounts
