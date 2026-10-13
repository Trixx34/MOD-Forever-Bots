// BotSettings: the module's own settings, read from its ModuleConfig (the core sends Module.forever-bots.<Key> with the
// prefix removed; see modules.d/forever-bots.conf.example). Pure logic, unit-tested in tests/settings_test.cpp.
#pragma once

#include "bot_ramp.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

struct BotSettings
{
    // Most bots this module controls at once. Never more than kMaxBotsCeiling (the Tier 2 request is sized to it).
    std::uint32_t MaxBots = 1;
    // Race and class ids, or 0 for "random" (config value random): a random valid combination, see bot_pick.h.
    std::uint32_t Race = 1;   // Human
    std::uint32_t Class = 1;  // Warrior
    // Seed for the random picks. The same seed gives the same sequence of bots.
    std::uint32_t RandomSeed = 1;
    // Spell cast at the attack target on a timer (0 = off). A test of the cast path and its refusal reports, until the
    // class rotations exist. Config key TestSpell.
    std::uint32_t TestSpell = 0;
    // How many bots the ramp wants over the run (config keys BotsInitial, BotsStep, BotsStepMinutes, BotsCap, BotsStopTime).
    RampPlan Ramp;
    // The telemetry CSV the bots write to (config key TelemetryFile, relative to the module's working directory). Empty
    // turns it off. The file is rotated at TelemetryMaxMB, keeping TelemetryKeep older files.
    std::string TelemetryFile = "forever-bots-telemetry.csv";
    std::uint32_t TelemetryMaxMB = 64;
    std::uint32_t TelemetryKeep = 5;
    // Seconds between summary lines per bot (config key TelemetrySummarySec, 1..3600).
    std::uint32_t TelemetrySummarySec = 60;
    // Seconds between vendor visits (sell grey, repair, equip) and between trainer checks, for an idle bot. A level-up
    // triggers a trainer check at once. Config keys ServiceEverySec and TrainEverySec (60..86400).
    std::uint32_t ServiceEverySec = 600;
    std::uint32_t TrainEverySec = 600;
    // Parties (M8): PartySize bots per party (config key PartySize, 0 = no parties, at most 5) and PartyCount parties,
    // cut from the lowest-guid bots (config key PartyCount, 1..100).
    std::uint32_t PartySize = 0;
    std::uint32_t PartyCount = 1;
    // Where each race's vendor stands in its start city (M6, config keys VendorPos<race> = "x y z"). A bot due a vendor
    // visit walks there. A race with no entry does not travel.
    std::map<std::uint32_t, std::array<float, 3>> VendorSpots;
};

// The most bots any configuration may ask for; also the Tier 2 bot bound requested at handshake.
constexpr std::uint32_t kMaxBotsCeiling = 1500;

// The number of bots the module may control: the configured MaxBots, capped at the Tier 2 bot grant the core returned at
// handshake. Without a Tier 2 region the module cannot drive bots, so it asks for none.
std::uint32_t EffectiveMaxBots(std::uint32_t configured, std::uint32_t grantedMaxBots, bool tier2Granted);

// Reads the settings from the config the core delivered. Missing or invalid values fall back to the defaults, and each
// fallback adds one line to `warnings` so the operator can see it in the module's log.
BotSettings ParseBotSettings(std::unordered_map<std::string, std::string> const& config, std::vector<std::string>& warnings);
