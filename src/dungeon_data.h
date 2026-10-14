// Dungeon routes (M9): a data file describes dungeons (where to enter, the party, the bosses, the route), and the module runs one as a
// scenario (scenario.h). This library parses the file and turns a chosen dungeon into the scenario's config keys; the director, the
// tank-led pulls, healing, rest, loot and corpse run are the existing scenario and policy code. No SDK dependency. Format: doc/dungeons.md.
//
//   dungeon rfc                      # starts a block; the id is what Dungeon = <id> picks
//   name Ragefire Chasm
//   levels 13 18
//   roster 2:1,5:5,8:8,2:9,5:4       # race:class of the tank, healer and three dps
//   staging 1 1629 -4373 31          # map x y z: where the bots log in
//   portal 1 1811 -4410 -18          # map x y z: the instance door on the outside map (for a corpse run)
//   entry 389 3.81 -14.82 -17.84     # map x y z: where the party is teleported to
//   duration 1500                    # seconds   (optional, default 1800)
//   wipe_recovery 300                # seconds a wiped party may take to get a bot back (optional, 0 = a wipe ends the run)
//   min_alive 2                      # optional: give up when fewer are alive for a while
//   leader_dead_abandon 600          # optional, seconds
//   go x y z                         # a route waypoint, in order
//   boss 11517 x y z Oggleflint      # a waypoint that is also a boss: creature entry, position, name
//   end
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct DungeonWaypoint
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    std::uint32_t BossEntry = 0; // 0: a plain waypoint
    std::string BossName;
};

struct Dungeon
{
    std::string Id;
    std::string Name;
    std::uint32_t MinLevel = 1;
    std::uint32_t MaxLevel = 60;
    std::string Roster;                                  // "race:class,..." as ScenarioRoster
    std::optional<std::array<float, 4>> Staging;         // map x y z
    std::optional<std::array<float, 4>> Portal;
    std::array<float, 4> Entry{};                        // map x y z
    std::uint32_t DurationSec = 1800;
    std::uint32_t WipeRecoverySec = 0;
    std::uint32_t MinAlive = 0;
    std::uint32_t LeaderDeadAbandonSec = 0;
    std::vector<DungeonWaypoint> Route;
};

// Parses a dungeon file. A block with a problem (no entry, no route, a bad number, a roster that is not race:class) is dropped with a line
// in `warnings`; the other blocks are kept. Unknown keys are warned about and ignored.
std::vector<Dungeon> ParseDungeons(std::string_view text, std::vector<std::string>& warnings);

// If `config` names a dungeon (key Dungeon) that `text` defines, adds the scenario keys the config does not already set (Scenario,
// ScenarioRoster, ScenarioLevel, ScenarioStaging, ScenarioPortal, ScenarioEntry, ScenarioDurationSec, ScenarioWipeRecovery*, ScenarioMinAlive,
// ScenarioLeaderDeadAbandonSec, ScenarioBosses, ScenarioRequireBosses and ScenarioRoute<n>) and returns the new config. The route keys are
// added only when the config has no ScenarioRoute<n> of its own. Explicit config always wins. Without a match the config comes back unchanged.
std::unordered_map<std::string, std::string> MergeDungeon(std::unordered_map<std::string, std::string> const& config, std::string_view text,
                                                          std::vector<std::string>& warnings);

// MergeDungeon with the text read from the file named by the config key DungeonFile (relative to the working directory). An unreadable file
// is a warning and the config comes back unchanged. Both Dungeon and DungeonFile must be set, else nothing happens.
std::unordered_map<std::string, std::string> MergeDungeonFile(std::unordered_map<std::string, std::string> const& config,
                                                              std::vector<std::string>& warnings);
