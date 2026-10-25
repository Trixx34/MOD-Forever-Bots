#include "dungeon_files.h"

#include "mini_json.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

using mini_json::Value;

namespace
{

// A problem that makes the whole file unusable. Thrown by the readers below and caught in ParseDungeonJson.
struct FileError
{
    std::string Why;
};

struct Reader
{
    std::string Source;
    std::vector<std::string>* Warnings = nullptr;

    [[noreturn]] void Fail(std::string const& path, std::string const& why) const
    {
        throw FileError{ path.empty() ? why : path + ": " + why };
    }

    void Warn(std::string const& path, std::string const& why) const
    {
        Warnings->push_back(Source + ": " + (path.empty() ? "" : path + ": ") + why);
    }

    // Keys the code knows. Any other key is a warning (probably a typo): the file is still used.
    void CheckKeys(Value const& object, std::string const& path, std::set<std::string> const& known) const
    {
        for (auto const& member : object.Members)
            if (!known.count(member.first))
                Warn(path, "unknown key '" + member.first + "'; ignored");
    }

    Value const& Object(Value const& parent, std::string const& path, char const* key, bool required) const
    {
        static Value const none;
        Value const* value = parent.Find(key);
        if (!value)
        {
            if (required)
                Fail(path, std::string("the key '") + key + "' is missing");
            return none;
        }
        if (!value->IsObject())
            Fail(path.empty() ? key : path + "." + key, "must be an object { ... }");
        return *value;
    }

    float Float(Value const& parent, std::string const& path, char const* key) const
    {
        Value const* value = parent.Find(key);
        float out = 0.0f;
        std::string const at = path.empty() ? key : path + "." + key;
        if (!value)
            Fail(path, std::string("the key '") + key + "' is missing");
        if (!mini_json::ToFloat(*value, out))
            Fail(at, "must be a number");
        return out;
    }

    std::uint32_t Uint(Value const& parent, std::string const& path, char const* key, std::uint32_t min, std::uint32_t max) const
    {
        Value const* value = parent.Find(key);
        std::string const at = path.empty() ? key : path + "." + key;
        if (!value)
            Fail(path, std::string("the key '") + key + "' is missing");
        return UintOf(*value, at, min, max);
    }

    std::uint32_t UintOf(Value const& value, std::string const& at, std::uint32_t min, std::uint32_t max) const
    {
        std::uint32_t out = 0;
        if (!mini_json::ToUint(value, out, max) || out < min)
            Fail(at, "must be a whole number from " + std::to_string(min) + " to " + std::to_string(max));
        return out;
    }

    std::string String(Value const& value, std::string const& at, std::size_t maxLength) const
    {
        if (!value.IsString())
            Fail(at, "must be text in quotes");
        if (value.Text.size() > maxLength)
            Fail(at, "is longer than " + std::to_string(maxLength) + " characters");
        for (char c : value.Text)
            if (c == '\n' || c == '\r')
                Fail(at, "must be on one line");
        return value.Text;
    }

    // { "x": .., "y": .., "z": .. }
    std::array<float, 3> Point3(Value const& value, std::string const& at, std::set<std::string> extra = {}) const
    {
        if (!value.IsObject())
            Fail(at, "must be an object { \"x\": .., \"y\": .., \"z\": .. }");
        std::set<std::string> known = { "x", "y", "z" };
        known.insert(extra.begin(), extra.end());
        CheckKeys(value, at, known);
        return { Float(value, at, "x"), Float(value, at, "y"), Float(value, at, "z") };
    }

    // { "map": .., "x": .., "y": .., "z": .. }
    std::array<float, 4> Point4(Value const& value, std::string const& at) const
    {
        if (!value.IsObject())
            Fail(at, "must be an object { \"map\": .., \"x\": .., \"y\": .., \"z\": .. }");
        CheckKeys(value, at, { "map", "x", "y", "z" });
        return { float(Uint(value, at, "map", 0, 100000)), Float(value, at, "x"), Float(value, at, "y"), Float(value, at, "z") };
    }

