// SPDX-License-Identifier: GPL-2.0-or-later
// Reputation planner (data/reputation_targets.txt, data/reputation_sources.txt, tools/gen_reputation_sources.py; doc/reputation.md): a bot picks one faction of the
// progression path that fits its level and side (Argent Dawn, Timbermaw Hold, Zandalarian Emissaries, Cenarion Circle, Thorium Brotherhood, Hydraxian Waterlords,
// Brood of Nozdormu) and then one source of reputation for it: a quest, an item hand-in quest, or a creature to kill in the open world. Off by default
// (ReputationPlan = 1 on top of ReputationGrind = 1). Pure logic: no core, no clock, no file access (tests/reputation_plan_test.cpp).
//
// Everything is deterministic by guid (a hash, never a random number) and bounded per call: one call looks at no more than Budget sources, however many the
// faction has, and a bot plans again only every ReplanMs. The module only says where the earning should happen (a journal line and the Step the policy can
// walk to); the quests and the kills themselves are played by the quest planner and the grind code.
#pragma once

#include "reputation.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace reputation {

// A faction of the progression path (a line of data/reputation_targets.txt).
struct Target
{
    std::uint32_t Faction = 0;
    std::string Name;
    std::uint8_t Side = 0;          // 0 both, 1 Alliance, 2 Horde
    std::uint32_t MinLevel = 0;     // a bot below this level does not work on it
    Rank GoalRank = kFriendly;      // the bot is done with the faction at this rank
    std::uint32_t Tier = 1;         // lower tiers first
};

// One way to earn reputation (a line of data/reputation_sources.txt).
struct Source
{
    enum Kind : std::uint8_t { kQuest, kHandIn, kKills, kKill };   // kQuest/kHandIn/kKills are quests; kKill is a creature
    Kind What = kQuest;
    std::uint32_t Faction = 0;
    std::uint32_t Id = 0;           // quest id or creature entry
    std::int32_t Points = 0;
    std::uint32_t Level = 0;        // quest level (0 for a creature)
    std::uint8_t Side = 0;
    Rank Cap = kRankCount;          // the highest rank the source still gives reputation up to; kRankCount = no cap
    std::uint32_t Map = 0;
    float X = 0.0f, Y = 0.0f, Z = 0.0f;
    std::uint32_t Spawns = 0;
};

struct PlanData
{
    std::vector<Target> Targets;
    std::vector<Source> Sources;                                              // grouped by faction
    std::unordered_map<std::uint32_t, std::pair<std::size_t, std::size_t>> Range;   // faction -> [begin, end) in Sources
};

std::vector<Target> ParseTargets(std::string_view text, std::vector<std::string>& errors);
// Sources: "q|faction|quest|points|level|side|kind|cap" and "k|faction|creature|points|cap|map|x|y|z|spawns"; a bad line is reported and skipped.
std::vector<Source> ParseSources(std::string_view text, std::vector<std::string>& errors);
// Groups the sources by faction and drops those of factions without a target.
PlanData BuildPlanData(std::vector<Target> targets, std::vector<Source> sources);

struct PlanSettings
{
    bool Enabled = false;                    // ReputationPlan
    std::uint32_t Budget = 64;               // sources looked at per call (ReputationPlanBudget)
    std::uint64_t ReplanMs = 120000;         // least time between two plans of one bot (ReputationPlanSec)
    std::uint64_t StallMs = 1800000;         // a source that raised the standing by nothing in this time is given up (ReputationStallSec)
};

constexpr std::size_t kSkipMemory = 8;       // sources given up, remembered per bot (the oldest is forgotten)

enum class Phase : std::uint8_t { kIdle, kActive };

// Per-bot memory of the plan.
struct Track
{
    std::uint32_t Faction = 0;
    Phase State = Phase::kIdle;
    std::uint32_t SourceId = 0;
    Source::Kind SourceKind = Source::kQuest;
    std::int32_t StartStanding = 0;
    std::uint64_t StartedMs = 0;
    std::uint64_t NextPlanMs = 0;
    std::uint32_t Cursor = 0;                // where the next scan starts, so a faction with many sources is covered over several plans
    std::array<std::uint64_t, kSkipMemory> Skipped{};   // (kind << 32 | id) + 1; 0 = empty
    std::uint32_t SkipNext = 0;
};

// What the bot should do now (Valid false: nothing planned).
struct Step
{
    bool Valid = false;
    bool Changed = false;                    // a new plan this call (journal it)
    Source const* Src = nullptr;
    std::uint32_t Faction = 0;
};

// The target: a faction of the bot's side and level, below its goal rank, with at least one source; the lowest tier first, ties spread by a hash of the guid;
// `current` is kept while it is still a candidate. nullopt: nothing to work on.
std::optional<std::size_t> ChooseTarget(PlanData const& data, Facts const& facts, std::uint64_t guid, std::uint32_t current);

// One planner call for one bot. Cheap when nothing is due (two comparisons); a plan costs at most Budget source checks. `examined` (optional) adds the number of
// sources looked at, for the cost measurement.
Step Advance(PlanData const& data, PlanSettings const& settings, Track& track, Facts const& facts, std::uint64_t guid, std::uint64_t nowMs,
             std::uint32_t* examined = nullptr);

} // namespace reputation
