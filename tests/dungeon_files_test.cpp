// Unit tests for the per-dungeon JSON files (dungeon_files.h, mini_json.h): the reader, the checks, the override folder, and that the shipped files say what
// the old text files said. No core needed.
#include "dungeon_data.h"
#include "dungeon_events.h"
#include "dungeon_files.h"
#include "mini_json.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

namespace fs = std::filesystem;

std::string ReadAll(std::string const& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void Write(fs::path const& path, std::string const& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

fs::path FreshDir(char const* name)
{
    fs::path const dir = fs::temp_directory_path() / (std::string("forever-bots-dungeon-files-") + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

// A small dungeon on map 7 that uses every section, including the reserved strategy fields.
std::string Sample(std::string const& mapText = "7", std::string const& id = "tiny")
{
    return R"json({
  "schema": 1,
  "id": ")json" + id + R"json(",
  "map": )json" + mapText + R"json(,
  "name": "Tiny Hold",
  "levels": [10, 14],
  "roster": "2:1,5:5,8:8",
  "entrance": {
    "staging": { "map": 1, "x": 1, "y": 2, "z": 3 },
    "portal": { "map": 0, "x": 4.5, "y": 5.5, "z": 6.5 },
    "entry": { "map": 7, "x": -1.25, "y": -2.5, "z": 3.75 }
  },
  "run": { "duration_sec": 900, "wipe_recovery_sec": 120, "min_alive": 2, "leader_dead_abandon_sec": 300 },
  "route": [
    { "x": 10, "y": 20, "z": 30 },
    { "x": 11, "y": 21, "z": 31, "boss": 900 },
    { "x": 12, "y": 22, "z": 32 }
  ],
  "bosses": [
    { "entry": 900, "name": "Big, Bad Boss",
      "strategy": {
        "pull": "tank",
        "tank_position": { "x": 1, "y": 2, "z": 3 },
        "interrupt": [8984, 12345],
        "dispel": [700],
        "avoid": [ { "x": 5, "y": 6, "z": 7, "radius": 8, "note": "fire on the floor" } ],
        "kill_order": 1,
        "notes": "pull to the door"
      } }
  ],
  "events": [
    { "id": "gate", "name": "Gate  (lever)",
      "trigger": { "type": "near", "x": 9, "y": 9, "z": 9, "radius": 20 },
      "steps": [
        { "type": "use", "object": 555, "x": 9, "y": 9, "z": 9 },
        { "type": "wait", "seconds": 3 },
        { "type": "use", "object": 556, "x": 8, "y": 8, "z": 8, "item": 5397 }
      ],
      "timeout_sec": 60 },
    { "id": "after", "trigger": { "type": "after_boss", "boss": 900 }, "steps": [ { "type": "wait", "seconds": 1 } ] },
    { "id": "go", "trigger": { "type": "at_start" }, "steps": [ { "type": "wait", "seconds": 0 } ] }
  ]
})json";
}

bool Parses(std::string const& text, Dungeon& d, std::vector<std::string>& warnings, std::string& error, std::optional<std::uint32_t> fileMap = std::nullopt)
{
    return ParseDungeonJson(text, "sample.json", fileMap, d, error, warnings);
}

// ---- the JSON reader -------------------------------------------------------------------------------------------------------------------------

void JsonReader()
{
    auto const ok = mini_json::Parse(R"({"a": [1, -2.5e1, "x\nyé😀"], "b": {"c": true, "d": null}})");
    CHECK(ok.Ok);
    CHECK(ok.Root.Find("a") && ok.Root.Find("a")->Items.size() == 3);
    CHECK(ok.Root.Find("a")->Items[2].Text == "x\ny\xC3\xA9\xF0\x9F\x98\x80");
    CHECK(ok.Root.Find("b")->Find("c")->Boolean && ok.Root.Find("b")->Find("d")->What == mini_json::Value::Kind::Null);

    float f = 0;
    CHECK(mini_json::ToFloat(ok.Root.Find("a")->Items[1], f) && f == -25.0f);
    std::uint32_t u = 0;
    CHECK(mini_json::ToUint(ok.Root.Find("a")->Items[0], u) && u == 1);
    CHECK(!mini_json::ToUint(ok.Root.Find("a")->Items[1], u));       // a fraction or a negative number is not a whole number
    CHECK(!mini_json::ToUint(mini_json::Parse("[\"7\"]").Root.Items[0], u));                       // text is not a number

    auto const dup = mini_json::Parse(R"({"a": 1, "a": 2})");
    CHECK(!dup.Ok && dup.Error.find("appears twice") != std::string::npos);
    auto const broken = mini_json::Parse("{\n  \"a\": [1, 2,\n  ]\n}");
    CHECK(!broken.Ok && broken.Error.rfind("line 3", 0) == 0);        // the error says where
    CHECK(!mini_json::Parse("{\"a\": 01}").Ok);                       // a leading zero is not a JSON number
    CHECK(!mini_json::Parse("{\"a\": 1} x").Ok);
    CHECK(!mini_json::Parse("{\"a\": \"unclosed}").Ok);
    CHECK(!mini_json::Parse("").Ok);
    CHECK(!mini_json::Parse("[1, 2").Ok);
    CHECK(mini_json::Parse("\xEF\xBB\xBF{\"a\": 1}").Ok);            // a byte order mark from an editor
    std::string deep(40, '[');
    deep += std::string(40, ']');
    CHECK(!mini_json::Parse(deep).Ok);                                // too deep
}

// ---- one file --------------------------------------------------------------------------------------------------------------------------------

void ReadsAFileWithEverySection()
{
    Dungeon d;
    std::vector<std::string> warnings;
    std::string error;
    CHECK(Parses(Sample(), d, warnings, error, 7));
    CHECK(error.empty() && warnings.empty());
    CHECK(d.Id == "tiny" && d.Name == "Tiny Hold" && d.MapId() == 7 && d.Source == "sample.json");
    CHECK(d.MinLevel == 10 && d.MaxLevel == 14 && d.Roster == "2:1,5:5,8:8");
    CHECK(d.Staging && (*d.Staging)[0] == 1 && (*d.Staging)[3] == 3);
    CHECK(d.Portal && (*d.Portal)[1] == 4.5f);
    CHECK(d.Entry[0] == 7 && d.Entry[1] == -1.25f && d.Entry[3] == 3.75f);
    CHECK(d.DurationSec == 900 && d.WipeRecoverySec == 120 && d.MinAlive == 2 && d.LeaderDeadAbandonSec == 300);
    CHECK(d.Route.size() == 3 && d.Route[1].BossEntry == 900 && d.Route[1].BossName == "Big Bad Boss"); // the comma of the name is dropped, as in the text file
    CHECK(d.Route[0].BossEntry == 0 && d.Route[2].X == 12.0f);

    // the reserved strategy block is read and kept (the module does not act on it yet)
    CHECK(d.Bosses.size() == 1 && d.Bosses[0].Entry == 900);
    BossStrategy const& s = d.Bosses[0].Strategy;
    CHECK(s.Pull == "tank" && s.TankPosition && (*s.TankPosition)[2] == 3.0f);
    CHECK((s.Interrupt == std::vector<std::uint32_t>{ 8984, 12345 }) && (s.Dispel == std::vector<std::uint32_t>{ 700 }));
    CHECK(s.Avoid.size() == 1 && s.Avoid[0].Radius == 8.0f && s.Avoid[0].Note == "fire on the floor");
    CHECK(s.KillOrder == 1 && s.Notes == "pull to the door");

    CHECK(d.Events.size() == 3);
    dungeon_events::Event const& gate = d.Events[0];
    CHECK(gate.Map == 7 && gate.Id == "gate" && gate.Name == "Gate (lever)" && gate.When == dungeon_events::Event::Trigger::Near);
    CHECK(gate.X == 9.0f && gate.Radius == 20.0f && gate.TimeoutSec == 60 && gate.Steps.size() == 3);
    CHECK(gate.Steps[0].What == dungeon_events::Step::Kind::Use && gate.Steps[0].Object == 555 && gate.Steps[0].Item == 0);
    CHECK(gate.Steps[1].What == dungeon_events::Step::Kind::Wait && gate.Steps[1].WaitSec == 3);
    CHECK(gate.Steps[2].Item == 5397);
    CHECK(d.Events[1].When == dungeon_events::Event::Trigger::AfterBoss && d.Events[1].Boss == 900 && d.Events[1].TimeoutSec == 120);
    CHECK(d.Events[2].When == dungeon_events::Event::Trigger::AtStart);
}

void MinimalFileGetsTheDefaults()
{
    std::string const text = R"({"schema": 1, "id": "m", "map": 5, "roster": "2:1", "entrance": {"entry": {"map": 5, "x": 1, "y": 2, "z": 3}}, "route": [{"x": 0, "y": 0, "z": 0}]})";
    Dungeon d;
    std::vector<std::string> warnings;
    std::string error;
    CHECK(Parses(text, d, warnings, error));
    CHECK(d.MinLevel == 1 && d.MaxLevel == 60 && d.DurationSec == 1800 && d.WipeRecoverySec == 0 && !d.Staging && !d.Portal && d.Name.empty());
    CHECK(d.Bosses.empty() && d.Events.empty());
}

std::string Replace(std::string text, std::string const& from, std::string const& to)
{
    auto const at = text.find(from);
    if (at != std::string::npos)
        text.replace(at, from.size(), to);
    return text;
}

bool Rejects(std::string const& text, std::string const& wordInReason, std::optional<std::uint32_t> fileMap = std::nullopt)
{
    Dungeon d;
    std::vector<std::string> warnings;
    std::string error;
    if (Parses(text, d, warnings, error, fileMap))
        return false;
    if (error.find(wordInReason) == std::string::npos)
        std::printf("   (the reason was: %s)\n", error.c_str());
    return error.find(wordInReason) != std::string::npos;
}

void BadFilesAreRejectedWithAReason()
{
    CHECK(Rejects("not json", "line 1"));
    CHECK(Rejects("[1]", "one object"));
    CHECK(Rejects(Replace(Sample(), "\"schema\": 1,", ""), "schema"));
    CHECK(Rejects(Replace(Sample(), "\"schema\": 1", "\"schema\": 2"), "schema 1"));
    CHECK(Rejects(Replace(Sample(), "\"id\": \"tiny\",", ""), "\"id\""));
    CHECK(Rejects(Replace(Sample(), "\"tiny\"", "\"two words\""), "one word"));
    CHECK(Rejects(Replace(Sample(), "\"map\": 7,", ""), "'map'"));
    CHECK(Rejects(Sample(), "file name says map 8", 8));                                   // 8-xyz.json holding map 7
    CHECK(Rejects(Replace(Sample(), "\"roster\": \"2:1,5:5,8:8\"", "\"roster\": \"2-1\""), "race:class"));
    CHECK(Rejects(Replace(Sample(), "\"levels\": [10, 14]", "\"levels\": [14, 10]"), "below the minimum"));
    CHECK(Rejects(Replace(Sample(), "\"levels\": [10, 14]", "\"levels\": [0, 70]"), "levels[0]"));
    CHECK(Rejects(Replace(Sample(), "\"entry\": { \"map\": 7,", "\"entry\": { \"map\": 8,"), "entrance.entry.map"));
    CHECK(Rejects(Replace(Sample(), "\"x\": -1.25", "\"x\": \"left\""), "entrance.entry.x"));
    CHECK(Rejects(Replace(Sample(), "\"duration_sec\": 900", "\"duration_sec\": -5"), "run.duration_sec"));
    CHECK(Rejects(Replace(Sample(), "\"route\": [", "\"route\": [] , \"x\": ["), "route"));
    CHECK(Rejects(Replace(Sample(), "{ \"x\": 10, \"y\": 20, \"z\": 30 },", "{ \"x\": 10, \"y\": 20 },"), "route[0]"));
    CHECK(Rejects(Replace(Sample(), "\"boss\": 900 }", "\"boss\": 0 }"), "route[1].boss"));
    CHECK(Rejects(Replace(Sample(), "\"pull\": \"tank\"", "\"pull\": \"two words\""), "pull"));
    CHECK(Rejects(Replace(Sample(), "\"radius\": 8,", "\"radius\": 0,"), "radius"));
    CHECK(Rejects(Replace(Sample(), "\"interrupt\": [8984, 12345]", "\"interrupt\": [8984, \"x\"]"), "interrupt[1]"));
    CHECK(Rejects(Replace(Sample(), "\"type\": \"near\"", "\"type\": \"later\""), "not near"));
    CHECK(Rejects(Replace(Sample(), "\"radius\": 20", "\"radius\": 0"), "trigger.radius"));
    CHECK(Rejects(Replace(Sample(), "\"seconds\": 3", "\"seconds\": 601"), "seconds"));
    CHECK(Rejects(Replace(Sample(), "\"timeout_sec\": 60", "\"timeout_sec\": 1"), "timeout_sec"));
    CHECK(Rejects(Replace(Sample(), "\"id\": \"after\"", "\"id\": \"gate\""), "used twice"));
    CHECK(Rejects(Replace(Sample(), "\"steps\": [ { \"type\": \"wait\", \"seconds\": 1 } ]", "\"steps\": []"), "steps"));
    // a boss listed twice
    CHECK(Rejects(Replace(Sample(), "\"bosses\": [", "\"bosses\": [ { \"entry\": 900 },"), "listed twice"));
}

void UnknownKeysAreWarningsOnly()
{
    Dungeon d;
    std::vector<std::string> warnings;
    std::string error;
    CHECK(Parses(Replace(Sample(), "\"name\": \"Tiny Hold\",", "\"name\": \"Tiny Hold\", \"nmae\": 1,"), d, warnings, error));
    CHECK(warnings.size() == 1 && warnings[0].find("unknown key 'nmae'") != std::string::npos && warnings[0].rfind("sample.json", 0) == 0);
    CHECK(d.Name == "Tiny Hold");
}

// ---- folders and overrides -------------------------------------------------------------------------------------------------------------------

void FolderLoadingAndOverrides()
{
    fs::path const root = FreshDir("override");
    Write(root / "shipped" / "7-tiny.json", Sample("7", "tiny"));
    Write(root / "shipped" / "8-other.json", Replace(Replace(Sample("8", "other"), "\"entry\": { \"map\": 7,", "\"entry\": { \"map\": 8,"), "x", "x"));
    Write(root / "shipped" / "README.txt", "not a dungeon");
    Write(root / "shipped" / "notes.json", "{}");                                             // a name without a map id
    Write(root / "mine" / "7-tiny.json", Replace(Sample("7", "tiny"), "\"Tiny Hold\"", "\"My Tiny Hold\""));

    std::vector<std::string> warnings;
    auto const shipped = LoadDungeonDir((root / "shipped").string(), true, warnings);
    CHECK(shipped.size() == 2 && shipped[0].MapId() == 7 && shipped[1].MapId() == 8);
    CHECK(warnings.size() == 1 && warnings[0].find("notes.json") != std::string::npos && warnings[0].find("<map id>-<name>.json") != std::string::npos);

    // your folder wins for a map it has a file for, and only for that map
    DungeonSources sources;
    sources.DataDir = (root / "shipped").string();
    sources.OverrideDir = (root / "mine").string();
    warnings.clear();
    DungeonSet set = LoadDungeonSet(sources, warnings);
    CHECK(set.Dungeons.size() == 2);
    CHECK(set.Dungeons[0].MapId() == 7 && set.Dungeons[0].Name == "My Tiny Hold" && set.Dungeons[0].Source.find("mine") != std::string::npos);
    CHECK(set.Dungeons[1].MapId() == 8 && set.Dungeons[1].Name == "Tiny Hold");
    CHECK(set.Events.size() == 6);                                                            // three events of each dungeon

    // a file of yours with a mistake is not used: the shipped one stays, and the mistake is logged with the file name
    Write(root / "mine" / "7-tiny.json", Replace(Sample("7", "tiny"), "\"roster\": \"2:1,5:5,8:8\"", "\"roster\": \"oops\""));
    warnings.clear();
    set = LoadDungeonSet(sources, warnings);
    CHECK(set.Dungeons.size() == 2 && set.Dungeons[0].Name == "Tiny Hold");
    bool logged = false;
    for (std::string const& w : warnings)
        logged = logged || (w.find("7-tiny.json") != std::string::npos && w.find("not used") != std::string::npos && w.find("race:class") != std::string::npos);
    CHECK(logged);

    // a file of yours for a map the shipped files do not have adds a dungeon
    Write(root / "mine" / "9-new.json", Replace(Sample("9", "newone"), "\"entry\": { \"map\": 7,", "\"entry\": { \"map\": 9,"));
    warnings.clear();
    set = LoadDungeonSet(sources, warnings);
    CHECK(set.Dungeons.size() == 3 && set.Dungeons[2].Id == "newone");

    // a folder that is named but is not there is a warning; the shipped files still load
    sources.OverrideDir = (root / "nowhere").string();
    warnings.clear();
    set = LoadDungeonSet(sources, warnings);
    std::size_t unreadable = 0;
    for (std::string const& w : warnings)
        unreadable += w.find("cannot be read") != std::string::npos;
    CHECK(set.Dungeons.size() == 2 && unreadable == 1);

    // the shipped folder defaults to "dungeons" beside the old file, with no warning when it is not there
    DungeonSources beside;
    beside.File = (root / "data" / "dungeons.txt").string();
    CHECK(ShippedDir(beside) == (root / "data").string() + "/dungeons");
    warnings.clear();
    CHECK(LoadDungeonSet(beside, warnings).Dungeons.empty());
    CHECK(warnings.size() == 1);                                                              // only: the old file cannot be read
    fs::remove_all(root);
}

void OldTextFilesStayAFallback()
{
    fs::path const root = FreshDir("fallback");
    Write(root / "data" / "dungeons" / "7-tiny.json", Sample("7", "tiny"));
    Write(root / "data" / "dungeons.txt", R"(dungeon tiny
name The Old One
levels 1 5
roster 2:1
entry 7 0 0 0
go 1 1 1
end
dungeon oldmap
name Only In Text
levels 20 25
roster 2:1,5:5
entry 8 1 2 3
go 4 5 6
boss 77 7 8 9 Old Boss
end
)");
    Write(root / "data" / "dungeon_events.txt", "event 7 text-event From text\nat_start\nwait 1\nend\nevent 8 old-door Old door\nat_start\nwait 2\nend\nevent 99 orphan Orphan\nat_start\nwait 3\nend\n");
    DungeonSources sources;
    sources.File = (root / "data" / "dungeons.txt").string();
    sources.EventsFile = (root / "data" / "dungeon_events.txt").string();
    std::vector<std::string> warnings;
    DungeonSet const set = LoadDungeonSet(sources, warnings);
    CHECK(warnings.empty());
    CHECK(set.Dungeons.size() == 2);
    CHECK(set.Dungeons[0].MapId() == 7 && set.Dungeons[0].Name == "Tiny Hold");               // the JSON file wins over the text block for map 7
    CHECK(set.Dungeons[0].Events.size() == 3);                                                // and its events are its own: the text event for map 7 is not added
    CHECK(set.Dungeons[1].MapId() == 8 && set.Dungeons[1].Id == "oldmap" && set.Dungeons[1].Events.size() == 1 && set.Dungeons[1].Events[0].Id == "old-door");
    CHECK(set.Events.size() == 3 + 1 + 1);                                                    // 3 JSON events, the old door, and the orphan of map 99
    bool sawText = false;
    for (dungeon_events::Event const& e : set.Events)
        sawText = sawText || e.Id == "text-event";
    CHECK(!sawText);
    fs::remove_all(root);
}

// ---- the shipped files say what the old text files said --------------------------------------------------------------------------------------

void SameEvent(dungeon_events::Event const& a, dungeon_events::Event const& b)
{
    CHECK(a.Map == b.Map && a.Id == b.Id && a.Name == b.Name && a.When == b.When && a.Boss == b.Boss);
    CHECK(a.X == b.X && a.Y == b.Y && a.Z == b.Z && a.Radius == b.Radius && a.TimeoutSec == b.TimeoutSec);
    CHECK(a.Steps.size() == b.Steps.size());
    for (std::size_t i = 0; i < a.Steps.size() && i < b.Steps.size(); ++i)
    {
        CHECK(a.Steps[i].What == b.Steps[i].What && a.Steps[i].Object == b.Steps[i].Object && a.Steps[i].Item == b.Steps[i].Item);
        CHECK(a.Steps[i].X == b.Steps[i].X && a.Steps[i].Y == b.Steps[i].Y && a.Steps[i].Z == b.Steps[i].Z && a.Steps[i].WaitSec == b.Steps[i].WaitSec);
    }
}

void ShippedFilesEqualTheOldTextFiles()
{
    std::string const root = FOREVER_BOTS_SOURCE_DIR;
    std::string const oldText = ReadAll(root + "/data/dungeons.txt");
    std::string const oldEventsText = ReadAll(root + "/data/dungeon_events.txt");
    CHECK(!oldText.empty() && !oldEventsText.empty());

    std::vector<std::string> warnings;
    std::vector<Dungeon> const oldDungeons = ParseDungeons(oldText, warnings);
    std::vector<dungeon_events::Event> const oldEvents = dungeon_events::Parse(oldEventsText, warnings);
    std::vector<Dungeon> const json = LoadDungeonDir(root + "/data/dungeons", true, warnings);
    for (std::string const& w : warnings)
        std::printf("   warning: %s\n", w.c_str());
    CHECK(warnings.empty());
    CHECK(!oldDungeons.empty() && json.size() == oldDungeons.size());

    std::size_t eventsSeen = 0;
    for (Dungeon const& old : oldDungeons)
    {
        Dungeon const* found = nullptr;
        for (Dungeon const& d : json)
            if (d.MapId() == old.MapId())
                found = &d;
        CHECK(found != nullptr);
        if (!found)
            continue;
        CHECK(found->Id == old.Id && found->Name == old.Name && found->MinLevel == old.MinLevel && found->MaxLevel == old.MaxLevel);
        CHECK(found->Roster == old.Roster && found->Entry == old.Entry);
        CHECK(found->Staging.has_value() == old.Staging.has_value() && (!old.Staging || *found->Staging == *old.Staging));
        CHECK(found->Portal.has_value() == old.Portal.has_value() && (!old.Portal || *found->Portal == *old.Portal));
        CHECK(found->DurationSec == old.DurationSec && found->WipeRecoverySec == old.WipeRecoverySec);
        CHECK(found->MinAlive == old.MinAlive && found->LeaderDeadAbandonSec == old.LeaderDeadAbandonSec);
        CHECK(found->Route.size() == old.Route.size());
        for (std::size_t i = 0; i < old.Route.size() && i < found->Route.size(); ++i)
        {
            CHECK(found->Route[i].X == old.Route[i].X && found->Route[i].Y == old.Route[i].Y && found->Route[i].Z == old.Route[i].Z);
            CHECK(found->Route[i].BossEntry == old.Route[i].BossEntry && found->Route[i].BossName == old.Route[i].BossName);
        }
        // the boss block lists each boss of the route once, with an empty strategy until someone writes one
        std::set<std::uint32_t> bossEntries;
        for (DungeonWaypoint const& p : old.Route)
            if (p.BossEntry != 0)
                bossEntries.insert(p.BossEntry);
        CHECK(found->Bosses.size() == bossEntries.size());

        std::vector<dungeon_events::Event> const expected = dungeon_events::ForMap(oldEvents, old.MapId());
        CHECK(found->Events.size() == expected.size());
        for (std::size_t i = 0; i < expected.size() && i < found->Events.size(); ++i)
            SameEvent(found->Events[i], expected[i]);
        eventsSeen += expected.size();

        // what the module reads at startup from the config key Dungeon = <id> is the same, key for key
        std::unordered_map<std::string, std::string> const config{ { "Dungeon", old.Id } };
        std::vector<std::string> mergeWarnings;
        auto const fromText = MergeDungeon(config, oldText, mergeWarnings);
        auto const fromJson = MergeDungeonSet(config, json, mergeWarnings);
        CHECK(mergeWarnings.empty());
        CHECK(fromText == fromJson);
        CHECK(fromJson.size() > 10);
    }
    CHECK(eventsSeen == oldEvents.size());                                                    // every old event is in a dungeon file

    // and the whole set, as the module loads it with the old files still named: the JSON files win and nothing is lost
    DungeonSources sources;
    sources.File = root + "/data/dungeons.txt";
    sources.EventsFile = root + "/data/dungeon_events.txt";
    warnings.clear();
    DungeonSet const set = LoadDungeonSet(sources, warnings);
    CHECK(warnings.empty());
    CHECK(set.Dungeons.size() == oldDungeons.size() && set.Events.size() == oldEvents.size());
}

void ConfigKeysNameTheFolders()
{
    DungeonSources const s = SourcesFromConfig({ { "DungeonFile", "data/dungeons.txt" }, { "DungeonDir", "/my/dungeons" }, { "DungeonDataDir", "/ship" }, { "DungeonEventsFile", "e.txt" } });
    CHECK(s.File == "data/dungeons.txt" && s.OverrideDir == "/my/dungeons" && s.DataDir == "/ship" && s.EventsFile == "e.txt");
    CHECK(ShippedDir(s) == "/ship");
    CHECK(ShippedDir(SourcesFromConfig({ { "DungeonFile", "data/dungeons.txt" } })) == "data/dungeons");
    CHECK(ShippedDir(SourcesFromConfig({ { "DungeonFile", "dungeons.txt" } })) == "dungeons");
    CHECK(ShippedDir(SourcesFromConfig({})).empty());
    // MergeDungeonFile: the Dungeon key plus any one place is enough, and a folder of JSON files alone works
    fs::path const root = FreshDir("merge");
    Write(root / "7-tiny.json", Sample("7", "tiny"));
    std::vector<std::string> warnings;
    auto const merged = MergeDungeonFile({ { "Dungeon", "tiny" }, { "DungeonDir", root.string() } }, warnings);
    CHECK(warnings.empty());
    CHECK(merged.count("ScenarioRoute1") == 1 && merged.at("ScenarioRoster") == "2:1,5:5,8:8" && merged.at("ScenarioEntry").rfind("7 ", 0) == 0);
    CHECK(merged.at("ScenarioBosses") == "900:Big Bad Boss" && merged.at("ScenarioBossAt") == "2:900");
    auto const unchanged = MergeDungeonFile({ { "Dungeon", "tiny" } }, warnings);              // no place named: nothing happens
    CHECK(unchanged.size() == 1);
    fs::remove_all(root);
}

} // namespace

int main()
{
    JsonReader();
    ReadsAFileWithEverySection();
    MinimalFileGetsTheDefaults();
    BadFilesAreRejectedWithAReason();
    UnknownKeysAreWarningsOnly();
    FolderLoadingAndOverrides();
    OldTextFilesStayAFallback();
    ShippedFilesEqualTheOldTextFiles();
    ConfigKeysNameTheFolders();
    if (failures != 0)
    {
        std::printf("dungeon_files_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("dungeon_files_test: all checks passed\n");
    return 0;
}