    std::vector<std::uint32_t> UintList(Value const& value, std::string const& at, std::size_t maxCount) const
    {
        if (!value.IsArray())
            Fail(at, "must be a list [ .., .. ] of whole numbers");
        if (value.Items.size() > maxCount)
            Fail(at, "has more than " + std::to_string(maxCount) + " entries");
        std::vector<std::uint32_t> out;
        for (std::size_t i = 0; i < value.Items.size(); ++i)
            out.push_back(UintOf(value.Items[i], at + "[" + std::to_string(i) + "]", 1, 1000000));
        return out;
    }
};

bool IdOk(std::string const& id, std::size_t maxLength)
{
    if (id.empty() || id.size() > maxLength)
        return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    return true;
}

std::string Squash(std::string const& text)
{
    // the old files kept a name as words joined by single spaces
    std::istringstream words(text);
    std::string out;
    for (std::string word; words >> word;)
        out += (out.empty() ? "" : " ") + word;
    return out;
}

std::string BossNameOf(std::string name, std::uint32_t entry)
{
    name = Squash(name);
    for (char& c : name)
        if (c == ',')
            c = ' '; // the boss list of the scenario is comma separated (as the text file did)
    name = Squash(name);
    return name.empty() ? "boss" + std::to_string(entry) : name;
}

BossStrategy ReadStrategy(Reader const& r, Value const& value, std::string const& at)
{
    if (!value.IsObject())
        r.Fail(at, "must be an object { ... }");
    r.CheckKeys(value, at, { "pull", "tank_position", "interrupt", "dispel", "avoid", "avoid_spells", "kill_order", "notes" });
    BossStrategy s;
    if (Value const* pull = value.Find("pull"))
    {
        s.Pull = r.String(*pull, at + ".pull", 32);
        if (!IdOk(s.Pull, 32))
            r.Fail(at + ".pull", "is one word of letters, digits, '-' and '_' (for example tank, ranged)");
    }
    if (Value const* position = value.Find("tank_position"))
        s.TankPosition = r.Point3(*position, at + ".tank_position");
    if (Value const* interrupt = value.Find("interrupt"))
        s.Interrupt = r.UintList(*interrupt, at + ".interrupt", 32);
    if (Value const* dispel = value.Find("dispel"))
        s.Dispel = r.UintList(*dispel, at + ".dispel", 32);
    if (Value const* avoid = value.Find("avoid"))
    {
        if (!avoid->IsArray())
            r.Fail(at + ".avoid", "must be a list [ { \"x\": .., \"y\": .., \"z\": .., \"radius\": .. } ]");
        if (avoid->Items.size() > 64)
            r.Fail(at + ".avoid", "has more than 64 zones");
        for (std::size_t i = 0; i < avoid->Items.size(); ++i)
        {
            std::string const zoneAt = at + ".avoid[" + std::to_string(i) + "]";
            auto const xyz = r.Point3(avoid->Items[i], zoneAt, { "radius", "note" });
            AvoidZone zone;
            zone.X = xyz[0], zone.Y = xyz[1], zone.Z = xyz[2];
            zone.Radius = r.Float(avoid->Items[i], zoneAt, "radius");
            if (zone.Radius <= 0.0f || zone.Radius > 500.0f)
                r.Fail(zoneAt + ".radius", "must be more than 0 and at most 500 yards");
            if (Value const* note = avoid->Items[i].Find("note"))
                zone.Note = r.String(*note, zoneAt + ".note", 200);
            s.Avoid.push_back(std::move(zone));
        }
    }
    if (Value const* avoidSpells = value.Find("avoid_spells"))
        s.AvoidSpells = r.UintList(*avoidSpells, at + ".avoid_spells", 32);
    if (Value const* order = value.Find("kill_order"))
        s.KillOrder = r.UintOf(*order, at + ".kill_order", 1, 50);
    if (Value const* notes = value.Find("notes"))
        s.Notes = r.String(*notes, at + ".notes", 500);
    return s;
}

dungeon_events::Event ReadEvent(Reader const& r, Value const& value, std::string const& at, std::uint32_t map)
{
    using dungeon_events::Event;
    using dungeon_events::Step;
    if (!value.IsObject())
        r.Fail(at, "must be an object { ... }");
    r.CheckKeys(value, at, { "id", "name", "trigger", "steps", "timeout_sec" });
    Event e;
    e.Map = map;
    Value const* id = value.Find("id");
    if (!id)
        r.Fail(at, "the key 'id' is missing");
    e.Id = r.String(*id, at + ".id", 40);
    if (!IdOk(e.Id, 40))
        r.Fail(at + ".id", "is one word of letters, digits, '-' and '_'");
    if (Value const* name = value.Find("name"))
        e.Name = Squash(r.String(*name, at + ".name", 120));
    Value const& trigger = r.Object(value, at, "trigger", true);
    std::string const triggerAt = at + ".trigger";
    Value const* type = trigger.Find("type");
    if (!type)
        r.Fail(triggerAt, "the key 'type' is missing (near, after_boss or at_start)");
    std::string const kind = r.String(*type, triggerAt + ".type", 16);
    if (kind == "at_start")
    {
        r.CheckKeys(trigger, triggerAt, { "type" });
        e.When = Event::Trigger::AtStart;
    }
    else if (kind == "after_boss")
    {
        r.CheckKeys(trigger, triggerAt, { "type", "boss" });
        e.When = Event::Trigger::AfterBoss;
        e.Boss = r.Uint(trigger, triggerAt, "boss", 1, 10000000);
    }
    else if (kind == "near")
    {
        r.CheckKeys(trigger, triggerAt, { "type", "x", "y", "z", "radius" });
        e.When = Event::Trigger::Near;
        e.X = r.Float(trigger, triggerAt, "x");
        e.Y = r.Float(trigger, triggerAt, "y");
        e.Z = r.Float(trigger, triggerAt, "z");
        e.Radius = r.Float(trigger, triggerAt, "radius");
        if (e.Radius <= 0.0f)
            r.Fail(triggerAt + ".radius", "must be more than 0");
    }
    else
        r.Fail(triggerAt + ".type", "'" + kind + "' is not near, after_boss or at_start");

    Value const* steps = value.Find("steps");
    if (!steps || !steps->IsArray() || steps->Items.empty())
        r.Fail(at + ".steps", "must be a list of at least one step");
    if (steps->Items.size() > 50)
        r.Fail(at + ".steps", "has more than 50 steps");
    for (std::size_t i = 0; i < steps->Items.size(); ++i)
    {
        Value const& step = steps->Items[i];
        std::string const stepAt = at + ".steps[" + std::to_string(i) + "]";
        if (!step.IsObject())
            r.Fail(stepAt, "must be an object { \"type\": \"use\" .. } or { \"type\": \"wait\" .. }");
        Value const* stepType = step.Find("type");
        if (!stepType)
            r.Fail(stepAt, "the key 'type' is missing (use or wait)");
        std::string const stepKind = r.String(*stepType, stepAt + ".type", 16);
        Step s;
        if (stepKind == "use")
        {
            r.CheckKeys(step, stepAt, { "type", "object", "x", "y", "z", "item" });
            s.What = Step::Kind::Use;
            s.Object = r.Uint(step, stepAt, "object", 1, 100000000);
            s.X = r.Float(step, stepAt, "x");
            s.Y = r.Float(step, stepAt, "y");
            s.Z = r.Float(step, stepAt, "z");
            if (Value const* item = step.Find("item"))
                s.Item = r.UintOf(*item, stepAt + ".item", 1, 100000000);
        }
        else if (stepKind == "wait")
        {
            r.CheckKeys(step, stepAt, { "type", "seconds" });
            s.What = Step::Kind::Wait;
            s.WaitSec = r.Uint(step, stepAt, "seconds", 0, 600);
        }
        else
            r.Fail(stepAt + ".type", "'" + stepKind + "' is not use or wait");
        e.Steps.push_back(s);
    }
    if (Value const* timeout = value.Find("timeout_sec"))
        e.TimeoutSec = r.UintOf(*timeout, at + ".timeout_sec", 5, 3600);
    return e;
}

std::string ReadFile(std::filesystem::path const& path, bool& ok)
{
    std::ifstream in(path, std::ios::binary);
    ok = bool(in);
    std::ostringstream text;
    if (in)
        text << in.rdbuf();
    return text.str();
}

std::string ParentOf(std::string const& file)
{
    return std::filesystem::path(file).parent_path().string();
}

} // namespace

