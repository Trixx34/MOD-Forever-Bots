// SPDX-License-Identifier: GPL-2.0-or-later
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

#include "dungeon_events.h"

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

// What a dungeon's boss block says about the fight (doc/dungeon-strategy.md, bosses[].strategy). The module does not act on any of these yet: they are
// read, checked and kept, so a file written today keeps working when the code starts to use them. (avoid and avoid_spells are acted on since bots 0.20: see
// doc/dungeon-strategy.md.)
struct AvoidZone
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    float Radius = 0.0f;
    std::string Note;
};

struct BossStrategy
{
    std::string Pull;                                    // "" = not set
    std::optional<std::array<float, 3>> TankPosition;
    std::vector<std::uint32_t> Interrupt;                // spell ids to interrupt
    std::vector<std::uint32_t> Dispel;                   // spell ids to dispel
    std::vector<AvoidZone> Avoid;                        // fixed places to stay out of (acted on since bots 0.20, with AvoidGroundEffects)
    std::vector<std::uint32_t> AvoidSpells;              // spells whose ground effect to step out of (bots 0.20)
    std::uint32_t KillOrder = 0;                         // 0 = not set
    std::string Notes;
};

struct DungeonBoss
{
    std::uint32_t Entry = 0;
    std::string Name;
    BossStrategy Strategy;
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
    // Creature entries in the order the party should kill them (casters and healers first): the damage dealers of a party fight the one of these that is
    // next to the tank's target before the tank's own target (kill_priority in the JSON file). Empty: they fight the tank's target.
    std::vector<std::uint32_t> KillPriority;
    // Filled by the JSON dungeon files (dungeon_files.h); empty for a block of the old text file.
    std::vector<DungeonBoss> Bosses;
    std::vector<dungeon_events::Event> Events;
    std::string Source;                                  // the file the dungeon came from (for the log)

    std::uint32_t MapId() const { return std::uint32_t(Entry[0]); }
};

// "race:class" pairs, 1 to 5, separated by commas.
bool DungeonRosterOk(std::string const& roster);

// Parses a dungeon file. A block with a problem (no entry, no route, a bad number, a roster that is not race:class) is dropped with a line
// in `warnings`; the other blocks are kept. Unknown keys are warned about and ignored.
std::vector<Dungeon> ParseDungeons(std::string_view text, std::vector<std::string>& warnings);

// If `config` names a dungeon (key Dungeon) that `text` defines, adds the scenario keys the config does not already set (Scenario,
// ScenarioRoster, ScenarioLevel, ScenarioStaging, ScenarioPortal, ScenarioEntry, ScenarioDurationSec, ScenarioWipeRecovery*, ScenarioMinAlive,
// ScenarioLeaderDeadAbandonSec, ScenarioBosses, ScenarioRequireBosses and ScenarioRoute<n>) and returns the new config. The route keys are
// added only when the config has no ScenarioRoute<n> of its own. Explicit config always wins. Without a match the config comes back unchanged.
// With a list in Scenario ("dm:10,rfc:10", bot_settings.h) each listed dungeon is merged the same way into the keys it reads (ScenarioConfigFor), and the keys the
// file adds are stored as "<key>.<id>" (ScenarioEntry.dm, ScenarioRoute3.rfc); Dungeon is not needed then.
std::unordered_map<std::string, std::string> MergeDungeon(std::unordered_map<std::string, std::string> const& config, std::string_view text,
                                                          std::vector<std::string>& warnings);

// MergeDungeon for dungeons that are loaded already (from the text file, the JSON files, or both: dungeon_files.h).
std::unordered_map<std::string, std::string> MergeDungeonSet(std::unordered_map<std::string, std::string> const& config, std::vector<Dungeon> const& dungeons,
                                                             std::vector<std::string>& warnings);

// MergeDungeonSet with the dungeons loaded from the places the config names: DungeonDir (your own JSON files), DungeonDataDir (the shipped JSON files) and
// DungeonFile (the old text file); see LoadDungeonSet in dungeon_files.h. Unreadable places are warnings. Dungeon (or a list in Scenario) must be set and at least one
// of the three places named, else nothing happens and the config comes back unchanged.
std::unordered_map<std::string, std::string> MergeDungeonFile(std::unordered_map<std::string, std::string> const& config,
                                                              std::vector<std::string>& warnings);
