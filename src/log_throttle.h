// SPDX-License-Identifier: GPL-2.0-or-later
// A per bot and reason throttle for repeating log lines that are not events (LogThrottleSec): one line per (bot, reason) per period, and the line that
// follows a held-back run says how many were held back. Header only; used on the Run() thread, no locking.
#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <string>
#include <utility>

namespace log_throttle
{

class KeyedThrottle
{
public:
    // True when the line for (bot, reason) is to be written now; `heldBefore` is then the number of lines held back for that key since the last one written.
    // False when it is held back (and counted). periodMs 0: always written, nothing held back.
    bool Allow(std::uint64_t bot, std::string const& reason, std::uint64_t nowMs, std::uint64_t periodMs, std::uint32_t& heldBefore)
    {
        heldBefore = 0;
        if (periodMs == 0)
            return true;
        if (_entries.size() >= kPruneAt)
            Prune(nowMs, periodMs);
        Entry& entry = _entries[{ bot, reason }];
        if (entry.Written && nowMs - entry.LastMs < periodMs)
        {
            ++entry.Held;
            return false;
        }
        heldBefore = entry.Held;
        entry.Held = 0;
        entry.Written = true;
        entry.LastMs = nowMs;
        return true;
    }

    // " suppressed=N" for the line that follows a held-back run; empty when none were held back.
    static std::string Field(std::uint32_t held) { return held == 0 ? std::string() : " suppressed=" + std::to_string(held); }

    std::size_t Size() const { return _entries.size(); }

private:
    static constexpr std::size_t kPruneAt = 4096; // bounded: keys older than the period (and with nothing held back) are dropped when this many are kept
    struct Entry
    {
        std::uint64_t LastMs = 0;
        std::uint32_t Held = 0;
        bool Written = false;
    };
    void Prune(std::uint64_t nowMs, std::uint64_t periodMs)
    {
        for (auto it = _entries.begin(); it != _entries.end();)
            it = (it->second.Held == 0 && nowMs - it->second.LastMs >= periodMs) ? _entries.erase(it) : std::next(it);
    }
    std::map<std::pair<std::uint64_t, std::string>, Entry> _entries;
};

} // namespace log_throttle
