// SPDX-License-Identifier: GPL-2.0-or-later
// BotLive: a small JSON picture of where the controlled bots are and what is near them, rewritten in place every second or
// so, for a live map (the dashboard reads it). Pure logic plus one atomic file write; unit-tested in tests/live_test.cpp.
//
// The file holds: time (Unix ms), tick, and "bots": one entry per controlled bot (guid, map, position, facing, health,
// level, combat flag, threat target, party role) and "units": the creatures and players the bots see (deduplicated per
// map, capped), so a map can show what a party is fighting. Nothing here changes the game; the file can be deleted at any time.
#pragma once

#include "forever_module/tier2_session.h"

#include <cstdint>
#include <string>

class BotLive
{
public:
    struct Options
    {
        std::string Path;                    // empty: off
        std::uint64_t EveryMs = 1000;        // at most one write this often
        std::size_t MaxUnits = 400;          // nearby units kept in the file
    };

    explicit BotLive(Options options) : _options(std::move(options)) {}

    bool Enabled() const { return !_options.Path.empty(); }

    // Writes the file if EveryMs has passed since the last write. nowMs is the monotonic clock the policy uses; unixMs is
    // wall-clock time for the file. Returns true if a file was written.
    bool Maybe(forever_module::WorldTickSnapshotView const& view, std::uint64_t nowMs, std::uint64_t unixMs);

    // The JSON text for one snapshot. Public for tests.
    static std::string Build(forever_module::WorldTickSnapshotView const& view, std::uint64_t unixMs, std::size_t maxUnits);

    // Writes text to path through a temporary file and a rename, so a reader never sees half a file. False on failure.
    static bool WriteAtomic(std::string const& path, std::string const& text);

private:
    Options _options;
    bool _written = false;
    bool _failedLogged = false;
    std::uint64_t _lastMs = 0;
};
