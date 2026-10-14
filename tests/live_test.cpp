// Unit tests for BotLive: the JSON for a hand-built snapshot, the unit cap and de-duplication, and the atomic file write.
#include "bot_live.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using forever_module::BotSnapshot;
using forever_module::NearbyUnit;
using forever_module::WorldTickSnapshotView;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

bool Has(std::string const& text, std::string const& part)
{
    return text.find(part) != std::string::npos;
}

WorldTickSnapshotView TwoBots()
{
    WorldTickSnapshotView view;
    view.tick_number = 7;
    view.max_nearby_units = 2;
    view.bots.resize(2);
    view.bots[0].guid = 11;
    view.bots[0].map_id = 36;
    view.bots[0].pos_x = -14.57f;
    view.bots[0].pos_y = -385.48f;
    view.bots[0].pos_z = 62.46f;
    view.bots[0].health = 90;
    view.bots[0].max_health = 100;
    view.bots[0].level = 18;
    view.bots[0].in_combat = 1;
    view.bots[0].group_role = 2;
    view.bots[0].nearby_count = 2;
    view.bots[1].guid = 12;
    view.bots[1].map_id = 36;
    view.bots[1].nearby_count = 1;
    view.nearby.resize(4);
    view.nearby[0].guid = 900;
    view.nearby[0].pos_x = 1.5f;
    view.nearby[0].is_hostile = true;
    view.nearby[0].elite = 1;
    view.nearby[1].guid = 901;
    view.nearby[2].guid = 900;      // the second bot sees the first bot's mob again: one entry in the file
    return view;
}

void TestBuild()
{
    std::string const json = BotLive::Build(TwoBots(), 123456, 100);
    CHECK(Has(json, "\"time_ms\":123456"));
    CHECK(Has(json, "\"tick\":7"));
    CHECK(Has(json, "\"guid\":11,\"map\":36,\"x\":-14.6,\"y\":-385.5,\"z\":62.5"));
    CHECK(Has(json, "\"level\":18,\"combat\":1"));
    CHECK(Has(json, "\"group\":2"));
    CHECK(Has(json, "\"hostile\":1,\"level\":0,\"elite\":1"));
    std::size_t count = 0;
    for (std::size_t at = json.find("\"guid\":900"); at != std::string::npos; at = json.find("\"guid\":900", at + 1))
        ++count;
    CHECK(count == 1);                                    // deduplicated per map
    CHECK(Has(json, "\"guid\":901"));
    CHECK(json.back() == '\n');
}

void TestUnitCap()
{
    std::string const json = BotLive::Build(TwoBots(), 1, 1);
    CHECK(Has(json, "\"guid\":900"));
    CHECK(!Has(json, "\"guid\":901"));
    CHECK(!Has(json, "\"units\":[]"));
    std::string const none = BotLive::Build(TwoBots(), 1, 0);
    CHECK(Has(none, "\"units\":[]"));
}

void TestEmptyAndOddFloats()
{
    WorldTickSnapshotView empty;
    std::string const json = BotLive::Build(empty, 5, 10);
    CHECK(Has(json, "\"bots\":[],\"units\":[]"));
    WorldTickSnapshotView odd = TwoBots();
    odd.bots[0].pos_x = 1e30f;                             // not a plausible position: written as 0, never as "inf" or "nan"
    odd.bots[0].pos_y = 0.0f / 0.0f;
    std::string const text = BotLive::Build(odd, 1, 10);
    CHECK(!Has(text, "inf") && !Has(text, "nan"));
}

void TestWriteAndRate()
{
    namespace fs = std::filesystem;
    fs::path const dir = fs::temp_directory_path() / "bot_live_test";
    fs::create_directories(dir);
    std::string const path = (dir / "live.json").string();
    fs::remove(path);

    BotLive live(BotLive::Options{ path, 1000, 400 });
    WorldTickSnapshotView const view = TwoBots();
    CHECK(live.Maybe(view, 5000, 1));                      // first call writes
    CHECK(!live.Maybe(view, 5500, 2));                     // too soon
    CHECK(live.Maybe(view, 6000, 3));                      // a second later
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    CHECK(Has(text.str(), "\"time_ms\":3"));
    CHECK(!fs::exists(path + ".tmp"));                     // the temporary file is gone

    BotLive off(BotLive::Options{});
    CHECK(!off.Enabled());
    CHECK(!off.Maybe(view, 1, 1));

    BotLive bad(BotLive::Options{ (dir / "no-such-folder" / "x.json").string(), 1000, 400 });
    CHECK(!bad.Maybe(view, 1, 1));                         // an unwritable path fails quietly
    fs::remove_all(dir);
}

} // namespace

int main()
{
    TestBuild();
    TestUnitCap();
    TestEmptyAndOddFloats();
    TestWriteAndRate();
    if (failures)
        std::printf("%d failure(s)\n", failures);
    else
        std::printf("live_test: all passed\n");
    return failures ? 1 : 0;
}
