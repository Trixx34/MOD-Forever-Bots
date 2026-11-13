// SPDX-License-Identifier: GPL-2.0-or-later
#include "bot_settings.h"

#include "bot_guilds.h"
#include "bot_pick.h"
#include "text_number.h"

#include <cstdlib>
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
    return text_number::ParseFloats(text, out, count);
}

// One dungeon's scenario: the single form, or one entry of a list read from its ScenarioConfigFor config.
void ReadOneScenario(std::unordered_map<std::string, std::string> const& config, ScenarioSettings& scenario, std::vector<std::string>& warnings)
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
            // race:class or race:class:spec (the spec is a lower case key such as bm, prot, shadow; whether the class has it is checked where the bot is spawned)
            auto const colon = item.find(':');
            auto const second = colon == std::string::npos ? std::string::npos : item.find(':', colon + 1);
            std::uint32_t race = 0, cls = 0;
            std::string spec;
            bool ok = colon != std::string::npos && std::from_chars(item.data(), item.data() + colon, race).ec == std::errc{} &&
                      std::from_chars(item.data() + colon + 1, item.data() + (second == std::string::npos ? item.size() : second), cls).ec == std::errc{} && race > 0 && cls > 0;
            if (ok && second != std::string::npos)
            {
                spec = item.substr(second + 1);
                while (!spec.empty() && std::isspace(static_cast<unsigned char>(spec.back())))
                    spec.pop_back();
                ok = !spec.empty() && std::all_of(spec.begin(), spec.end(), [](char c) { return (c >= 'a' && c <= 'z') || c == '_'; });
            }
            if (ok)
            {
                scenario.Roster.push_back({ race, cls });
                scenario.RosterSpec.push_back(spec);
            }
            else
                warnings.push_back("Module.forever-bots.ScenarioRoster: '" + item + "' is not race:class or race:class:spec; ignored.");
        }
    }
    if (scenario.Roster.empty())
    {
        warnings.push_back("Module.forever-bots.Scenario: the roster is empty; the scenario is off.");
        scenario.Name.clear();
        return;
    }
    // ScenarioRaid (doc 108): a raid roster may go up to 40 (TrinityCore's MAX_RAID_SIZE); a five-man scenario (unset, the
    // default) keeps the existing cap of 5 exactly as before -- this is a distinct mode, not a universal cap raise.
    ReadBool(config, "ScenarioRaid", scenario.Raid, warnings);
    std::uint32_t const rosterCap = scenario.Raid ? 40 : 5;
    if (scenario.Roster.size() > rosterCap)
        scenario.Roster.resize(rosterCap);

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
    if (auto const it = config.find("ScenarioSummons"); it != config.end())
    {
        std::istringstream list(it->second);
        std::string item;
        while (std::getline(list, item, ','))
        {
            std::uint32_t entry = 0;
            if (std::from_chars(item.data(), item.data() + item.size(), entry).ec == std::errc{} && entry != 0)
                scenario.Summons.push_back(entry);
            else
                warnings.push_back("Module.forever-bots.ScenarioSummons: '" + item + "' is not a creature entry; ignored.");
        }
    }
    ReadKey(config, "ScenarioParties", 1, kMaxScenarioParties, scenario.Parties, warnings);
    ReadKey(config, "ScenarioDurationSec", 30, 86400, scenario.DurationSec, warnings);
    if (auto const it = config.find("ScenarioStaging"); it != config.end())
        scenario.HasStaging = ParseNumbers(it->second, scenario.Staging.data(), 4);
    if (auto const it = config.find("ScenarioTameSpot"); it != config.end())
        scenario.HasTameSpot = ParseNumbers(it->second, scenario.TameSpot.data(), 4);
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
        else if (key == "ScenarioClassSupplies" || key == "ScenarioClassQuests")
        {
            // The first mention replaces the defaults; an empty value switches the kind off.
            bool const supplies = key == "ScenarioClassSupplies";
            if (supplies)
                scenario.ClassSupplies.clear();
            else
                scenario.ClassQuests.clear();
            std::istringstream list(value);
            std::string item;
            while (std::getline(list, item, ','))
            {
                std::istringstream parts(item);
                std::string a, b, c;
                std::getline(parts, a, ':');
                std::getline(parts, b, ':');
                std::getline(parts, c, ':');
                auto const num = [](std::string const& text, std::uint32_t& out)
                { return !text.empty() && std::from_chars(text.data(), text.data() + text.size(), out).ec == std::errc{} && out > 0; };
                std::uint32_t classId = 0, first = 0, second = 0;
                bool const ok = supplies ? num(a, classId) && num(b, first) && num(c, second) : num(a, classId) && num(b, first);
                if (!ok)
                    warnings.push_back("Module.forever-bots." + key + ": '" + item + "' is not " + (supplies ? "class:item:count" : "class:quest") + "; ignored.");
                else if (supplies)
                    scenario.ClassSupplies[classId].emplace_back(first, second);
                else
                    scenario.ClassQuests[classId].push_back(first);
            }
        }
        else if (key == "ScenarioAmmo")
        {
            std::uint32_t n = 0;
            if (std::from_chars(value.data(), value.data() + value.size(), n).ec == std::errc{} && n <= 2000)
                scenario.Ammo = n;
            else
                warnings.push_back("Module.forever-bots.ScenarioAmmo: not a count from 0 to 2000; ignored.");
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

// A party is one faction: the server refuses an invite across the sides, so a roster with races of both could never form its party.
bool OneFaction(ScenarioSettings const& scenario)
{
    bool alliance = false, horde = false;
    for (auto const& [race, cls] : scenario.Roster)
    {
        alliance = alliance || pick::IsAllianceRace(race);
        horde = horde || pick::IsHordeRace(race);
    }
    return !(alliance && horde);
}

// The keys that describe one dungeon: in the list form they are only read as "<key>.<id>".
bool IsPlainDungeonKey(std::string const& key)
{
    static char const* const keys[] = { "Dungeon", "ScenarioRoster", "ScenarioLevel", "ScenarioStaging", "ScenarioEntry", "ScenarioPortal",
                                        "ScenarioBosses", "ScenarioBossAt", "ScenarioKillPriority", "ScenarioSummons", "ScenarioParties", "ScenarioTameSpot", "ScenarioRaid" };
    if (key.find('.') != std::string::npos)
        return false;
    for (char const* plain : keys)
        if (key == plain)
            return true;
    return key.rfind("ScenarioRoute", 0) == 0;
}

void ReadScenarios(std::unordered_map<std::string, std::string> const& config, BotSettings& settings, std::vector<std::string>& warnings)
{
    auto const name = config.find("Scenario");
    if (name == config.end() || name->second.empty())
        return;
    if (!IsScenarioList(name->second))
    {
        ReadOneScenario(config, settings.Scenario, warnings);
        if (!settings.Scenario.Name.empty() && !OneFaction(settings.Scenario))
        {
            warnings.push_back("Module.forever-bots.ScenarioRoster: the roster mixes Alliance and Horde races, which cannot be one party; the scenario is off.");
            settings.Scenario = ScenarioSettings{};
        }
        if (!settings.Scenario.Name.empty())
            settings.Scenarios.push_back(settings.Scenario);
        return;
    }

    for (auto const& [key, value] : config)
        if (IsPlainDungeonKey(key) && key != "Dungeon")
            warnings.push_back("Module.forever-bots." + key + ": ignored with a list in Scenario; set " + key + ".<dungeon> for one dungeon of the list.");
    for (ScenarioListItem const& item : ParseScenarioList(name->second, warnings))
    {
        std::size_t const before = warnings.size();
        ScenarioSettings scenario;
        ReadOneScenario(ScenarioConfigFor(config, item.Name, item.Parties), scenario, warnings);
        for (std::size_t i = before; i < warnings.size(); ++i)
            warnings[i] += " (dungeon " + item.Name + ")";
        if (scenario.Name.empty())
            continue;
        if (!OneFaction(scenario))
        {
            warnings.push_back("Module.forever-bots.ScenarioRoster." + item.Name + ": the roster mixes Alliance and Horde races, which cannot be one party; " +
                               item.Name + " is dropped from the run.");
            continue;
        }
        // The module and the policy cut the bots, in guid order, into parties of one size: a dungeon with another roster size would shift every party after it.
        if (!settings.Scenarios.empty() && scenario.Roster.size() != settings.Scenarios.front().Roster.size())
        {
            warnings.push_back("Module.forever-bots.ScenarioRoster." + item.Name + ": " + std::to_string(scenario.Roster.size()) + " bots, not " +
                               std::to_string(settings.Scenarios.front().Roster.size()) + " as the first dungeon's roster; " + item.Name + " is dropped from the run.");
            continue;
        }
        settings.Scenarios.push_back(std::move(scenario));
    }
    if (!settings.Scenarios.empty())
        settings.Scenario = settings.Scenarios.front();
    else
        warnings.push_back("Module.forever-bots.Scenario: no dungeon of '" + name->second + "' can run; the scenario is off.");
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

bool IsScenarioList(std::string const& value)
{
    return value.find(':') != std::string::npos || value.find(',') != std::string::npos;
}

std::vector<ScenarioListItem> ParseScenarioList(std::string const& value, std::vector<std::string>& warnings)
{
    std::vector<ScenarioListItem> items;
    std::uint32_t total = 0;
    std::istringstream list(value);
    std::string entry;
    while (std::getline(list, entry, ','))
    {
        while (!entry.empty() && entry.front() == ' ')
            entry.erase(entry.begin());
        while (!entry.empty() && entry.back() == ' ')
            entry.pop_back();
        ScenarioListItem item;
        std::size_t const colon = entry.find(':');
        item.Name = entry.substr(0, colon);
        std::string reason;
        if (item.Name.empty() || (colon != std::string::npos && !ParseBounded(entry.substr(colon + 1), 1, kMaxScenarioParties, item.Parties, reason)))
        {
            warnings.push_back("Module.forever-bots.Scenario: '" + entry + "' is not dungeon:parties (1.." + std::to_string(kMaxScenarioParties) + "); ignored.");
            continue;
        }
        bool repeated = false;
        for (ScenarioListItem const& seen : items)
            repeated = repeated || seen.Name == item.Name;
        if (repeated)
        {
            warnings.push_back("Module.forever-bots.Scenario: '" + item.Name + "' is listed twice; the second one is ignored (give the first more parties).");
            continue;
        }
        if (total + item.Parties > kMaxScenarioParties)
        {
            warnings.push_back("Module.forever-bots.Scenario: more than " + std::to_string(kMaxScenarioParties) + " parties in all; " + item.Name + " gets " +
                               std::to_string(kMaxScenarioParties - total) + " instead of " + std::to_string(item.Parties) + ".");
            item.Parties = kMaxScenarioParties - total;
            if (item.Parties == 0)
                continue;
        }
        total += item.Parties;
        items.push_back(std::move(item));
    }
    return items;
}

std::unordered_map<std::string, std::string> ScenarioConfigFor(std::unordered_map<std::string, std::string> const& config, std::string const& name,
                                                               std::uint32_t parties)
{
    std::unordered_map<std::string, std::string> out;
    std::string const suffix = "." + name;
    for (auto const& [key, value] : config)
    {
        bool const isOverride = key.rfind("Scenario", 0) == 0 && key.find('.') != std::string::npos;
        if (!isOverride && !IsPlainDungeonKey(key) && key != "Scenario")
            out.emplace(key, value);
    }
    for (auto const& [key, value] : config)
        if (key.rfind("Scenario", 0) == 0 && key.size() > suffix.size() && key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0)
            out[key.substr(0, key.size() - suffix.size())] = value;
    out["Scenario"] = name;
    out["ScenarioParties"] = std::to_string(parties);
    out["Dungeon"] = name;
    return out;
}

ScenarioSlot ScenarioSlotOf(std::vector<ScenarioSettings> const& scenarios, std::size_t slot)
{
    ScenarioSlot where;
    std::size_t party = 0;
    for (std::size_t d = 0; d < scenarios.size(); ++d)
    {
        std::size_t const size = scenarios[d].Roster.size();
        std::size_t const bots = size * scenarios[d].Parties;
        if (size != 0 && slot < bots)
        {
            where.Valid = true;
            where.Dungeon = d;
            where.Party = party + slot / size;
            where.Race = scenarios[d].Roster[slot % size][0];
            where.Class = scenarios[d].Roster[slot % size][1];
            if (slot % size < scenarios[d].RosterSpec.size())
                where.Spec = scenarios[d].RosterSpec[slot % size];
            return where;
        }
        slot -= bots;
        party += scenarios[d].Parties;
    }
    return where;
}

std::uint32_t ScenarioBotCount(std::vector<ScenarioSettings> const& scenarios)
{
    std::uint32_t bots = 0;
    for (ScenarioSettings const& scenario : scenarios)
        bots += std::uint32_t(scenario.Roster.size()) * scenario.Parties;
    return bots;
}

std::uint32_t ScenarioPartyCount(std::vector<ScenarioSettings> const& scenarios)
{
    std::uint32_t parties = 0;
    for (ScenarioSettings const& scenario : scenarios)
        parties += scenario.Parties;
    return parties;
}

std::size_t ScenarioDungeonOfParty(std::vector<ScenarioSettings> const& scenarios, std::size_t party)
{
    for (std::size_t d = 0; d < scenarios.size(); ++d)
    {
        if (party < scenarios[d].Parties)
            return d;
        party -= scenarios[d].Parties;
    }
    return scenarios.empty() ? 0 : scenarios.size() - 1;
}

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

    // Brackets: the default split unless BracketTargets says otherwise (see bot_brackets.h).
    ReadBool(config, "BracketsEnabled", settings.Brackets.Enabled, warnings);
    ReadKey(config, "BracketSpotCap", 0, 1000, settings.Brackets.SpotCap, warnings);
    ReadKey(config, "BracketStalledMinutes", 0, 1440, settings.Brackets.StalledMinutes, warnings);
    ReadBool(config, "BracketMoveDown", settings.Brackets.MoveDown, warnings);
    {
        std::string error;
        brackets::ParseTargets(brackets::kDefaultTargets, settings.Brackets.Targets, error);
        if (auto const it = config.find("BracketTargets"); it != config.end() && !brackets::ParseTargets(it->second, settings.Brackets.Targets, error))
            warnings.push_back("BracketTargets: " + error + "; the default split is kept");
        if (auto const it = config.find("RetireLevel60"); it != config.end() && !brackets::ParseRetire(it->second, settings.Brackets.RetireLevel60, error))
            warnings.push_back("RetireLevel60: " + error);
    }

    if (auto const it = config.find("TelemetryFile"); it != config.end())
        settings.TelemetryFile = it->second;
    if (auto const it = config.find("LiveFile"); it != config.end())
        settings.LiveFile = it->second;
    ReadKey(config, "LiveEveryMs", 250, 10000, settings.LiveEveryMs, warnings);
    ReadKey(config, "TelemetryMaxMB", 1, 10240, settings.TelemetryMaxMB, warnings);
    ReadKey(config, "TelemetryKeep", 0, 50, settings.TelemetryKeep, warnings);
    ReadKey(config, "TelemetrySummarySec", 1, 3600, settings.TelemetrySummarySec, warnings);
    ReadKey(config, "StallAfterMinutes", 0, 1440, settings.StallAfterMinutes, warnings);
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
    ReadBool(config, "TalentRespec", settings.TalentRespec, warnings);
    ReadBool(config, "QuestTeleport", settings.QuestTeleport, warnings);
    ReadBool(config, "ChatCommands", settings.ChatCommands, warnings);
    ReadBool(config, "ChatOrders", settings.ChatOrders, warnings);
    ReadBool(config, "ChatControl", settings.ChatControl, warnings);
    ReadBool(config, "ChatSay", settings.ChatSay, warnings);
    ReadKey(config, "ChatSayMax", 0, 50, settings.ChatSayMax, warnings);
    ReadBool(config, "AmbientChat", settings.AmbientChat, warnings);
    ReadKey(config, "AmbientPerMin", 0, 30, settings.AmbientPerMin, warnings);
    ReadKey(config, "AmbientBotCooldownSec", 0, 3600, settings.AmbientBotCooldownSec, warnings);
    ReadKey(config, "AmbientChannels", 1, 3, settings.AmbientChannels, warnings);
    if (auto const it = config.find("AmbientFile"); it != config.end())
        settings.AmbientFile = it->second;
    ReadBool(config, "ChatWhisperReplies", settings.ChatWhisperReplies, warnings);
    ReadKey(config, "WhisperCooldownMs", 0, 600000, settings.WhisperCooldownMs, warnings);
    ReadKey(config, "WhisperMaxPerMin", 1, 60, settings.WhisperMaxPerMin, warnings);
    ReadBool(config, "Duels", settings.Duels.Enabled, warnings);
    ReadKey(config, "DuelMinLevel", 1, 80, settings.Duels.MinLevel, warnings);
    ReadKey(config, "DuelCooldownMin", 1, 1440, settings.Duels.CooldownMin, warnings);
    ReadBool(config, "Battlegrounds", settings.Battlegrounds.Enabled, warnings);
    ReadKey(config, "BattlegroundMinLevel", 1, 80, settings.Battlegrounds.MinLevel, warnings);
    ReadKey(config, "BattlegroundMaxWaitMin", 1, 60, settings.Battlegrounds.MaxWaitMin, warnings);
    ReadBool(config, "IdleFidget", settings.Fidget.Enabled, warnings);
    ReadKey(config, "IdleFidgetChancePct", 0, 100, settings.Fidget.ChancePct, warnings);
    ReadKey(config, "IdleFidgetMinSec", 1, 600, settings.Fidget.MinGapSec, warnings);
    ReadKey(config, "IdleFidgetMaxSec", 1, 600, settings.Fidget.MaxGapSec, warnings);
    ReadKey(config, "IdleFidgetStandWeight", 0, 100, settings.Fidget.StandWeight, warnings);
    ReadKey(config, "IdleFidgetStepWeight", 0, 100, settings.Fidget.StepWeight, warnings);
    ReadKey(config, "IdleFidgetTurnWeight", 0, 100, settings.Fidget.TurnWeight, warnings);
    if (settings.Fidget.MaxGapSec < settings.Fidget.MinGapSec)
        settings.Fidget.MaxGapSec = settings.Fidget.MinGapSec;
    ReadBool(config, "ReadDelay", settings.ReadDelay.Enabled, warnings);
    ReadKey(config, "ReadDelayMinSec", 0, 60, settings.ReadDelay.MinSec, warnings);
    ReadKey(config, "ReadDelayMaxSec", 0, 60, settings.ReadDelay.MaxSec, warnings);
    if (settings.ReadDelay.MaxSec < settings.ReadDelay.MinSec)
        settings.ReadDelay.MaxSec = settings.ReadDelay.MinSec;
    ReadBool(config, "PassingBuff", settings.PassingBuff.Enabled, warnings);
    ReadKey(config, "PassingBuffChancePct", 0, 100, settings.PassingBuff.ChancePct, warnings);
    ReadKey(config, "PassingBuffManaAbovePct", 0, 100, settings.PassingBuff.ManaAbovePct, warnings);
    ReadKey(config, "PassingBuffCooldownSec", 0, 86400, settings.PassingBuff.CooldownSec, warnings);
    ReadKey(config, "PassingBuffZonePerMin", 0, 60, settings.PassingBuff.ZonePerMin, warnings);
    ReadKey(config, "PassingBuffReserveCasts", 0, 20, settings.PassingBuff.ReserveCasts, warnings);
    ReadBool(config, "TownAfk", settings.TownAfk.Enabled, warnings);
    ReadKey(config, "TownAfkChancePct", 0, 100, settings.TownAfk.ChancePct, warnings);
    ReadKey(config, "TownAfkMinMin", 1, 240, settings.TownAfk.MinMin, warnings);
    ReadKey(config, "TownAfkMaxMin", 1, 240, settings.TownAfk.MaxMin, warnings);
    ReadKey(config, "TownAfkZoneMax", 0, 100, settings.TownAfk.ZoneMax, warnings);
    if (settings.TownAfk.MaxMin < settings.TownAfk.MinMin)
        settings.TownAfk.MaxMin = settings.TownAfk.MinMin;
    ReadBool(config, "LfgQueueFill", settings.LfgQueueFill.Enabled, warnings);
    ReadKey(config, "LfgQueueFillWaitSec", 0, 3600, settings.LfgQueueFill.WaitSec, warnings);
    ReadKey(config, "LfgQueueFillCooldownSec", 0, 86400, settings.LfgQueueFill.CooldownSec, warnings);
    ReadBool(config, "DeathPause", settings.DeathPause.Enabled, warnings);
    ReadKey(config, "DeathPauseMinSec", 0, 600, settings.DeathPause.MinSec, warnings);
    ReadKey(config, "DeathPauseMaxSec", 0, 600, settings.DeathPause.MaxSec, warnings);
    if (settings.DeathPause.MaxSec < settings.DeathPause.MinSec)
        settings.DeathPause.MaxSec = settings.DeathPause.MinSec;
    ReadBool(config, "TradeAds", settings.TradeAds.Enabled, warnings);
    ReadKey(config, "TradeAdsIntervalMin", 1, 1440, settings.TradeAds.IntervalMin, warnings);
    ReadKey(config, "TradeAdsMinSkill", 1, 450, settings.TradeAds.MinSkill, warnings);
    if (auto const it = config.find("TradeAdsFile"); it != config.end())
        settings.TradeAdsFile = it->second;
    ReadBool(config, "PartyMana", settings.PartyMana.Enabled, warnings);
    ReadKey(config, "PartyManaHoldSec", 1, 600, settings.PartyMana.HoldSec, warnings);
    if (auto const it = config.find("PartyManaFile"); it != config.end())
        settings.PartyManaFile = it->second;
    ReadBool(config, "WhisperMatrix", settings.WhisperMatrix.Enabled, warnings);
    if (auto const it = config.find("WhisperMatrixFile"); it != config.end())
        settings.WhisperMatrixFile = it->second;
    ReadBool(config, "PathJitter", settings.PathJitter.Enabled, warnings);
    if (auto const it = config.find("PathJitterYards"); it != config.end())
    {
        char* end = nullptr;
        float const yards = std::strtof(it->second.c_str(), &end);
        if (end != it->second.c_str() && *end == '\0' && yards >= 0.0f && yards <= 5.0f)
            settings.PathJitter.Yards = yards;
        else
            warnings.push_back("Module.forever-bots.PathJitterYards: '" + it->second + "' is not a number from 0 to 5; using the default.");
    }
    ReadBool(config, "GuildMilestone", settings.GuildMilestone.Enabled, warnings);
    ReadKey(config, "GuildMilestoneChancePct", 0, 100, settings.GuildMilestone.ChancePct, warnings);
    if (auto const it = config.find("GuildMilestoneFile"); it != config.end())
        settings.GuildMilestoneFile = it->second;
    ReadBool(config, "CityInspect", settings.CityInspect.Enabled, warnings);
    ReadKey(config, "CityInspectChancePct", 0, 100, settings.CityInspect.ChancePct, warnings);
    ReadKey(config, "CityInspectZoneMax", 0, 100, settings.CityInspect.ZoneMax, warnings);
    ReadBool(config, "TransportRide", settings.TransportRide.Enabled, warnings);
    ReadKey(config, "TransportRideEveryMs", 1000, 60000, settings.TransportRide.EveryMs, warnings);
    ReadKey(config, "TransportRideBackoffMs", 5000, 600000, settings.TransportRide.BackoffMs, warnings);
    ReadBool(config, "CityGreet", settings.CityGreet.Enabled, warnings);
    ReadKey(config, "CityGreetChancePct", 0, 100, settings.CityGreet.ChancePct, warnings);
    ReadKey(config, "CityGreetZoneMax", 0, 100, settings.CityGreet.ZoneMax, warnings);
    if (auto const it = config.find("CityGreetFile"); it != config.end())
        settings.CityGreetFile = it->second;
    if (auto const it = config.find("AuctionTalkFile"); it != config.end())
        settings.AuctionTalkFile = it->second;
    ReadBool(config, "QuestInvite", settings.QuestInvite.Enabled, warnings);
    ReadBool(config, "QuestGroups", settings.QuestGroups.Enabled, warnings);
    ReadBool(config, "QuestGroupGuildHelpers", settings.QuestGroups.GuildHelpers, warnings);
    {
        std::uint32_t minutes = std::uint32_t(settings.QuestGroups.WaitMs / 60000);
        ReadKey(config, "QuestGroupWaitMinutes", 1, 120, minutes, warnings);
        settings.QuestGroups.WaitMs = std::uint64_t(minutes) * 60000;
        std::uint32_t range = std::uint32_t(settings.QuestGroups.RangeYards);
        ReadKey(config, "QuestGroupRangeYards", 10, 1000, range, warnings);
        settings.QuestGroups.RangeYards = float(range);
    }
    // ADVISOR-KEYS
    ReadBool(config, "InnDowntime", settings.InnDowntime.Enabled, warnings);
    ReadBool(config, "ScenarioConfigGuard", settings.ScenarioConfigGuard, warnings);
    if (auto const it = config.find("DungeonSummons"); it != config.end())
    {
        std::istringstream list(it->second);
        std::string item;
        while (std::getline(list, item, ','))
            if (!item.empty())
                settings.DungeonSummons.push_back(item);
    }
    ReadKey(config, "InnHealthBelowPct", 1, 100, settings.InnDowntime.HealthBelowPct, warnings);
    ReadKey(config, "InnPowerBelowPct", 0, 100, settings.InnDowntime.PowerBelowPct, warnings);
    ReadKey(config, "InnMaxRestMin", 1, 120, settings.InnDowntime.MaxRestMin, warnings);
    ReadBool(config, "GuildJoin", settings.GuildJoin.Enabled, warnings);
    settings.Guilds = bot_guilds::LoadSettings(config, warnings);
    ReadBool(config, "MailGifts", settings.MailGifts.Enabled, warnings);
    ReadKey(config, "MailGiftDailyCap", 0, 20, settings.MailGifts.DailyCap, warnings);
    ReadBool(config, "Supplies", settings.Supplies.Enabled, warnings);
    ReadBool(config, "VendorSellGear", settings.VendorSellGear, warnings);
    ReadKey(config, "SpiritHealerAfterMs", 0, 3600000, settings.SpiritHealerAfterMs, warnings);
    ReadBool(config, "SpiritHealerOnRepeat", settings.SpiritHealerOnRepeat, warnings);
    ReadBool(config, "HealDownrank", settings.HealDownrank, warnings);
    ReadBool(config, "ThreatHold", settings.ThreatHold, warnings);
    ReadKey(config, "ThreatHoldPct", 1, 100, settings.ThreatHoldPct, warnings);
    ReadKey(config, "ThreatReleasePct", 0, 99, settings.ThreatReleasePct, warnings);
    ReadKey(config, "ThreatHoldMaxSec", 1, 60, settings.ThreatHoldMaxSec, warnings);
    ReadKey(config, "ThreatNearDeathPct", 0, 100, settings.ThreatNearDeathPct, warnings);
    ReadBool(config, "ThreatDropAbilities", settings.ThreatDropAbilities, warnings);
    if (settings.ThreatReleasePct >= settings.ThreatHoldPct)
    {
        warnings.push_back("Module.forever-bots.ThreatReleasePct: not under ThreatHoldPct; using ThreatHoldPct - 1.");
        settings.ThreatReleasePct = settings.ThreatHoldPct - 1;
    }
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
    ReadBool(config, "LevelDungeons", settings.LevelDungeons, warnings);
    ReadKey(config, "LevelDungeonRuns", 0, 1000, settings.LevelDungeonRuns, warnings);
    settings.LevelDungeonRuns = std::clamp<std::uint32_t>(settings.LevelDungeonRuns, 1, 5);
    ReadBool(config, "LfgCommand", settings.LfgCommand, warnings);
    ReadBool(config, "LfgFill", settings.LfgFill, warnings);
    ReadBool(config, "DungeonReviveOutOfCombat", settings.DungeonReviveOutOfCombat, warnings);
    for (auto const& [key, value] : { std::pair<char const*, std::uint32_t*>{ "DungeonPullLeashYards", &settings.DungeonPullLeashYards },
                                      std::pair<char const*, std::uint32_t*>{ "DungeonChaseLeashYards", &settings.DungeonChaseLeashYards } })
    {
        if (auto const leash = config.find(key); leash != config.end() && leash->second == "0")
            *value = 0; // explicitly off
        else
            ReadKey(config, key, 10, 80, *value, warnings);
    }
    ReadBool(config, "DungeonLeaderRejoin", settings.DungeonLeaderRejoin, warnings);
    if (auto const perSecond = config.find("GhostTeleportPerSecond"); perSecond != config.end() && perSecond->second == "0")
        settings.GhostTeleportPerSecond = 0;
    else
        ReadKey(config, "GhostTeleportPerSecond", 1, 100, settings.GhostTeleportPerSecond, warnings);
    ReadBool(config, "GhostTeleportHold", settings.GhostTeleportHold, warnings);
    ReadKey(config, "GhostTeleportHoldMaxMs", 1000, 3600000, settings.GhostTeleportHoldMaxMs, warnings);
    if (auto const lockout = config.find("DeathLockoutCount"); lockout != config.end() && lockout->second == "0")
        settings.DeathLockoutCount = 0; // explicitly off
    else
        ReadKey(config, "DeathLockoutCount", 2, 50, settings.DeathLockoutCount, warnings);
    ReadKey(config, "DeathLockoutWindowMs", 10000, 86400000, settings.DeathLockoutWindowMs, warnings);
    ReadKey(config, "DeathLockoutHoldMs", 0, 3600000, settings.DeathLockoutHoldMs, warnings);
    if (auto const teleport = config.find("CorpseRunTeleportSec"); teleport != config.end() && teleport->second == "0")
        settings.CorpseRunTeleportSec = 0; // explicitly off: the ghost walks on, and is put at its corpse after CorpseRunTimeoutMs
    else
        ReadKey(config, "CorpseRunTeleportSec", 10, 3600, settings.CorpseRunTeleportSec, warnings);
    ReadKey(config, "DungeonHealBelowPct", 30, 85, settings.DungeonHealBelowPct, warnings);
    ReadKey(config, "DungeonTankHealBelowPct", 40, 95, settings.DungeonTankHealBelowPct, warnings);
    ReadKey(config, "HealerDpsManaPct", 10, 80, settings.HealerDpsManaPct, warnings);
    ReadKey(config, "HealerCombatManaPct", 0, 80, settings.HealerCombatManaPct, warnings);
    ReadBool(config, "TankPickup", settings.TankPickup, warnings);
    ReadBool(config, "TankLoosePickup", settings.TankLoosePickup, warnings);
    ReadKey(config, "TankLooseCooldownMs", 500, 60000, settings.TankLooseCooldownMs, warnings);
    ReadKey(config, "TankLooseRangeYards", 10, 60, settings.TankLooseRangeYards, warnings);
    if (auto const chase = config.find("TankLooseChaseYards"); chase != config.end() && chase->second == "0")
        settings.TankLooseChaseYards = 0; // explicitly never walk
    else
        ReadKey(config, "TankLooseChaseYards", 2, 20, settings.TankLooseChaseYards, warnings);
    ReadBool(config, "DamageEvents", settings.DamageEvents, warnings);
    ReadKey(config, "DamageEventMinPct", 1, 100, settings.DamageEventMinPct, warnings);
    ReadBool(config, "PullTankFirst", settings.PullTankFirst, warnings);
    ReadKey(config, "PullTankHoldMs", 1000, 120000, settings.PullTankHoldMs, warnings);
    ReadKey(config, "PullMemberRangeYards", 0, 40, settings.PullMemberRangeYards, warnings);
    ReadKey(config, "PullMinHpPct", 0, 100, settings.PullMinHpPct, warnings);
    ReadKey(config, "PullReadyManaPct", 0, 100, settings.PullReadyManaPct, warnings);
    ReadKey(config, "PullReadyMaxMs", 1000, 600000, settings.PullReadyMaxMs, warnings);
    ReadKey(config, "WipeReadyPct", 0, 100, settings.WipeReadyPct, warnings);
    ReadKey(config, "WipeReadyMaxMs", 10000, 1800000, settings.WipeReadyMaxMs, warnings);
    ReadKey(config, "WipeBackoffCount", 0, 20, settings.WipeBackoffCount, warnings);
    ReadKey(config, "WipeBackoffMs", 0, 900000, settings.WipeBackoffMs, warnings);
    ReadBool(config, "WarderPartyHold", settings.WarderPartyHold, warnings);
    ReadKey(config, "HealerLosMoveYards", 3, 12, settings.HealerLosMoveYards, warnings);
    ReadBool(config, "AggressiveStart", settings.AggressiveStart, warnings);
    if (auto const it = config.find("DungeonTactics"); it != config.end())
    {
        settings.TacticFocus = settings.TacticInterrupt = settings.TacticHeal = settings.TacticRest = settings.TacticRecover = settings.TacticCc = false;
        std::istringstream list(it->second);
        std::string item;
        while (std::getline(list, item, ','))
        {
            while (!item.empty() && item.front() == ' ')
                item.erase(item.begin());
            while (!item.empty() && item.back() == ' ')
                item.pop_back();
            if (item == "all")
                settings.TacticFocus = settings.TacticInterrupt = settings.TacticHeal = settings.TacticRest = settings.TacticRecover = settings.TacticCc = true;
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
            else if (item == "cc")
                settings.TacticCc = true;
            else if (item != "none" && !item.empty())
                warnings.push_back("Module.forever-bots.DungeonTactics: '" + item + "' is not focus, interrupt, heal, rest, recover, cc, all or none; ignored.");
        }
    }
    ReadBool(config, "PartyPlanner", settings.PartyPlanner, warnings);
    ReadBool(config, "HealPlan", settings.HealPlan, warnings);
    ReadKey(config, "HealPlanPreHealMs", 0, 60000, settings.HealPlanPreHealMs, warnings);
    ReadKey(config, "HealPlanPreHealPct", 1, 100, settings.HealPlanPreHealPct, warnings);
    ReadBool(config, "PartyViewPlanner", settings.PartyViewPlanner, warnings);
    ReadBool(config, "Crafting", settings.Crafting, warnings);
    ReadBool(config, "ActionStats", settings.ActionStats, warnings);
    ReadKey(config, "MountMinLevel", 0, 80, settings.MountMinLevel, warnings);
    ReadBool(config, "Gathering", settings.Gathering, warnings);
    ReadBool(config, "AvoidGroundEffects", settings.AvoidGroundEffects, warnings);
    ReadBool(config, "TelegraphEscape", settings.TelegraphEscape, warnings);
    ReadBool(config, "TelegraphEscapeCasters", settings.TelegraphEscapeCasters, warnings);
    ReadBool(config, "RaidSpread", settings.RaidSpread, warnings);
    ReadBool(config, "RaidAvoidEnemy", settings.RaidAvoidEnemy, warnings);
    ReadBool(config, "RaidPurge", settings.RaidPurge, warnings);
    ReadBool(config, "RaidMindControl", settings.RaidMindControl, warnings);
    if (auto const it = config.find("AvoidSpellsFile"); it != config.end())
        settings.AvoidSpellsFile = it->second;
    if (auto const it = config.find("RaidRulesFile"); it != config.end())
        settings.RaidRulesFile = it->second;
    if (auto const it = config.find("CraftingFociFile"); it != config.end())
        settings.CraftingFociFile = it->second;
    ReadBool(config, "GatherWalk", settings.GatherWalk, warnings);
    if (auto const it = config.find("GatherNodesFile"); it != config.end())
        settings.GatherNodesFile = it->second;
    if (auto const it = config.find("DungeonFile"); it != config.end())
        settings.DungeonFile = it->second;
    if (auto const it = config.find("DungeonDataDir"); it != config.end())
        settings.DungeonDataDir = it->second;
    if (auto const it = config.find("DungeonDir"); it != config.end())
        settings.DungeonDir = it->second;
    if (auto const it = config.find("RunTelemetryDir"); it != config.end())
    {
        if (it->second.empty() || it->second.front() == '/')
            settings.RunTelemetryDir = it->second;
        else
            warnings.push_back("Module.forever-bots.RunTelemetryDir: '" + it->second + "' is not an absolute path; the run telemetry file stays off.");
    }
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
    ReadBool(config, "AuctionBuy", settings.Auction.BuyEnabled, warnings);
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
    ReadKey(config, "AhBatchIntervalHours", 0, 168, settings.Auction.BatchIntervalHours, warnings);
    ReadKey(config, "AuctionBuyMaxPct", 1, 100, settings.Auction.BuyMaxPct, warnings);
    ReadKey(config, "AuctionBrowsePerVisit", 1, 20, settings.Auction.BrowsePerVisit, warnings);
    ReadKey(config, "AuctionCallsPerTick", 1, 50, settings.Auction.MaxCallsPerTick, warnings);
    ReadBool(config, "AuctionGoodsSell", settings.Auction.GoodsSell, warnings);
    ReadKey(config, "AuctionGoodsPct", 50, 1000, settings.Auction.GoodsPct, warnings);
    ReadKey(config, "AuctionGoodsStackMax", 1, 200, settings.Auction.GoodsStackMax, warnings);
    ReadKey(config, "AuctionGoodsPerEntryMax", 1, 10, settings.Auction.GoodsPerEntryMax, warnings);
    {
        std::uint32_t minQuality = settings.Auction.GoodsMinQuality;
        ReadKey(config, "AuctionGoodsMinQuality", 1, 4, minQuality, warnings);
        settings.Auction.GoodsMinQuality = static_cast<std::uint8_t>(minQuality);
    }
    ReadKey(config, "AuctionRoundStep", 0, 1000000, settings.Auction.RoundStepCopper, warnings);
    ReadKey(config, "AuctionRoundMaxPct", 0, 10, settings.Auction.RoundMaxPct, warnings);
    ReadBool(config, "SellVendor", settings.Sell.Enabled, warnings);
    {
        std::uint32_t everySec = static_cast<std::uint32_t>(settings.Sell.EveryMs / 1000);
        ReadKey(config, "SellEverySec", 60, 86400, everySec, warnings);
        settings.Sell.EveryMs = std::uint64_t(everySec) * 1000;
    }
    ReadKey(config, "SellPerVisit", 1, 20, settings.Sell.MaxSalesPerVisit, warnings);
    ReadBool(config, "Errands", settings.Errand.Enabled, warnings);
    ReadBool(config, "RidingTraining", settings.RidingTraining, warnings);
    if (auto const it = config.find("RidingRankFile"); it != config.end())
        settings.RidingRankFile = it->second;
    ReadBool(config, "WeaponTraining", settings.WeaponTraining, warnings);
    ReadBool(config, "Fishing", settings.Fishing, warnings);
    if (auto const it = config.find("FishingSpotFile"); it != config.end())
        settings.FishingSpotFile = it->second;
    ReadBool(config, "RareHunts", settings.RareHunts, warnings);
    if (auto const it = config.find("RareHuntFile"); it != config.end())
        settings.RareHuntFile = it->second;
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
    ReadScenarios(config, settings, warnings);
    if (auto const it = config.find("RotationFile"); it != config.end())
        settings.RotationFile = it->second;
    if (auto const it = config.find("TameBeastFile"); it != config.end())
        settings.TameBeastFile = it->second;
    if (auto const it = config.find("SpellCostFile"); it != config.end())
        settings.SpellCostFile = it->second;
    if (auto const it = config.find("GrindFile"); it != config.end())
        settings.GrindFile = it->second;
    if (auto const it = config.find("QuestFile"); it != config.end())
        settings.QuestFile = it->second;
    if (auto const it = config.find("QuestFileAlliance"); it != config.end())
        settings.QuestFileAlliance = it->second;
    if (auto const it = config.find("QuestFileHorde"); it != config.end())
        settings.QuestFileHorde = it->second;
    if (auto const it = config.find("QuestFileAlliance60"); it != config.end())
        settings.QuestFileAlliance60 = it->second;
    if (auto const it = config.find("QuestFileHorde60"); it != config.end())
        settings.QuestFileHorde60 = it->second;
    return settings;
}
