// BotRoster: which bots this module controls, which requests are waiting for the core, and what was taken back.
// Pure bookkeeping with no SDK dependency, so it is unit-tested on its own (tests/roster_test.cpp).
#pragma once

#include <cstdint>
#include <set>
#include <string>

class BotRoster
{
public:
    explicit BotRoster(std::uint32_t maxBots);

    // Changes the cap, for a config reload. Bots already owned are kept even if the cap goes below their count.
    void SetMaxBots(std::uint32_t maxBots) { _maxBots = maxBots; }

    // Whether another control request may be sent. Counts the bots this module spawned plus requests still waiting for a reply.
    // Alts are not counted (see OnAltGranted).
    bool CanRequest() const;

    // Bots this module spawned: the owned bots minus the alts. This is what the cap and the spawn ramp count.
    std::uint32_t SpawnedCount() const;

    // Core granted control of an alt (a player's own character logged in as a bot). Alts do not use up the cap, and no pending
    // count is kept for them, so an alt add is never refused for the cap.
    void OnAltGranted(std::uint64_t grantedGuid);

    // Call when a RequestControl is sent. Use 0 for a spawn request; the guid arrives with the grant.
    void NoteRequestSent(std::uint64_t botGuid);

    // Core granted control. For a spawn (requestedGuid == 0) the granted guid is the new bot.
    void OnGranted(std::uint64_t requestedGuid, std::uint64_t grantedGuid);

    // Core refused a request. A spawn request has no guid to forget, so it is dropped as a pending spawn.
    void OnDenied(std::uint64_t requestedGuid);

    // Core took a bot back (ControlRevoked). Returns false if the bot was not owned.
    bool OnRevoked(std::uint64_t botGuid, std::string const& reason);

    std::set<std::uint64_t> const& Owned() const { return _owned; }
    std::uint32_t PendingCount() const { return _pending; }
    std::uint32_t MaxBots() const { return _maxBots; }
    std::string const& LastRevokeReason() const { return _lastRevokeReason; }

private:
    std::uint32_t _maxBots;
    std::uint32_t _pending = 0;      // requests sent and not yet answered
    std::set<std::uint64_t> _owned;  // every bot this module controls, alts included
    std::set<std::uint64_t> _alts;   // the subset of _owned that are alts
    std::string _lastRevokeReason;
};
