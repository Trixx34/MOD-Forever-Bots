// SPDX-License-Identifier: GPL-2.0-or-later
#include "dungeon_data.h"
#include "dungeon_files.h"
#include "bot_settings.h"
#include "text_number.h"

#include <charconv>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace
{

bool ReadNumbers(std::istringstream& in, float* out, std::size_t count)
{
    return text_number::ParseFloats(in, out, count);
}

bool ReadUint(std::istringstream& in, std::uint32_t& out)
{
    long long value = -1;
    std::string trailing;
    if (!(in >> value) || value < 0 || value > 1000000 || (in >> trailing))
        return false;
    out = std::uint32_t(value);
    return true;
}

bool RosterOkImpl(std::string const& roster)
{
    std::istringstream list(roster);
    std::string item;
    std::size_t count = 0;
    while (std::getline(list, item, ','))
    {
        auto const colon = item.find(':');
        std::uint32_t race = 0, cls = 0;
        if (colon == std::string::npos || std::from_chars(item.data(), item.data() + colon, race).ec != std::errc{} ||
            std::from_chars(item.data() + colon + 1, item.data() + item.size(), cls).ec != std::errc{} || race == 0 || cls == 0)
            return false;
        ++count;
    }
    return count >= 1 && count <= 5;
}

std::string Num(float value)
{
    std::ostringstream out;
    out << std::setprecision(9) << value;
    return out.str();
}

std::string Join(float const* values, std::size_t count)
{
    std::string text;
    for (std::size_t i = 0; i < count; ++i)
        text += (i ? " " : "") + Num(values[i]);
    return text;
}

std::string Trim(std::string text)
{
    auto const first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos)
        return {};
    return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

} // namespace

bool DungeonRosterOk(std::string const& roster)
{
    return RosterOkImpl(roster);
}

std::vector<Dungeon> ParseDungeons(std::string_view text, std::vector<std::string>& warnings)
{
    std::vector<Dungeon> dungeons;
    std::optional<Dungeon> current;
    bool bad = false;
    std::size_t lineNumber = 0;
    auto const fail = [&](std::string const& why)
    {
        warnings.push_back("dungeon file line " + std::to_string(lineNumber) + ": " + why);
        bad = true;
    };

    std::istringstream lines{ std::string(text) };
    std::string raw;
    while (std::getline(lines, raw))
    {
        ++lineNumber;
        if (auto const hash = raw.find('#'); hash != std::string::npos)
            raw.erase(hash);
        std::string const line = Trim(raw);
        if (line.empty())
            continue;
        std::istringstream in(line);
        std::string key;
        in >> key;

        if (key == "dungeon")
        {
            if (current)
                fail("'dungeon' inside another dungeon (missing 'end'); the first one is dropped");
            current.emplace();
            bad = false;
            in >> current->Id;
            if (current->Id.empty())
                fail("'dungeon' needs an id");
            continue;
        }
        if (!current)
        {
            warnings.push_back("dungeon file line " + std::to_string(lineNumber) + ": '" + key + "' outside a dungeon; ignored");
            continue;
        }
        Dungeon& d = *current;
        if (key == "end")
        {
            std::string const id = d.Id;
            if (!bad)
            {
                bool const hasEntry = d.Entry[0] != 0.0f || d.Entry[1] != 0.0f || d.Entry[2] != 0.0f || d.Entry[3] != 0.0f;
                if (!hasEntry)
                    warnings.push_back("dungeon " + id + ": no 'entry'; dropped");
                else if (d.Route.empty())
                    warnings.push_back("dungeon " + id + ": empty route; dropped");
                else if (d.Roster.empty())
                    warnings.push_back("dungeon " + id + ": no 'roster'; dropped");
                else
                    dungeons.push_back(std::move(d));
            }
            else
                warnings.push_back("dungeon " + id + ": dropped because of the errors above");
            current.reset();
            continue;
        }
        if (key == "name")
        {
            std::getline(in, d.Name);
            d.Name = Trim(d.Name);
        }
        else if (key == "levels")
        {
            if (!(in >> d.MinLevel >> d.MaxLevel) || d.MinLevel < 1 || d.MaxLevel < d.MinLevel || d.MaxLevel > 60)
                fail("'levels' needs min max, 1 to 60");
        }
        else if (key == "roster")
        {
            in >> d.Roster;
            if (!RosterOkImpl(d.Roster))
                fail("'roster' is not 1 to 5 race:class pairs separated by commas");
        }
        else if (key == "staging" || key == "portal" || key == "entry")
        {
            std::array<float, 4> value{};
            if (!ReadNumbers(in, value.data(), 4))
                fail("'" + key + "' needs map x y z");
            else if (key == "staging")
                d.Staging = value;
            else if (key == "portal")
                d.Portal = value;
            else
                d.Entry = value;
        }
        else if (key == "duration" || key == "wipe_recovery" || key == "min_alive" || key == "leader_dead_abandon")
        {
            std::uint32_t& target = key == "duration"        ? d.DurationSec
                                    : key == "wipe_recovery" ? d.WipeRecoverySec
                                    : key == "min_alive"     ? d.MinAlive
                                                             : d.LeaderDeadAbandonSec;
            if (!ReadUint(in, target))
                fail("'" + key + "' needs one whole number");
        }
        else if (key == "go" || key == "boss")
        {
            DungeonWaypoint point;
            if (key == "boss")
            {
                long long entry = 0;
                if (!(in >> entry) || entry <= 0)
                {
                    fail("'boss' needs entry x y z name");
                    continue;
                }
                point.BossEntry = std::uint32_t(entry);
            }
            float xyz[3];
            bool ok = true;
            for (float& v : xyz)
                ok = ok && bool(in >> v);
            if (!ok)
            {
                fail("'" + key + "' needs x y z");
                continue;
            }
            point.X = xyz[0], point.Y = xyz[1], point.Z = xyz[2];
            if (key == "boss")
            {
                std::getline(in, point.BossName);
                point.BossName = Trim(point.BossName);
                for (char& c : point.BossName)
                    if (c == ',')
                        c = ' '; // the boss list is comma separated
                if (point.BossName.empty())
                    point.BossName = "boss" + std::to_string(point.BossEntry);
            }
            else if (std::string trailing; in >> trailing)
            {
                fail("'go' takes only x y z");
                continue;
            }
            d.Route.push_back(std::move(point));
        }
        else
            warnings.push_back("dungeon file line " + std::to_string(lineNumber) + ": unknown key '" + key + "'; ignored");
    }
    if (current)
        warnings.push_back("dungeon " + current->Id + ": no 'end' before the end of the file; dropped");
    return dungeons;
}

