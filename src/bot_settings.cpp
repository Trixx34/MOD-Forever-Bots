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


bool ReadBool(std::unordered_map<std::string, std::string> const& config, char const* key, bool& value, std::vector<std::string>& warnings)
{
    auto const it = config.find(key);
    if (it == config.end())
        return false;
    if (it->second == "1" || it->second == "true")
        value = true;
    else if (it->second == "0" || it->second == "false")
        value = false;
    else
        warnings.push_back(std::string("Module.forever-bots.") + key + ": '" + it->second + "' is not 0 or 1; using the default.");
    return true;
}

// "a b c ..." into exactly `count` numbers.
bool ParseNumbers(std::string const& text, float* out, std::size_t count)
{
    std::istringstream in(text);
    std::string trailing;
    for (std::size_t i = 0; i < count; ++i)
        if (!(in >> out[i]))
            return false;
    return !(in >> trailing);
}

void ReadScenario(std::unordered_map<std::string, std::string> const& config, ScenarioSettings& scenario, std::vector<std::string>& warnings)
{
    auto const name = config.find("Scenario");
    if (name == config.end() || name->second.empty())
        return;
    scenario.Name = name->second;

    if (auto const roster = config.find("ScenarioRoster"); roster != config.end())
    {
        std::istringstream list(roster->second);
        std::string item;
        while (std::getline(list, item, ','))
        {
            auto const colon = item.find(':');
            std::uint32_t race = 0, cls = 0;
            if (colon != std::string::npos && std::from_chars(item.data(), item.data() + colon, race).ec == std::errc{} &&
                std::from_chars(item.data() + colon + 1, item.data() + item.size(), cls).ec == std::errc{} && race > 0 && cls > 0)
                scenario.Roster.push_back({ race, cls });
            else
                warnings.push_back("Module.forever-bots.ScenarioRoster: '" + item + "' is not race:class; ignored.");
        }
    }
    if (scenario.Roster.empty())
    {
        warnings.push_back("Module.forever-bots.Scenario: the roster is empty; the scenario is off.");
        scenario.Name.clear();
        return;
    }
    if (scenario.Roster.size() > 5)
        scenario.Roster.resize(5);

    ReadKey(config, "ScenarioLevel", 1, 60, scenario.Level, warnings);
    if (auto const it = config.find("ScenarioKillPriority"); it != config.end())
    {
        std::istringstream list(it->second);
        std::string item;
        while (std::getline(list, item, ','))
        {
            std::uint32_t entry = 0;
            if (std::from_chars(item.data(), item.data() + item.size(), entry).ec == std::errc{} && entry != 0)
                scenario.KillPriority.push_back(entry);
            else
                warnings.push_back("Module.forever-bots.ScenarioKillPriority: '" + item + "' is not a creature entry; ignored.");
        }
    }
    ReadKey(config, "ScenarioParties", 1, 8, scenario.Parties, warnings);
    ReadKey(config, "ScenarioDurationSec", 30, 86400, scenario.DurationSec, warnings);
    if (auto const it = config.find("ScenarioStaging"); it != config.end())
        scenario.HasStaging = ParseNumbers(it->second, scenario.Staging.data(), 4);
    if (auto const it = config.find("ScenarioEntry"); it != config.end())
        scenario.HasEntry = ParseNumbers(it->second, scenario.Entry.data(), 4);
    if (!scenario.HasEntry)
        warnings.push_back("Module.forever-bots.ScenarioEntry: missing or not four numbers (map x y z); the scenario cannot enter its map.");
    if (auto const it = config.find("ScenarioBosses"); it != config.end())
    {
        std::istringstream list(it->second);
        std::string item;
        while (std::getline(list, item, ','))
        {
            std::size_t const colon = item.find(':');
            std::string const number = item.substr(0, colon);
            std::uint32_t entry = 0;
            if (std::from_chars(number.data(), number.data() + number.size(), entry).ec == std::errc{} && entry > 0)
            {
                std::string name = colon == std::string::npos ? std::to_string(entry) : item.substr(colon + 1);
                scenario.Bosses.emplace_back(entry, name);
            }
            else
                warnings.push_back("Module.forever-bots.ScenarioBosses: '" + item + "' is not entry[:name]; ignored.");
        }
    }
    if (auto const it = config.find("ScenarioBossAt"); it != config.end())
    {
        std::istringstream list(it->second);
        std::string item;
        while (std::getline(list, item, ','))
        {
            std::size_t const colon = item.find(':');
            std::uint32_t point = 0;
            std::uint32_t entry = 0;
            if (colon != std::string::npos && std::from_chars(item.data(), item.data() + colon, point).ec == std::errc{} && point > 0 &&
                std::from_chars(item.data() + colon + 1, item.data() + item.size(), entry).ec == std::errc{} && entry > 0)
                scenario.BossAt.emplace_back(point - 1, entry);
            else
                warnings.push_back("Module.forever-bots.ScenarioBossAt: '" + item + "' is not point:entry; ignored.");
        }
    }
    if (auto const it = config.find("ScenarioResultFile"); it != config.end())
        scenario.ResultFile = it->second;
    // Dead-bot recovery. The portal is the instance door on the outside map; without it a ghost released outside cannot reach a corpse inside.
    ReadBool(config, "ScenarioWipeRecovery", scenario.WipeRecovery, warnings);
    ReadBool(config, "ScenarioRequireBosses", scenario.RequireBosses, warnings);
    ReadKey(config, "ScenarioWipeRecoverySec", 10, 3600, scenario.WipeRecoverySec, warnings);
    ReadKey(config, "ScenarioMinAlive", 0, 5, scenario.MinAlive, warnings);
    ReadKey(config, "ScenarioLeaderDeadAbandonSec", 0, 3600, scenario.LeaderDeadAbandonSec, warnings);
    if (auto const it = config.find("ScenarioPortal"); it != config.end())
    {
        scenario.HasPortal = ParseNumbers(it->second, scenario.Portal.data(), 4);
        if (!scenario.HasPortal)
            warnings.push_back("Module.forever-bots.ScenarioPortal: '" + it->second + "' is not four numbers (map x y z); ignored.");
    }

    std::vector<std::pair<std::uint32_t, std::array<float, 3>>> route;
    for (auto const& [key, value] : config)
    {
        if (key.rfind("ScenarioRoute", 0) == 0)
        {
            std::uint32_t n = 0;
            std::array<float, 3> point{};
            if (std::from_chars(key.data() + 13, key.data() + key.size(), n).ec == std::errc{} && n > 0 && ParseNumbers(value, point.data(), 3))
                route.emplace_back(n, point);
            else
                warnings.push_back("Module.forever-bots." + key + ": not ScenarioRoute<n> = \"x y z\"; ignored.");
        }
        else if (key == "ScenarioSupplies")
        {
            std::istringstream list(value);
            std::string item;
            while (std::getline(list, item, ','))
            {
                std::size_t const colon = item.find(':');
                std::uint32_t entry = 0, count = 0;
                if (colon != std::string::npos && std::from_chars(item.data(), item.data() + colon, entry).ec == std::errc{} && entry > 0 &&
                    std::from_chars(item.data() + colon + 1, item.data() + item.size(), count).ec == std::errc{} && count > 0)
                    scenario.Supplies.emplace_back(entry, count);
                else
                    warnings.push_back("Module.forever-bots.ScenarioSupplies: '" + item + "' is not item:count; ignored.");
            }
        }
        else if (key.rfind("ScenarioGear", 0) == 0)
        {
            std::uint32_t classId = 0;
            if (std::from_chars(key.data() + 12, key.data() + key.size(), classId).ec != std::errc{} || classId == 0)
            {
                warnings.push_back("Module.forever-bots." + key + ": not ScenarioGear<classId>; ignored.");
                continue;
            }
            std::istringstream list(value);
            std::string item;
            while (std::getline(list, item, ','))
            {
                std::uint32_t entry = 0;
                if (std::from_chars(item.data(), item.data() + item.size(), entry).ec == std::errc{} && entry > 0)
                    scenario.Gear[classId].push_back(entry);
            }
        }
    }
    std::sort(route.begin(), route.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
    for (auto const& [n, point] : route)
        scenario.Route.push_back(point);
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
    if (auto const it = config.find("LiveFile"); it != config.end())
        settings.LiveFile = it->second;
    ReadKey(config, "LiveEveryMs", 250, 10000, settings.LiveEveryMs, warnings);
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
    ReadKey(config, "PartyPercent", 0, 100, settings.PartyPercent, warnings);
    ReadKey(config, "RandomPct", 0, 90, settings.RandomPct, warnings);
    ReadHubs(config, settings.Hubs, warnings);
    ReadBool(config, "AutoGearing", settings.AutoGearing, warnings);
    ReadBool(config, "QuestTeleport", settings.QuestTeleport, warnings);
    ReadBool(config, "ChatCommands", settings.ChatCommands, warnings);
    ReadBool(config, "ChatOrders", settings.ChatOrders, warnings);
    ReadBool(config, "ChatControl", settings.ChatControl, warnings);
    ReadBool(config, "ChatSay", settings.ChatSay, warnings);
    ReadKey(config, "ChatSayMax", 0, 50, settings.ChatSayMax, warnings);
    ReadBool(config, "Supplies", settings.Supplies.Enabled, warnings);
    ReadKey(config, "SupplyAmmoLow", 0, 1000, settings.Supplies.AmmoLow, warnings);
    ReadKey(config, "SupplyAmmoTarget", 1, 2000, settings.Supplies.AmmoTarget, warnings);
    ReadKey(config, "SupplyFoodLow", 0, 100, settings.Supplies.FoodLow, warnings);
    ReadKey(config, "SupplyFoodTarget", 1, 200, settings.Supplies.FoodTarget, warnings);
    ReadKey(config, "SupplyDrinkLow", 0, 100, settings.Supplies.DrinkLow, warnings);
    ReadKey(config, "SupplyDrinkTarget", 1, 200, settings.Supplies.DrinkTarget, warnings);
    ReadBool(config, "SupplyPotions", settings.Supplies.Potions, warnings);
    ReadBool(config, "SupplyBandages", settings.Supplies.Bandages, warnings);
    ReadBool(config, "SupplyReagents", settings.Supplies.Reagents, warnings);
    ReadKey(config, "SupplyHealthPotionLow", 0, 20, settings.Supplies.HealthPotionLow, warnings);
    ReadKey(config, "SupplyManaPotionLow", 0, 20, settings.Supplies.ManaPotionLow, warnings);
    ReadKey(config, "SupplyBandageLow", 0, 50, settings.Supplies.BandageLow, warnings);
    ReadKey(config, "UseHealthPotionBelowPct", 1, 90, settings.Supplies.UseHealthPotionBelowPct, warnings);
    ReadKey(config, "UseManaPotionBelowPct", 1, 90, settings.Supplies.UseManaPotionBelowPct, warnings);
    ReadKey(config, "UseBandageBelowPct", 1, 95, settings.Supplies.UseBandageBelowPct, warnings);
    {
        std::uint32_t tripSec = static_cast<std::uint32_t>(settings.Supplies.TripEveryMs / 1000);
        ReadKey(config, "SupplyTripSec", 30, 3600, tripSec, warnings);
        settings.Supplies.TripEveryMs = std::uint64_t(tripSec) * 1000;
    }
    ReadBool(config, "DungeonTeleport", settings.DungeonTeleport, warnings);
    ReadBool(config, "DungeonReviveOutOfCombat", settings.DungeonReviveOutOfCombat, warnings);
    if (auto const revive = config.find("CorpseRunReviveSec"); revive != config.end() && revive->second == "0")
        settings.CorpseRunReviveSec = 0; // explicitly off: the ghost walks, and is put at its corpse after CorpseRunTimeoutMs as before
    else
        ReadKey(config, "CorpseRunReviveSec", 10, 3600, settings.CorpseRunReviveSec, warnings);
    ReadKey(config, "DungeonHealBelowPct", 30, 85, settings.DungeonHealBelowPct, warnings);
    ReadKey(config, "DungeonTankHealBelowPct", 40, 95, settings.DungeonTankHealBelowPct, warnings);
    ReadKey(config, "HealerDpsManaPct", 10, 80, settings.HealerDpsManaPct, warnings);
    ReadKey(config, "HealerLosMoveYards", 3, 12, settings.HealerLosMoveYards, warnings);
    ReadBool(config, "AggressiveStart", settings.AggressiveStart, warnings);
    if (auto const it = config.find("DungeonTactics"); it != config.end())
    {
        settings.TacticFocus = settings.TacticInterrupt = settings.TacticHeal = settings.TacticRest = settings.TacticRecover = false;
        std::istringstream list(it->second);
        std::string item;
        while (std::getline(list, item, ','))
        {
            while (!item.empty() && item.front() == ' ')
                item.erase(item.begin());
            while (!item.empty() && item.back() == ' ')
                item.pop_back();
            if (item == "all")
                settings.TacticFocus = settings.TacticInterrupt = settings.TacticHeal = settings.TacticRest = settings.TacticRecover = true;
            else if (item == "focus")
                settings.TacticFocus = true;
            else if (item == "interrupt")
                settings.TacticInterrupt = true;
            else if (item == "heal")
                settings.TacticHeal = true;
            else if (item == "rest")
                settings.TacticRest = true;
            else if (item == "recover")
                settings.TacticRecover = true;
            else if (item != "none" && !item.empty())
                warnings.push_back("Module.forever-bots.DungeonTactics: '" + item + "' is not focus, interrupt, heal, rest, recover, all or none; ignored.");
        }
    }
    ReadBool(config, "Crafting", settings.Crafting, warnings);
    ReadBool(config, "ActionStats", settings.ActionStats, warnings);
    ReadKey(config, "MountMinLevel", 0, 80, settings.MountMinLevel, warnings);
    ReadBool(config, "Gathering", settings.Gathering, warnings);
    ReadBool(config, "AvoidGroundEffects", settings.AvoidGroundEffects, warnings);
    if (auto const it = config.find("AvoidSpellsFile"); it != config.end())
        settings.AvoidSpellsFile = it->second;
    if (auto const it = config.find("CraftingFociFile"); it != config.end())
        settings.CraftingFociFile = it->second;
    if (auto const it = config.find("DungeonFile"); it != config.end())
        settings.DungeonFile = it->second;
    if (auto const it = config.find("DungeonDataDir"); it != config.end())
        settings.DungeonDataDir = it->second;
    if (auto const it = config.find("DungeonDir"); it != config.end())
        settings.DungeonDir = it->second;
    if (auto const it = config.find("DungeonEventsFile"); it != config.end())
        settings.DungeonEventsFile = it->second;
    ReadBool(config, "Presence", settings.Presence.Enabled, warnings);
    ReadKey(config, "PresenceOnlineMinMin", 1, 1440, settings.Presence.OnlineMinMin, warnings);
    ReadKey(config, "PresenceOnlineMaxMin", 1, 1440, settings.Presence.OnlineMaxMin, warnings);
    ReadKey(config, "PresenceOfflineMinMin", 1, 1440, settings.Presence.OfflineMinMin, warnings);
    ReadKey(config, "PresenceOfflineMaxMin", 1, 1440, settings.Presence.OfflineMaxMin, warnings);
    ReadKey(config, "PresenceMinOnline", 0, 100000, settings.Presence.MinOnline, warnings);
    ReadKey(config, "PresenceMaxOnline", 0, 100000, settings.Presence.MaxOnline, warnings);
    settings.Presence.OnlineMaxMin = std::max(settings.Presence.OnlineMaxMin, settings.Presence.OnlineMinMin);
    settings.Presence.OfflineMaxMin = std::max(settings.Presence.OfflineMaxMin, settings.Presence.OfflineMinMin);
    if (auto const it = config.find("PresenceDayPercent"); it != config.end())
    {
        // 24 comma-separated percents, midnight first.
        std::array<std::uint32_t, 24> percent{};
        std::size_t count = 0;
        bool ok = true;
        std::istringstream in(it->second);
        std::string part;
        while (std::getline(in, part, ','))
        {
            std::uint32_t value = 0;
            std::string reason;
            if (count >= 24 || !ParseBounded(part, 0, 100, value, reason))
            {
                ok = false;
                break;
            }
            percent[count++] = value;
        }
        if (ok && count == 24)
            settings.Presence.DayPercent = percent;
        else
            warnings.push_back("Module.forever-bots.PresenceDayPercent: expected 24 comma-separated numbers from 0 to 100; using the default curve.");
    }
    ReadBool(config, "Taxi", settings.Taxi.Enabled, warnings);
    {
        std::uint32_t minWalk = static_cast<std::uint32_t>(settings.Taxi.MinWalkYards);
        std::uint32_t masterMax = static_cast<std::uint32_t>(settings.Taxi.MasterMaxYards);
        std::uint32_t blockedSec = static_cast<std::uint32_t>(settings.Taxi.BlockedMs / 1000);
        ReadKey(config, "TaxiMinWalkYards", 300, 20000, minWalk, warnings);
        ReadKey(config, "TaxiMasterMaxYards", 50, 5000, masterMax, warnings);
        ReadKey(config, "TaxiBlockedSec", 10, 86400, blockedSec, warnings);
        settings.Taxi.MinWalkYards = float(minWalk);
        settings.Taxi.MasterMaxYards = float(masterMax);
        settings.Taxi.BlockedMs = std::uint64_t(blockedSec) * 1000;
    }
    ReadBool(config, "Auction", settings.Auction.Enabled, warnings);
    ReadKey(config, "AuctionSellPctGreen", 100, 2000, settings.Auction.SellPctGreen, warnings);
    ReadKey(config, "AuctionSellPctBlue", 100, 2000, settings.Auction.SellPctBlue, warnings);
    ReadKey(config, "AuctionSellPctEpic", 100, 2000, settings.Auction.SellPctEpic, warnings);
    ReadKey(config, "AuctionBuyoutPct", 100, 500, settings.Auction.BuyoutPct, warnings);
    ReadKey(config, "AuctionRelistPct", 50, 100, settings.Auction.RelistPct, warnings);
    {
        // The core's kList sends every auction a 120-minute run (ModuleActions.cpp, sellItem.RunTime and the deposit), and the wire
        // carries no run time. So 2 is the only value; any other is refused with a warning and the auction runs 2 hours.
        std::uint32_t hours = settings.Auction.RunMinutes / 60;
        ReadKey(config, "AuctionRunHours", 2, 2, hours, warnings);
        settings.Auction.RunMinutes = hours * 60;
    }
    {
        std::uint32_t minCopper = static_cast<std::uint32_t>(settings.Auction.MinVendorCopper);
        ReadKey(config, "AuctionMinVendorCopper", 0, 1000000, minCopper, warnings);
        settings.Auction.MinVendorCopper = minCopper;
    }
    ReadKey(config, "AuctionBagFullPct", 1, 100, settings.Auction.BagFullPct, warnings);
    ReadKey(config, "AuctionPerVisit", 1, 20, settings.Auction.PerVisit, warnings);
    ReadKey(config, "AuctionListedMax", 1, 50, settings.Auction.ListedMax, warnings);
    {
        std::uint32_t everySec = static_cast<std::uint32_t>(settings.Auction.EveryMs / 1000);
        ReadKey(config, "AuctionEverySec", 60, 86400, everySec, warnings);
        settings.Auction.EveryMs = std::uint64_t(everySec) * 1000;
    }
    ReadKey(config, "AuctionBuyMaxPct", 1, 100, settings.Auction.BuyMaxPct, warnings);
    ReadKey(config, "AuctionBrowsePerVisit", 1, 20, settings.Auction.BrowsePerVisit, warnings);
    ReadKey(config, "AuctionCallsPerTick", 1, 50, settings.Auction.MaxCallsPerTick, warnings);
    ReadBool(config, "SellVendor", settings.Sell.Enabled, warnings);
    {
        std::uint32_t everySec = static_cast<std::uint32_t>(settings.Sell.EveryMs / 1000);
        ReadKey(config, "SellEverySec", 60, 86400, everySec, warnings);
        settings.Sell.EveryMs = std::uint64_t(everySec) * 1000;
    }
    ReadKey(config, "SellPerVisit", 1, 20, settings.Sell.MaxSalesPerVisit, warnings);
    ReadBool(config, "Errands", settings.Errand.Enabled, warnings);
    ReadKey(config, "ErrandDailyMin", 1, 1440, settings.Errand.DailyMinutes, warnings);
    ReadKey(config, "ErrandGapMin", 0, 1440, settings.Errand.GapMinutes, warnings);
    ReadKey(config, "ErrandMaxMin", 5, 240, settings.Errand.MaxMinutes, warnings);
    ReadKey(config, "ErrandPerCity", 1, 20, settings.Errand.MaxPerCity, warnings);
    ReadKey(config, "ErrandStartsPerTick", 1, 10, settings.Errand.MaxStartsPerTick, warnings);
    if (auto const it = config.find("ErrandTrainerFile"); it != config.end())
        settings.ErrandTrainerFile = it->second;
    // A target below its "low" mark would send a bot shopping that then buys nothing: the target is raised to the mark.
    settings.Supplies.AmmoTarget = std::max(settings.Supplies.AmmoTarget, settings.Supplies.AmmoLow);
    settings.Supplies.FoodTarget = std::max(settings.Supplies.FoodTarget, settings.Supplies.FoodLow);
    settings.Supplies.DrinkTarget = std::max(settings.Supplies.DrinkTarget, settings.Supplies.DrinkLow);
    ReadKey(config, "AutoGearEveryLevels", 1, 60, settings.AutoGearEveryLevels, warnings);
    ReadScenario(config, settings.Scenario, warnings);
    if (auto const it = config.find("RotationFile"); it != config.end())
        settings.RotationFile = it->second;
    if (auto const it = config.find("SpellCostFile"); it != config.end())
        settings.SpellCostFile = it->second;
    if (auto const it = config.find("GrindFile"); it != config.end())
        settings.GrindFile = it->second;
    if (auto const it = config.find("QuestFile"); it != config.end())
        settings.QuestFile = it->second;
    return settings;
}
