// Random race and class for spawned bots. Pure logic: the seeded stream and the pick, unit-tested in tests/pick_test.cpp.
//
// The combinations are the ones the Forever server offers (the Wowhead "Forever" race and class table, patch 1.60.1).
// The core still validates every pick when it creates the character (Player::Create refuses a pair with no player
// info), so a stale entry here costs one refused spawn, not a bad bot. The team rule (Alliance or Horde) follows the race.
#pragma once

#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>

struct RaceClass
{
    std::uint32_t Race;
    std::uint32_t Class;
};

// Race and class ids as the core uses them (ChrRaces and ChrClasses).
namespace pick
{
constexpr std::uint32_t kRandom = 0; // config value "random": pick from the valid combinations

// Every valid combination, in table order.
std::span<RaceClass const> ValidCombinations();

bool IsValidCombination(std::uint32_t race, std::uint32_t classId);
bool IsAllianceRace(std::uint32_t race);
bool IsHordeRace(std::uint32_t race);

// A deterministic stream (SplitMix64). Picks use plain modulo, so the same seed gives the same bots on any compiler
// (std::uniform_int_distribution is not specified to do that).
class Stream
{
public:
    explicit Stream(std::uint64_t seed) : _state(seed) {}
    std::uint64_t Next();
    std::uint32_t Below(std::uint32_t n); // uniform-ish in [0, n); n must be non-zero

private:
    std::uint64_t _state;
};

// Picks one valid combination. A fixed value in raceSpec or classSpec is kept; kRandom picks from the rest. Returns false,
// with the reason, when no valid combination fits the fixed values.
bool Pick(Stream& stream, std::uint32_t raceSpec, std::uint32_t classSpec, RaceClass& out, std::string& reason);
// The class of each bot the module controls, by guid. The Run() thread records it when a bot is spawned or reclaimed, and
// the pump thread reads it for the rotation, so access is locked.
class ClassTable
{
public:
    void Set(std::uint64_t botGuid, std::uint32_t classId);
    // 0 for a bot that is not in the table.
    std::uint32_t Get(std::uint64_t botGuid) const;
    // The race of a bot (M6: each race starts in its own city, and its vendor is there). 0 when unknown.
    void SetRace(std::uint64_t botGuid, std::uint32_t race);
    std::uint32_t GetRace(std::uint64_t botGuid) const;
    std::size_t Size() const;

private:
    mutable std::mutex _mutex;
    std::unordered_map<std::uint64_t, std::uint32_t> _classes;
    std::unordered_map<std::uint64_t, std::uint32_t> _races;
};
} // namespace pick
