#include "bot_roster.h"

BotRoster::BotRoster(std::uint32_t maxBots) : _maxBots(maxBots)
{
}

bool BotRoster::CanRequest() const
{
    return _owned.size() + _pending < _maxBots;
}

void BotRoster::NoteRequestSent(std::uint64_t)
{
    ++_pending;
}

void BotRoster::OnGranted(std::uint64_t, std::uint64_t grantedGuid)
{
    if (_pending > 0)
        --_pending;
    if (grantedGuid != 0)
        _owned.insert(grantedGuid);
}

void BotRoster::OnDenied(std::uint64_t)
{
    if (_pending > 0)
        --_pending;
}

bool BotRoster::OnRevoked(std::uint64_t botGuid, std::string const& reason)
{
    if (_owned.erase(botGuid) == 0)
        return false;
    _lastRevokeReason = reason;
    return true;
}