bool ParseDungeonJson(std::string_view text, std::string const& source, std::optional<std::uint32_t> fileMap, Dungeon& out, std::string& error,
                      std::vector<std::string>& warnings)
{
    mini_json::ParseResult const parsed = mini_json::Parse(text);
    if (!parsed.Ok)
    {
        error = parsed.Error;
        return false;
    }
    Reader r;
    r.Source = source;
    r.Warnings = &warnings;
    try
    {
        Value const& root = parsed.Root;
        if (!root.IsObject())
            r.Fail("", "the file must hold one object { ... }");
        r.CheckKeys(root, "", { "schema", "id", "map", "name", "levels", "roster", "entrance", "run", "route", "events", "bosses", "kill_priority", "notes" });
        Value const* schema = root.Find("schema");
        if (!schema)
            r.Fail("", "the key \"schema\" is missing (this module reads schema 1)");
        std::uint32_t schemaNumber = 0;
        if (!mini_json::ToUint(*schema, schemaNumber, 1000) || schemaNumber != std::uint32_t(kDungeonSchema))
            r.Fail("schema", "is not 1; this module reads schema 1 only");

        Dungeon d;
        d.Source = source;
        Value const* id = root.Find("id");
        if (!id)
            r.Fail("", "the key \"id\" is missing (the word that Dungeon = <id> picks)");
        d.Id = r.String(*id, "id", 32);
        if (!IdOk(d.Id, 32))
            r.Fail("id", "is one word of letters, digits, '-' and '_'");
        std::uint32_t const map = r.Uint(root, "", "map", 1, 100000);
        if (fileMap && *fileMap != map)
            r.Fail("map", "is " + std::to_string(map) + " but the file name says map " + std::to_string(*fileMap));
        if (Value const* name = root.Find("name"))
            d.Name = Squash(r.String(*name, "name", 80));
        if (Value const* levels = root.Find("levels"))
        {
            if (!levels->IsArray() || levels->Items.size() != 2)
                r.Fail("levels", "must be [ minimum, maximum ]");
            d.MinLevel = r.UintOf(levels->Items[0], "levels[0]", 1, 60);
            d.MaxLevel = r.UintOf(levels->Items[1], "levels[1]", 1, 60);
            if (d.MaxLevel < d.MinLevel)
                r.Fail("levels", "the maximum is below the minimum");
        }
        if (Value const* priority = root.Find("kill_priority"))
            d.KillPriority = r.UintList(*priority, "kill_priority", 64);
        Value const* roster = root.Find("roster");
        if (!roster)
            r.Fail("", "the key \"roster\" is missing (race:class of the tank, healer and dps, for example \"2:1,5:5,8:8,2:9,5:4\")");
        d.Roster = r.String(*roster, "roster", 100);
        if (!DungeonRosterOk(d.Roster))
            r.Fail("roster", "is not 1 to 5 race:class pairs separated by commas");

        Value const& entrance = r.Object(root, "", "entrance", true);
        r.CheckKeys(entrance, "entrance", { "staging", "portal", "entry" });
        Value const* entry = entrance.Find("entry");
        if (!entry)
            r.Fail("entrance", "the key 'entry' is missing (where the party is teleported to inside)");
        d.Entry = r.Point4(*entry, "entrance.entry");
        if (std::uint32_t(d.Entry[0]) != map)
            r.Fail("entrance.entry.map", "is " + std::to_string(std::uint32_t(d.Entry[0])) + " but the dungeon's map is " + std::to_string(map));
        if (Value const* staging = entrance.Find("staging"))
            d.Staging = r.Point4(*staging, "entrance.staging");
        if (Value const* portal = entrance.Find("portal"))
            d.Portal = r.Point4(*portal, "entrance.portal");

        if (Value const* run = root.Find("run"))
        {
            if (!run->IsObject())
                r.Fail("run", "must be an object { ... }");
            r.CheckKeys(*run, "run", { "duration_sec", "wipe_recovery_sec", "min_alive", "leader_dead_abandon_sec" });
            if (Value const* v = run->Find("duration_sec"))
                d.DurationSec = r.UintOf(*v, "run.duration_sec", 0, 1000000);
            if (Value const* v = run->Find("wipe_recovery_sec"))
                d.WipeRecoverySec = r.UintOf(*v, "run.wipe_recovery_sec", 0, 1000000);
            if (Value const* v = run->Find("min_alive"))
                d.MinAlive = r.UintOf(*v, "run.min_alive", 0, 5);
            if (Value const* v = run->Find("leader_dead_abandon_sec"))
                d.LeaderDeadAbandonSec = r.UintOf(*v, "run.leader_dead_abandon_sec", 0, 1000000);
        }

        // bosses first: the route names its boss points by entry, and the names come from here
        std::map<std::uint32_t, std::string> bossNames;
        if (Value const* bosses = root.Find("bosses"))
        {
            if (!bosses->IsArray())
                r.Fail("bosses", "must be a list [ { \"entry\": .., \"name\": \"..\" } ]");
            if (bosses->Items.size() > 50)
                r.Fail("bosses", "has more than 50 bosses");
            for (std::size_t i = 0; i < bosses->Items.size(); ++i)
            {
                Value const& boss = bosses->Items[i];
                std::string const at = "bosses[" + std::to_string(i) + "]";
                if (!boss.IsObject())
                    r.Fail(at, "must be an object { \"entry\": .., \"name\": \"..\" }");
                r.CheckKeys(boss, at, { "entry", "name", "strategy" });
                DungeonBoss b;
                b.Entry = r.Uint(boss, at, "entry", 1, 100000000);
                if (bossNames.count(b.Entry))
                    r.Fail(at + ".entry", "the boss " + std::to_string(b.Entry) + " is listed twice");
                std::string name;
                if (Value const* v = boss.Find("name"))
                    name = r.String(*v, at + ".name", 80);
                b.Name = BossNameOf(name, b.Entry);
                if (Value const* strategy = boss.Find("strategy"))
                    b.Strategy = ReadStrategy(r, *strategy, at + ".strategy");
                bossNames[b.Entry] = b.Name;
                d.Bosses.push_back(std::move(b));
            }
        }

        Value const* route = root.Find("route");
        if (!route || !route->IsArray() || route->Items.empty())
            r.Fail("route", "must be a list of at least one waypoint [ { \"x\": .., \"y\": .., \"z\": .. } ]");
        if (route->Items.size() > 300)
            r.Fail("route", "has more than 300 waypoints");
        for (std::size_t i = 0; i < route->Items.size(); ++i)
        {
            std::string const at = "route[" + std::to_string(i) + "]";
            auto const xyz = r.Point3(route->Items[i], at, { "boss" });
            DungeonWaypoint point;
            point.X = xyz[0], point.Y = xyz[1], point.Z = xyz[2];
            if (Value const* boss = route->Items[i].Find("boss"))
            {
                point.BossEntry = r.UintOf(*boss, at + ".boss", 1, 100000000);
                auto const named = bossNames.find(point.BossEntry);
                point.BossName = named != bossNames.end() ? named->second : "boss" + std::to_string(point.BossEntry);
            }
            d.Route.push_back(std::move(point));
        }

        if (Value const* events = root.Find("events"))
        {
            if (!events->IsArray())
                r.Fail("events", "must be a list [ { \"id\": \"..\", \"trigger\": { .. }, \"steps\": [ .. ] } ]");
            if (events->Items.size() > 100)
                r.Fail("events", "has more than 100 events");
            std::set<std::string> ids;
            for (std::size_t i = 0; i < events->Items.size(); ++i)
            {
                dungeon_events::Event e = ReadEvent(r, events->Items[i], "events[" + std::to_string(i) + "]", map);
                if (!ids.insert(e.Id).second)
                    r.Fail("events[" + std::to_string(i) + "].id", "the id '" + e.Id + "' is used twice in this file");
                d.Events.push_back(std::move(e));
            }
        }
        out = std::move(d);
        return true;
    }
    catch (FileError const& e)
    {
        error = e.Why;
        return false;
    }
}

