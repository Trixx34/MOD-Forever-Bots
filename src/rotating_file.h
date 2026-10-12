// RotatingFile: an append-only text file that is rotated when it would pass a size limit. The current file is `path`;
// rotated files are `path.1` (newest) to `path.<keep>` (oldest, deleted on the next rotation). Lines are flushed as they
// are written, so a crash loses at most the line being written. Used for the telemetry CSV (tests/rotating_file_test.cpp).
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

class RotatingFile
{
public:
    RotatingFile(std::filesystem::path path, std::uint64_t maxBytes, std::uint32_t keep);

    // Whether the file is open for writing.
    bool IsOpen() const { return _out.is_open(); }

    // Appends the line and a newline, rotating first if the line would pass the limit.
    void WriteLine(std::string const& line);

    // Bytes in the current file, including what was there when it was opened.
    std::uint64_t CurrentBytes() const { return _bytes; }

private:
    void Rotate();
    void Open();

    std::filesystem::path _path;
    std::uint64_t _maxBytes;
    std::uint32_t _keep;
    std::uint64_t _bytes = 0;
    std::ofstream _out;
};
