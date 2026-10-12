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

    // Whether another control request may be sent. Counts owned bots plus requests still waiting for a reply.
    bool CanRequest() const;

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
    std::set<std::uint64_t> _owned;
    std::string _lastRevokeReason;
};