namespace
{

// The scenario keys of dungeon `d` that `config` does not set already, added to it.
std::unordered_map<std::string, std::string> MergeOne(std::unordered_map<std::string, std::string> const& config, Dungeon const& d)
{
    auto merged = config;
    auto const set = [&merged](std::string const& key, std::string value) { merged.emplace(key, std::move(value)); };
    set("Scenario", d.Id);
    set("ScenarioRoster", d.Roster);
    set("ScenarioLevel", std::to_string(d.MaxLevel));
    if (d.Staging)
        set("ScenarioStaging", Join(d.Staging->data(), 4));
    if (d.Portal)
        set("ScenarioPortal", Join(d.Portal->data(), 4));
    set("ScenarioEntry", Join(d.Entry.data(), 4));
    set("ScenarioDurationSec", std::to_string(d.DurationSec));
    if (d.WipeRecoverySec != 0)
    {
        set("ScenarioWipeRecovery", "1");
        set("ScenarioWipeRecoverySec", std::to_string(d.WipeRecoverySec));
    }
    if (d.MinAlive != 0)
        set("ScenarioMinAlive", std::to_string(d.MinAlive));
    if (d.LeaderDeadAbandonSec != 0)
        set("ScenarioLeaderDeadAbandonSec", std::to_string(d.LeaderDeadAbandonSec));

    if (!d.KillPriority.empty())
    {
        std::string order;
        for (std::uint32_t entry : d.KillPriority)
            order += (order.empty() ? "" : ",") + std::to_string(entry);
        set("ScenarioKillPriority", order);
    }

    std::string bosses;
    for (DungeonWaypoint const& point : d.Route)
        if (point.BossEntry != 0)
            bosses += (bosses.empty() ? "" : ",") + std::to_string(point.BossEntry) + ":" + point.BossName;
    if (!bosses.empty())
    {
        set("ScenarioBosses", bosses);
        set("ScenarioRequireBosses", "1");
        // Which route points are bosses ("6:644,11:643": the point's number from 1, the creature entry): the leader does not go past one, and does not
        // skip one that it cannot route to, before that boss is dead.
        std::string at;
        for (std::size_t i = 0; i < d.Route.size(); ++i)
            if (d.Route[i].BossEntry != 0)
                at += (at.empty() ? "" : ",") + std::to_string(i + 1) + ":" + std::to_string(d.Route[i].BossEntry);
        set("ScenarioBossAt", at);
    }

    bool ownRoute = false;
    for (auto const& [key, value] : config)
        ownRoute = ownRoute || key.rfind("ScenarioRoute", 0) == 0;
    if (!ownRoute)
        for (std::size_t i = 0; i < d.Route.size(); ++i)
        {
            float const xyz[3] = { d.Route[i].X, d.Route[i].Y, d.Route[i].Z };
            set("ScenarioRoute" + std::to_string(i + 1), Join(xyz, 3));
        }
    return merged;
}

Dungeon const* FindDungeon(std::vector<Dungeon> const& dungeons, std::string const& id)
{
    Dungeon const* found = nullptr;
    for (Dungeon const& d : dungeons)
        if (d.Id == id)
            found = &d;
    return found;
}

// Whether the config asks for a dungeon run: Dungeon = <id>, or a list of dungeons in Scenario (bot_settings.h).
bool WantsDungeon(std::unordered_map<std::string, std::string> const& config)
{
    auto const wanted = config.find("Dungeon");
    auto const scenario = config.find("Scenario");
    return (wanted != config.end() && !wanted->second.empty()) || (scenario != config.end() && IsScenarioList(scenario->second));
}

} // namespace

