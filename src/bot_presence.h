// SPDX-License-Identifier: GPL-2.0-or-later
// Bot presence: which of the module's ordinary bots are online, simulated like players who come and go. Each bot plays a session of a random
// length, logs off, stays away for a random time and comes back; how many are around follows the time of day. Pure logic that takes the time as
// input, so it is unit-tested without a clock (tests/presence_test.cpp). The module acts on its decisions: a log off is a ReleaseControl (the
// core logs the bot out when Bot.Presence.LogoutOnRelease is on) and a log on takes the bot back by its guid, as at a restart.
//
// Only ordinary bots take part. A bot in a party, an alt, or one that is busy (a fight, a command, a trade, a walk to a vendor) is never logged
// off, and does not count toward the minimum.
#pragma once

#include <array>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

namespace presence {

struct Settings
{
    bool Enabled = false;                      // config key Presence
    std::uint32_t OnlineMinMin = 60;           // a session lasts between these many minutes ... (PresenceOnlineMinMin, PresenceOnlineMaxMin)
    std::uint32_t OnlineMaxMin = 240;
    std::uint32_t OfflineMinMin = 30;          // ... and a bot stays away between these (PresenceOfflineMinMin, PresenceOfflineMaxMin)
    std::uint32_t OfflineMaxMin = 240;
    std::uint32_t MinOnline = 0;               // never fewer ordinary bots online than this (PresenceMinOnline)
    std::uint32_t MaxOnline = 0;               // never more than this at once; 0 = no cap (PresenceMaxOnline)
    std::uint32_t MaxChangesPerTick = 5;       // log offs plus log ons in one call: a crowd does not come and go on the same second
    std::uint64_t RetryMs = 30000;             // a bot that is due but busy is asked again after this
    std::uint64_t LeaveGraceMs = 60000;        // a bot told to log off may still be in view this long
    std::uint64_t ForgetAfterMs = 600000;      // a bot that was not away and has not been in view this long is no longer scheduled
    std::uint64_t LoginGraceMs = 300000;       // a bot sent back online has this long to show up before it counts as away again
    // The most of the population that may be around at each hour of the local day, in percent (PresenceDayPercent, 24 comma-separated values).
    // It is a ceiling: the random sessions and absences make the actual number fluctuate below it, and when it falls the soonest-due sessions end
    // early to get under it. The default has the evening busiest and the small hours quiet.
    std::array<std::uint32_t, 24> DayPercent = { 25, 20, 15, 12, 10, 10, 12, 18, 25, 32, 38, 42, 48, 52, 55, 60, 68, 78, 88, 95, 100, 95, 75, 45 };
};

// What the module knows about an ordinary bot that is online now.
struct Online
{
    std::uint64_t Guid = 0;
    bool Idle = true;     // nothing that a log off would cut short: no fight, party, command, trade, walk to a vendor
};

struct Decisions
{
    std::vector<std::uint64_t> LogOff;
    std::vector<std::uint64_t> LogOn;
};

class Scheduler
{
public:
    Scheduler(Settings const& settings, std::uint64_t seed);

    void SetSettings(Settings const& settings) { _settings = settings; }

    // One step. `online` lists the ordinary bots that are online and eligible now (alts and party members are left out by the caller).
    // `secOfDay` is local time since midnight. Returns the bots to log off and to bring back.
    Decisions Tick(std::uint64_t nowMs, std::uint32_t secOfDay, std::vector<Online> const& online);

    // A bot the module no longer controls for another reason (alt, party, revoked): it stops being scheduled.
    void Forget(std::uint64_t guid);

    // The bring-back of a bot failed (refused): it is tried again after RetryMs.
    void LogOnFailed(std::uint64_t guid, std::uint64_t nowMs);

    std::uint32_t OfflineCount() const;
    std::uint32_t TrackedCount() const { return std::uint32_t(_bots.size()); }

    // How many bots the hour calls for out of `population` (tested on its own).
    std::uint32_t AllowedOnline(std::uint32_t secOfDay, std::uint32_t population) const;

private:
    struct Entry
    {
        bool Away = false;          // logged off by the scheduler, waiting to come back
        bool Returning = false;     // sent back, not seen online yet
        std::uint64_t DueMs = 0;    // when the session ends (online) or the absence ends (away)
        std::uint64_t LeftMs = 0;   // when it was told to log off: it is still in view for a moment, and that is not a return
        std::uint64_t SeenMs = 0;   // when it was last in view; a bot gone for a long time that was never logged off is forgotten
    };

    std::uint64_t Minutes(std::uint32_t low, std::uint32_t high);

    Settings _settings;
    std::mt19937_64 _rng;
    std::unordered_map<std::uint64_t, Entry> _bots;
};

} // namespace presence
