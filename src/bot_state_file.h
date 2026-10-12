// BotStateFile: the bot guids this module controls, kept across restarts so a restarted module can take its bots back
// instead of spawning new ones. One guid per line. Saves are atomic: the new contents go to a temp file, then replace the
// old file. Unit-tested in tests/bot_state_file_test.cpp.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

class BotStateFile
{
public:
    explicit BotStateFile(std::string path);

    // Reads the file. A missing file is an empty state, not an error. Lines that are not guids are skipped with a
    // warning. Returns false, with the reason, only if the file exists and cannot be read.
    bool Load(std::vector<std::string>& warnings, std::string& error);

    // Guids in the order they were added.
    std::vector<std::uint64_t> const& Guids() const { return _guids; }

    // Each change saves the file. Returns false if the save failed; the in-memory state still changes.
    bool Add(std::uint64_t guid, std::string& error);
    bool Remove(std::uint64_t guid, std::string& error);

    std::string const& Path() const { return _path; }

private:
    bool Save(std::string& error) const;

    std::string _path;
    std::vector<std::uint64_t> _guids;
};
