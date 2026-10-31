// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for RotatingFile, in a scratch directory.
#include "rotating_file.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

std::string Read(fs::path const& path)
{
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

fs::path Scratch(char const* name)
{
    fs::path dir = fs::temp_directory_path() / "forever-bots-rotating-file-test" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir / "telemetry.csv";
}

void RotatesAndKeepsTheNewestFiles()
{
    fs::path path = Scratch("rotates");
    {
        RotatingFile file(path, 20, 2); // 20 bytes per file: two 9-byte lines fit, a third starts a new file
        file.WriteLine("line-one-1"); // 11 bytes with the newline
        file.WriteLine("line-two-2"); // 22 bytes would pass 20: rotate first
        file.WriteLine("line-three"); // rotate again
        file.WriteLine("line-four-"); // rotate again: the oldest file is dropped (keep = 2)
    }
    CHECK(Read(path) == "line-four-\n");
    CHECK(Read(fs::path(path.string() + ".1")) == "line-three\n");
    CHECK(Read(fs::path(path.string() + ".2")) == "line-two-2\n");
    CHECK(!fs::exists(fs::path(path.string() + ".3")));
}

void ReopenAppendsAndCountsExistingBytes()
{
    fs::path path = Scratch("reopen");
    {
        RotatingFile file(path, 1000, 3);
        file.WriteLine("first");
    }
    RotatingFile again(path, 1000, 3);
    CHECK(again.CurrentBytes() == 6); // "first\n" is already there
    again.WriteLine("second");
    CHECK(Read(path) == "first\nsecond\n");
    // The count the rotation uses must match the bytes on disk, on every platform.
    CHECK(again.CurrentBytes() == fs::file_size(path));
}

void KeepZeroTruncates()
{
    fs::path path = Scratch("keep-zero");
    RotatingFile file(path, 12, 0);
    file.WriteLine("0123456789"); // 11 bytes
    file.WriteLine("abcdefghij"); // rotates: no history kept
    CHECK(Read(path) == "abcdefghij\n");
    CHECK(!fs::exists(fs::path(path.string() + ".1")));
}

void OversizedLineIsWrittenAlone()
{
    fs::path path = Scratch("oversized");
    RotatingFile file(path, 5, 1);
    file.WriteLine("x");
    file.WriteLine("this line is longer than the limit");
    CHECK(Read(path) == "this line is longer than the limit\n");
    CHECK(Read(fs::path(path.string() + ".1")) == "x\n");
}

} // namespace

int main()
{
    RotatesAndKeepsTheNewestFiles();
    ReopenAppendsAndCountsExistingBytes();
    KeepZeroTruncates();
    OversizedLineIsWrittenAlone();

    if (failures == 0)
        std::printf("rotating_file_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
