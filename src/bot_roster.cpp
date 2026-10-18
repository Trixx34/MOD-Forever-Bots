#include "bot_roster.h"

BotRoster::BotRoster(std::uint32_t maxBots) : _maxBots(maxBots)
{
}

bool BotRoster::CanRequest() const
{
    return SpawnedCount() + _pending < _maxBots;
}

std::uint32_t BotRoster::SpawnedCount() const
{
    return static_cast<std::uint32_t>(_owned.size() - _alts.size() + _offline.size());
}

void BotRoster::NoteOffline(std::uint64_t botGuid)
{
    if (botGuid == 0 || _alts.count(botGuid) != 0)
        return;
    _owned.erase(botGuid);
    _offline.insert(botGuid);
}

void BotRoster::OnAltGranted(std::uint64_t grantedGuid)
{
    if (grantedGuid == 0)
        return;
    _owned.insert(grantedGuid);
    _alts.insert(grantedGuid);
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
    {
        _owned.insert(grantedGuid);
        _offline.erase(grantedGuid);
    }
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
    _alts.erase(botGuid);
    _lastRevokeReason = reason;
    return true;
}