std::vector<Dungeon> LoadDungeonDir(std::string const& dir, bool required, std::vector<std::string>& warnings)
{
    namespace fs = std::filesystem;
    std::vector<Dungeon> out;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec))
    {
        if (required && !dir.empty())
            warnings.push_back("dungeon folder '" + dir + "' cannot be read");
        return out;
    }
    std::vector<fs::path> files;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == ".json")
            files.push_back(it->path());
    std::sort(files.begin(), files.end());
    std::set<std::uint32_t> seen;
    for (fs::path const& path : files)
    {
        std::string const name = path.filename().string();
        std::size_t digits = 0;
        while (digits < name.size() && name[digits] >= '0' && name[digits] <= '9')
            ++digits;
        if (digits == 0 || digits > 6 || digits + 1 >= name.size() || name[digits] != '-')
        {
            warnings.push_back(path.string() + ": ignored; the name must be <map id>-<name>.json, for example 36-the-deadmines.json");
            continue;
        }
        std::uint32_t const fileMap = std::uint32_t(std::stoul(name.substr(0, digits)));
        bool readable = false;
        std::string const text = ReadFile(path, readable);
        if (!readable)
        {
            warnings.push_back(path.string() + ": cannot be read");
            continue;
        }
        Dungeon d;
        std::string error;
        if (!ParseDungeonJson(text, path.string(), fileMap, d, error, warnings))
        {
            warnings.push_back(path.string() + ": not used: " + error);
            continue;
        }
        if (!seen.insert(fileMap).second)
        {
            warnings.push_back(path.string() + ": ignored; another file in this folder already holds map " + std::to_string(fileMap));
            continue;
        }
        out.push_back(std::move(d));
    }
    return out;
}

