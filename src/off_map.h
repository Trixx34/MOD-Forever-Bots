// SPDX-License-Identifier: GPL-2.0-or-later
// Off-map recovery: a bot that stands far outside the part of its map the module knows (its quest givers, quest anchors, grind spots and hubs) is
// lost: it was put there by a GM teleport, a follow or a summon of a player who has since left, and nothing in the module ever sends it anywhere it can
// walk from (prod 2026-11-14: Jorordan, level 12 Skyborne, on map 2991 at (954, -1574), about 800 yards west and 1900 yards south of Zephras Isle, zone 0).
// After OffForMs off the map, the bot is teleported to the nearest known place of its map for its faction (kTeleport, needs Bot.Actions.Teleport in
// the core); a try is not repeated before CooldownMs.
//
// The snapshot carries no zone or area id, so "off the map" is the module's own test: the map must have at least kMinPlaces known places (so an instance
// or a map the data does not cover is never judged), and the bot must stand more than MarginYards outside the box around them. A bot in the sea between
// two continents' coasts is inside the box and is not caught; a bot thousands of yards off an island is.
//
// Pure logic, unit-tested in tests/off_map_test.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace off_map
{

struct Place
{
    std::uint32_t Map = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    std::uint8_t Side = 0;   // 0 both factions, 1 Alliance, 2 Horde
};

// A map with fewer known places than this is never judged (instances, battlegrounds, maps the data does not cover).
constexpr std::size_t kMinPlaces = 10;

class KnownWorld
{
public:
    void Add(Place const& place);
    // True when the map has at least kMinPlaces places.
    bool Knows(std::uint32_t map) const;
    // True when (x, y) lies more than `margin` yards outside the box around the map's places. False for a map it does not know.
    bool Outside(std::uint32_t map, float x, float y, float margin) const;
    // The nearest place of the map that a bot of `side` may use (Side 0 or `side`; side 0 takes any). Null when the map has none.
    Place const* Home(std::uint32_t map, float x, float y, std::uint8_t side) const;
    // The places of any side within `radius` (2D) of (x, y): the lowest of them (null when none) and the smallest height difference to `z` (below-map
    // recovery: the lowest near place stands for the ground, so a giver on a tower or a tree platform beside a lower place does not count as the ground).
    struct Around
    {
        Place const* Lowest = nullptr;
        float NearestDz = 1e9f;
        std::vector<Place const*> Within;   // every place within the radius (below_map::FindGround picks the closest floor of them)
    };
    Around Near(std::uint32_t map, float x, float y, float z, float radius) const;
    std::size_t PlacesOn(std::uint32_t map) const;

private:
    struct MapPlaces
    {
        std::vector<Place> Places;
        float MinX = 0.0f, MaxX = 0.0f, MinY = 0.0f, MaxY = 0.0f;
    };
    std::map<std::uint32_t, MapPlaces> _maps;
};

struct Options
{
    bool Enabled = false;                 // off here (the policy tests); the module setting OffMapRecover defaults to on
    std::uint64_t OffForMs = 60000;       // off the map this long (continuously) before the bot is sent home
    std::uint64_t CooldownMs = 600000;    // no second try for the same bot before this
    float MarginYards = 500.0f;           // how far outside the box of known places counts as off the map
};

// What the policy knows about the bot this tick.
struct Facts
{
    std::uint32_t Map = 0;
    float X = 0.0f;
    float Y = 0.0f;
    bool Alive = true;        // life_state 0 (a corpse or ghost has its own recovery)
    bool InCombat = false;    // the core refuses a teleport in combat
    bool Travelling = false;  // on a flight or a transport: the route may pass outside the box
    bool Held = false;        // the policy holds the bot (a scenario, a dungeon run, a teleport just ordered)
};

// Per bot.
struct Watch
{
    std::uint64_t OffSinceMs = 0;   // when the bot was first seen off the map in this spell (0 = on the map)
    std::uint64_t NextTryMs = 0;    // no teleport before this
    std::uint32_t Tries = 0;        // teleports sent while it stayed off the map
    bool Logged = false;            // the off_map_seen line of this spell was written
};

enum class Verdict : std::uint8_t
{
    kOnMap,       // inside the box, or a map it does not know: the watch is reset
    kTravelling,  // on a flight or transport: the watch is reset
    kWaiting,     // off the map, not long enough yet
    kBlocked,     // off the map long enough, but dead, fighting or held: tried when that ends
    kCooldown,    // off the map, a try was made and the cooldown runs
    kSend,        // teleport now (Check set NextTryMs and counted the try)
};

Verdict Check(Options const& options, KnownWorld const& world, Watch& watch, Facts const& facts, std::uint64_t nowMs);

char const* VerdictName(Verdict verdict);

} // namespace off_map
