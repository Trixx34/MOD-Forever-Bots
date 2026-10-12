// BotSettings: the module's own settings, read from its ModuleConfig (the core sends Module.forever-bots.<Key> with the
// prefix removed; see modules.d/forever-bots.conf.example). Pure logic, unit-tested in tests/settings_test.cpp.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct BotSettings
{
    // Most bots this module controls at once. Never more than kMaxBotsCeiling (the Tier 2 request is sized to it).
    std::uint32_t MaxBots = 1;
    std::uint32_t Race = 1;   // Human
    std::uint32_t Class = 1;  // Warrior
    // Where the module keeps the guids of the bots it controls, so a restart can take them back.
    std::string StateFile = "forever-bots.state";
};

// The most bots any configuration may ask for; also the Tier 2 bot bound requested at handshake.
constexpr std::uint32_t kMaxBotsCeiling = 16;

// The number of bots the module may control: the configured MaxBots, capped at the Tier 2 bot grant the core returned at
// handshake. Without a Tier 2 region the module cannot drive bots, so it asks for none.
std::uint32_t EffectiveMaxBots(std::uint32_t configured, std::uint32_t grantedMaxBots, bool tier2Granted);

// Reads the settings from the config the core delivered. Missing or invalid values fall back to the defaults, and each
// fallback adds one line to `warnings` so the operator can see it in the module's log.
BotSettings ParseBotSettings(std::unordered_map<std::string, std::string> const& config, std::vector<std::string>& warnings);