DungeonSources SourcesFromConfig(std::unordered_map<std::string, std::string> const& config)
{
    DungeonSources sources;
    auto const get = [&config](char const* key) -> std::string
    {
        auto const it = config.find(key);
        return it == config.end() ? std::string() : it->second;
    };
    sources.File = get("DungeonFile");
    sources.EventsFile = get("DungeonEventsFile");
    sources.DataDir = get("DungeonDataDir");
    sources.OverrideDir = get("DungeonDir");
    return sources;
}

std::string ShippedDir(DungeonSources const& sources)
{
    if (!sources.DataDir.empty())
        return sources.DataDir;
    if (!sources.File.empty())
    {
        std::string const parent = ParentOf(sources.File);
        return parent.empty() ? "dungeons" : parent + "/dungeons";
    }
    return {};
}

DungeonSet LoadDungeonSet(DungeonSources const& sources, std::vector<std::string>& warnings)
{
    DungeonSet set;
    std::map<std::uint32_t, Dungeon> byMap;

    std::string const shippedDir = ShippedDir(sources);
    std::vector<Dungeon> const shipped = LoadDungeonDir(shippedDir, !sources.DataDir.empty(), warnings);
    for (Dungeon const& d : shipped)
        byMap[d.MapId()] = d;
    if (!shipped.empty())
        set.Log.push_back("dungeons: " + std::to_string(shipped.size()) + " from " + shippedDir);

    std::vector<Dungeon> const mine = LoadDungeonDir(sources.OverrideDir, true, warnings);
    for (Dungeon const& d : mine)
    {
        byMap[d.MapId()] = d; // your file replaces the shipped one for that map
        set.Log.push_back("dungeons: map " + std::to_string(d.MapId()) + " (" + d.Id + ") from " + d.Source + (shipped.empty() ? "" : " (replaces the shipped file)"));
    }

    // two files with one id on different maps: the Dungeon = <id> key would be ambiguous; the lower map id keeps it
    std::set<std::string> ids;
    for (auto it = byMap.begin(); it != byMap.end();)
    {
        if (!ids.insert(it->second.Id).second)
        {
            warnings.push_back(it->second.Source + ": the id '" + it->second.Id + "' is used by a dungeon with a lower map id already; this one is left out");
            it = byMap.erase(it);
        }
        else
            ++it;
    }

    std::set<std::uint32_t> jsonMaps;
    for (auto const& [map, d] : byMap)
        jsonMaps.insert(map);

    // the old text files: only for maps no JSON file holds
    std::vector<dungeon_events::Event> oldEvents;
    if (!sources.EventsFile.empty())
    {
        bool ok = false;
        std::string const text = ReadFile(sources.EventsFile, ok);
        if (!ok)
            warnings.push_back("Module.forever-bots.DungeonEventsFile: cannot read '" + sources.EventsFile + "'");
        else
            oldEvents = dungeon_events::Parse(text, warnings);
    }
    std::vector<Dungeon> oldDungeons;
    if (!sources.File.empty())
    {
        bool ok = false;
        std::string const text = ReadFile(sources.File, ok);
        if (!ok)
            warnings.push_back("Module.forever-bots.DungeonFile: cannot read '" + sources.File + "'; no dungeon run.");
        else
            oldDungeons = ParseDungeons(text, warnings);
    }
    std::size_t fromText = 0;
    for (Dungeon& d : oldDungeons)
    {
        if (jsonMaps.count(d.MapId()))
            continue;
        if (!ids.insert(d.Id).second)
        {
            warnings.push_back(sources.File + ": the id '" + d.Id + "' is used already; the block is left out");
            continue;
        }
        d.Source = sources.File;
        for (dungeon_events::Event const& e : oldEvents)
            if (e.Map == d.MapId())
                d.Events.push_back(e);
        byMap[d.MapId()] = std::move(d);
        ++fromText;
    }
    if (fromText != 0)
        set.Log.push_back("dungeons: " + std::to_string(fromText) + " from " + sources.File + " (no JSON file for those maps)");

    for (auto& [map, d] : byMap)
    {
        for (dungeon_events::Event const& e : d.Events)
            set.Events.push_back(e);
        set.Dungeons.push_back(std::move(d));
    }
    // events of the old file for maps that no dungeon of the set covers (a scenario can run on a map with an events table alone)
    std::set<std::uint32_t> covered;
    for (Dungeon const& d : set.Dungeons)
        covered.insert(d.MapId());
    for (dungeon_events::Event const& e : oldEvents)
        if (!covered.count(e.Map))
            set.Events.push_back(e);
    return set;
}