std::unordered_map<std::string, std::string> MergeDungeon(std::unordered_map<std::string, std::string> const& config, std::string_view text,
                                                          std::vector<std::string>& warnings)
{
    if (!WantsDungeon(config))
        return config;
    return MergeDungeonSet(config, ParseDungeons(text, warnings), warnings);
}

std::unordered_map<std::string, std::string> MergeDungeonSet(std::unordered_map<std::string, std::string> const& config, std::vector<Dungeon> const& dungeons,
                                                             std::vector<std::string>& warnings)
{
    // Several dungeons (Scenario = "dm:10,rfc:10"): each listed one is merged into the keys it reads (ScenarioConfigFor), and what the file adds is stored as
    // "<key>.<id>", where the settings read that dungeon's keys from. Keys the config sets for that dungeon, or for all of them, still win.
    if (auto const scenario = config.find("Scenario"); scenario != config.end() && IsScenarioList(scenario->second))
    {
        auto merged = config;
        std::vector<std::string> listWarnings; // the settings report the list's own mistakes
        for (ScenarioListItem const& item : ParseScenarioList(scenario->second, listWarnings))
        {
            Dungeon const* found = FindDungeon(dungeons, item.Name);
            if (!found)
            {
                warnings.push_back("Module.forever-bots.Scenario: '" + item.Name + "' is not in the dungeon file; that dungeon does not run.");
                continue;
            }
            auto const own = ScenarioConfigFor(config, item.Name, item.Parties);
            for (auto const& [key, value] : MergeOne(own, *found))
                if (own.count(key) == 0)
                    merged.emplace(key + "." + item.Name, value);
        }
        return merged;
    }

    auto const wanted = config.find("Dungeon");
    if (wanted == config.end() || wanted->second.empty())
        return config;
    Dungeon const* found = FindDungeon(dungeons, wanted->second);
    if (!found)
    {
        warnings.push_back("Module.forever-bots.Dungeon: '" + wanted->second + "' is not in the dungeon file; no dungeon run.");
        return config;
    }
    return MergeOne(config, *found);
}

std::unordered_map<std::string, std::string> MergeDungeonFile(std::unordered_map<std::string, std::string> const& config,
                                                              std::vector<std::string>& warnings)
{
    if (!WantsDungeon(config))
        return config;
    DungeonSources const sources = SourcesFromConfig(config);
    if (sources.File.empty() && sources.DataDir.empty() && sources.OverrideDir.empty())
        return config;
    std::size_t const before = warnings.size();
    DungeonSet const set = LoadDungeonSet(sources, warnings);
    if (set.Dungeons.empty() && warnings.size() > before)
        return config; // nothing could be read: the warnings above say why, and there is no dungeon run
    return MergeDungeonSet(config, set.Dungeons, warnings);
}
