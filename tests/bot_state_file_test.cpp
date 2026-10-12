// Unit tests for BotStateFile. Uses a file in the system temp directory; no core needed.
#include "bot_state_file.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

std::filesystem::path TestPath(char const* name)
{
    auto dir = std::filesystem::temp_directory_path() / "forever-bots-state-test";
    std::filesystem::create_directories(dir);
    auto path = dir / name;
    std::filesystem::remove(path);
    return path;
}

void MissingFileIsEmpty()
{
    BotStateFile state(TestPath("missing.state").string());
    std::vector<std::string> warnings;
    std::string error;
    CHECK(state.Load(warnings, error));
    CHECK(state.Guids().empty());
    CHECK(warnings.empty());
}

void AddRemoveRoundTrip()
{
    auto path = TestPath("roundtrip.state");
    std::string error;
    {
        BotStateFile state(path.string());
        CHECK(state.Add(101, error));
        CHECK(state.Add(202, error));
        CHECK(state.Add(101, error)); // duplicate: not added twice
        CHECK(state.Guids().size() == 2);
        CHECK(state.Remove(101, error));
        CHECK(state.Guids().size() == 1 && state.Guids()[0] == 202);
    }
    BotStateFile reloaded(path.string());
    std::vector<std::string> warnings;
    CHECK(reloaded.Load(warnings, error));
    CHECK(reloaded.Guids().size() == 1 && reloaded.Guids()[0] == 202);
    CHECK(warnings.empty());
    CHECK(!std::filesystem::exists(path.string() + ".tmp")); // the temp file is gone after a save
}

void BadLinesAreSkipped()
{
    auto path = TestPath("bad.state");
    {
        std::ofstream out(path);
        out << "55\nnot-a-guid\n0\n\n55\n77 extra\n88\n";
    }
    BotStateFile state(path.string());
    std::vector<std::string> warnings;
    std::string error;
    CHECK(state.Load(warnings, error));
    CHECK(state.Guids().size() == 2 && state.Guids()[0] == 55 && state.Guids()[1] == 88); // 0 and garbage skipped, 55 once
    CHECK(warnings.size() == 3);
}

} // namespace

int main()
{
    MissingFileIsEmpty();
    AddRemoveRoundTrip();
    BadLinesAreSkipped();

    if (failures == 0)
        std::printf("bot_state_file_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
