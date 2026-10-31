// SPDX-License-Identifier: GPL-2.0-or-later
#include "bot_pick.h"

#include <array>
#include <mutex>
#include <string>

namespace pick
{

namespace {

// Race 1 Human, 2 Orc, 3 Dwarf, 4 Night Elf, 5 Undead, 6 Tauren, 7 Gnome, 8 Troll, 95 Skyborne (Alliance), 96 Skyborne
// (Horde). Class 1 Warrior, 2 Paladin, 3 Hunter, 4 Rogue, 5 Priest, 7 Shaman, 8 Mage, 9 Warlock, 11 Druid.
// The table is the Wowhead Forever grid: the ticks and stars of each race column. Skyborne Mage is Alliance only and
// Skyborne Shaman is Horde only, which is what the race split above encodes.
constexpr auto kValid = std::to_array<RaceClass>({
    // Human
    { 1, 1 }, { 1, 2 }, { 1, 3 }, { 1, 4 }, { 1, 5 }, { 1, 8 }, { 1, 9 },
    // Orc
    { 2, 1 }, { 2, 3 }, { 2, 4 }, { 2, 7 }, { 2, 8 }, { 2, 9 },
    // Dwarf
    { 3, 1 }, { 3, 2 }, { 3, 3 }, { 3, 4 }, { 3, 5 }, { 3, 7 },
    // Night Elf
    { 4, 1 }, { 4, 3 }, { 4, 4 }, { 4, 5 }, { 4, 11 },
    // Undead
    { 5, 1 }, { 5, 2 }, { 5, 4 }, { 5, 5 }, { 5, 8 }, { 5, 9 },
    // Tauren
    { 6, 1 }, { 6, 3 }, { 6, 7 }, { 6, 11 },
    // Gnome
    { 7, 1 }, { 7, 4 }, { 7, 5 }, { 7, 8 }, { 7, 9 },
    // Troll
    { 8, 1 }, { 8, 3 }, { 8, 4 }, { 8, 5 }, { 8, 7 }, { 8, 8 }, { 8, 9 },
    // Skyborne, Alliance (Mage is Alliance only)
    { 95, 1 }, { 95, 3 }, { 95, 4 }, { 95, 8 }, { 95, 11 },
    // Skyborne, Horde (Shaman is Horde only)
    { 96, 1 }, { 96, 3 }, { 96, 4 }, { 96, 7 }, { 96, 11 },
});

constexpr std::array<std::uint32_t, 5> kAllianceRaces = { 1, 3, 4, 7, 95 };
constexpr std::array<std::uint32_t, 5> kHordeRaces = { 2, 5, 6, 8, 96 };

bool Contains(auto const& values, std::uint32_t value)
{
    for (auto const& v : values)
        if (v == value)
            return true;
    return false;
}

} // namespace

std::span<RaceClass const> ValidCombinations()
{
    return kValid;
}

bool IsValidCombination(std::uint32_t race, std::uint32_t classId)
{
    for (RaceClass const& combo : kValid)
        if (combo.Race == race && combo.Class == classId)
            return true;
    return false;
}

bool IsAllianceRace(std::uint32_t race)
{
    return Contains(kAllianceRaces, race);
}

bool IsHordeRace(std::uint32_t race)
{
    return Contains(kHordeRaces, race);
}

std::uint64_t Stream::Next()
{
    // SplitMix64: a well-mixed sequence from any seed, including zero.
    _state += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = _state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

std::uint32_t Stream::Below(std::uint32_t n)
{
    return static_cast<std::uint32_t>(Next() % n);
}

bool Pick(Stream& stream, std::uint32_t raceSpec, std::uint32_t classSpec, RaceClass& out, std::string& reason)
{
    // Every valid combination that matches the fixed values.
    std::array<RaceClass, kValid.size()> fits{};
    std::size_t count = 0;
    for (RaceClass const& combo : kValid)
    {
        if (raceSpec != kRandom && combo.Race != raceSpec)
            continue;
        if (classSpec != kRandom && combo.Class != classSpec)
            continue;
        fits[count++] = combo;
    }

    if (count == 0)
    {
        reason = "no valid race and class combination for race " + std::to_string(raceSpec) + " class " +
                 std::to_string(classSpec);
        return false;
    }

    out = fits[stream.Below(static_cast<std::uint32_t>(count))];
    return true;
}

void ClassTable::Set(std::uint64_t botGuid, std::uint32_t classId)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _classes[botGuid] = classId;
}

std::uint32_t ClassTable::Get(std::uint64_t botGuid) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto const it = _classes.find(botGuid);
    return it == _classes.end() ? 0 : it->second;
}

void SpecTable::Set(std::uint64_t botGuid, std::string spec)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _specs[botGuid] = std::move(spec);
}

std::string SpecTable::Get(std::uint64_t botGuid) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto const it = _specs.find(botGuid);
    return it == _specs.end() ? std::string() : it->second;
}

std::size_t ClassTable::Size() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _classes.size();
}

} // namespace pick
