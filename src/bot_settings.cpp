#include "bot_settings.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <system_error>

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
    // Race and class ids are validated by the core when it creates the character; here only the range is checked.
    ReadKey(config, "Race", 1, std::numeric_limits<std::uint8_t>::max(), settings.Race, warnings);
    ReadKey(config, "Class", 1, std::numeric_limits<std::uint8_t>::max(), settings.Class, warnings);

    if (auto const it = config.find("StateFile"); it != config.end())
    {
        if (it->second.empty())
            warnings.push_back("Module.forever-bots.StateFile is empty; using " + settings.StateFile + ".");
        else
            settings.StateFile = it->second;
    }
    return settings;
}
