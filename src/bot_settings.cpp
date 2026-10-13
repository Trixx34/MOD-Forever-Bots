#include "bot_settings.h"

#include "bot_pick.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <sstream>
#include <system_error>
#include <tuple>

namespace
{

// Parses an unsigned decimal in [min, max]. Returns false, with the reason, if the text is not one.
bool ParseBounded(std::string const& text, std::uint32_t min, std::uint32_t max, std::uint32_t& value, std::string& reason)
{
    std::uint64_t parsed = 0;
    auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc() || end != text.data() + text.size())
    {
        reason = "'" + text + "' is not a whole number";
        return false;
    }
    if (parsed < min || parsed > max)
    {
        reason = "'" + text + "' is outside " + std::to_string(min) + ".." + std::to_string(max);
        return false;
    }
    value = static_cast<std::uint32_t>(parsed);
    return true;
}

// Reads one key. Keeps `value` at its default and records a warning if the key is present but invalid.
void ReadKey(std::unordered_map<std::string, std::string> const& config, char const* key, std::uint32_t min, std::uint32_t max,
             std::uint32_t& value, std::vector<std::string>& warnings)
{
    auto const it = config.find(key);
    if (it == config.end())
        return;

    std::uint32_t parsed = value;
    std::string reason;
    if (ParseBounded(it->second, min, max, parsed, reason))
        value = parsed;
    else
        warnings.push_back(std::string("Module.forever-bots.") + key + ": " + reason + "; using " + std::to_string(value) + ".");
}

// Reads Hub<map>_<n> = "x y z" for every map, in order n. A key or value that does not fit is dropped with a warning.
void ReadHubs(std::unordered_map<std::string, std::string> const& config,
              std::map<std::uint32_t, std::vector<std::array<float, 3>>>& hubs, std::vector<std::string>& warnings)
{
    std::vector<std::tuple<std::uint32_t, std::uint32_t, std::array<float, 3>>> found; // map, n, spot
    for (auto const& [key, value] : config)
    {
        if (key.rfind("Hub", 0) != 0)
            continue;

        std::string const body = key.substr(3);
        auto const sep = body.find('_');
        std::uint32_t mapId = 0;
        std::uint32_t n = 0;
        bool const keyOk = sep != std::string::npos && sep > 0 && sep + 1 < body.size() &&
                           std::from_chars(body.data(), body.data() + sep, mapId).ec == std::errc{} &&
                           std::from_chars(body.data() + sep + 1, body.data() + body.size(), n).ec == std::errc{} && n > 0;
        if (!keyOk)
        {
            warnings.push_back("Module.forever-bots." + key + ": the key is not Hub<map>_<n>; ignored.");
            continue;
        }

        std::istringstream in(value);
        std::array<float, 3> spot{};
        std::string trailing;
        if (!(in >> spot[0] >> spot[1] >> spot[2]) || (in >> trailing))
            warnings.push_back("Module.forever-bots." + key + ": '" + value + "' is not three numbers; ignored.");
        else
            found.emplace_back(mapId, n, spot);
    }

    std::sort(found.begin(), found.end(), [](auto const& a, auto const& b)
    {
        return std::get<0>(a) != std::get<0>(b) ? std::get<0>(a) < std::get<0>(b) : std::get<1>(a) < std::get<1>(b);
    });
    for (auto const& [mapId, n, spot] : found)
        hubs[mapId].push_back(spot);
}

// Reads a race or class id. The word "random" means pick (0); otherwise a valid id.
void ReadIdOrRandom(std::unordered_map<std::string, std::string> const& config, char const* key, std::uint32_t& value,
                    std::vector<std::string>& warnings)
{
    auto const it = config.find(key);
    if (it == config.end())
        return;

    if (it->second == "random")
    {
        value = pick::kRandom;
        return;
    }
    // Race and class ids are validated by the core when it creates the character; here only the range is checked.
    ReadKey(config, key, 1, std::numeric_limits<std::uint8_t>::max(), value, warnings);
}

// Reads a clock time for the ramp's stop. Keeps the default and warns if it is not HH:MM or HH:MM:SS.
void ReadStopTime(std::unordered_map<std::string, std::string> const& config, std::uint32_t& stopSecOfDay,
                  std::vector<std::string>& warnings)
{
    auto const it = config.find("BotsStopTime");
    if (it == config.end())
        return;

    if (auto const parsed = ParseClock(it->second))
        stopSecOfDay = *parsed;
    else
        warnings.push_back("Module.forever-bots.BotsStopTime: '" + it->second + "' is not HH:MM; using the default.");
}

} // namespace

std::uint32_t EffectiveMaxBots(std::uint32_t configured, std::uint32_t grantedMaxBots, bool tier2Granted)
{
    if (!tier2Granted)
        return 0;
    return std::min(configured, grantedMaxBots);
}

BotSettings ParseBotSettings(std::unordered_map<std::string, std::string> const& config, std::vector<std::string>& warnings)
{
    BotSettings settings;
    ReadKey(config, "MaxBots", 1, kMaxBotsCeiling, settings.MaxBots, warnings);
    ReadIdOrRandom(config, "Race", settings.Race, warnings);
    ReadIdOrRandom(config, "Class", settings.Class, warnings);
    ReadKey(config, "RandomSeed", 0, std::numeric_limits<std::uint32_t>::max(), settings.RandomSeed, warnings);
    ReadKey(config, "TestSpell", 0, std::numeric_limits<std::int32_t>::max(), settings.TestSpell, warnings);

    ReadKey(config, "BotsInitial", 0, kMaxBotsCeiling, settings.Ramp.Initial, warnings);
    ReadKey(config, "BotsStep", 0, kMaxBotsCeiling, settings.Ramp.Step, warnings);
    ReadKey(config, "BotsStepMinutes", 1, 1440, settings.Ramp.StepMinutes, warnings);
    ReadKey(config, "BotsCap", 1, kMaxBotsCeiling, settings.Ramp.Cap, warnings);
    ReadStopTime(config, settings.Ramp.StopSecOfDay, warnings);

    if (auto const it = config.find("TelemetryFile"); it != config.end())
        settings.TelemetryFile = it->second;
    ReadKey(config, "TelemetryMaxMB", 1, 10240, settings.TelemetryMaxMB, warnings);
    ReadKey(config, "TelemetryKeep", 0, 50, settings.TelemetryKeep, warnings);
    ReadKey(config, "TelemetrySummarySec", 1, 3600, settings.TelemetrySummarySec, warnings);
    ReadKey(config, "ServiceEverySec", 60, 86400, settings.ServiceEverySec, warnings);
    if (auto const train = config.find("TrainEverySec"); train != config.end() && train->second == "0")
        settings.TrainEverySec = 0; // explicitly off
    else
        ReadKey(config, "TrainEverySec", 60, 86400, settings.TrainEverySec, warnings);
    ReadKey(config, "PartySize", 0, 5, settings.PartySize, warnings);
    ReadKey(config, "PartyCount", 1, 100, settings.PartyCount, warnings);
    ReadHubs(config, settings.Hubs, warnings);
    if (auto const it = config.find("QuestFile"); it != config.end())
        settings.QuestFile = it->second;
    return settings;
}
