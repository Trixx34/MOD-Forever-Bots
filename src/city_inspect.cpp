// SPDX-License-Identifier: GPL-2.0-or-later
#include "city_inspect.h"

#include "bot_random.h"

namespace city_inspect {

bool Inspector::Try(Settings const& settings, Facts const& facts, std::uint64_t seed, std::uint64_t bot, std::uint64_t player, std::uint32_t map,
                    std::uint64_t nowMs, Emote& emote)
{
    if (!settings.Enabled || !facts.Idle || !facts.InCapital || facts.PlayerItemLevel == 0 || facts.PlayerItemLevel <= facts.BotItemLevel ||
        facts.Distance > settings.RangeYards)
        return false;
    if (auto const it = _playerMs.find(player); it != _playerMs.end() && nowMs - it->second < settings.PlayerCooldownMs)
        return false;
    std::uint64_t const n = _counter++;
    if (bot_random::Below(bot_random::Draw(seed, bot, 0x91, n), 100) >= settings.ChancePct)
        return false;
    if (!_zone.TryGrant(map, nowMs, settings.ZoneMax))
        return false;
    _playerMs[player] = nowMs;
    if (_playerMs.size() > 4096)
        for (auto it = _playerMs.begin(); it != _playerMs.end();)
            it = nowMs - it->second >= settings.PlayerCooldownMs ? _playerMs.erase(it) : std::next(it);
    emote = static_cast<Emote>(bot_random::Below(bot_random::Draw(seed, bot, 0x92, n), 3));
    return true;
}

}  // namespace city_inspect
