// SPDX-License-Identifier: GPL-2.0-or-later
#include "city_greet.h"

#include "bot_random.h"

namespace city_greet {

bool Greeter::Try(Settings const& settings, Facts const& facts, std::uint64_t seed, std::uint64_t bot, std::uint64_t player, std::uint32_t map,
                  std::uint64_t nowMs, std::uint32_t& draw)
{
    if (!settings.Enabled || !facts.Idle || !facts.InCapital || facts.Distance > settings.RangeYards)
        return false;
    if (auto const it = _playerMs.find(player); it != _playerMs.end() && nowMs - it->second < settings.PlayerCooldownMs)
        return false;
    std::uint64_t const n = _counter++;
    if (bot_random::Below(bot_random::Draw(seed, bot, 0xA1, n), 100) >= settings.ChancePct)
        return false;
    if (!_zone.TryGrant(map, nowMs, settings.ZoneMax))
        return false;
    _playerMs[player] = nowMs;
    if (_playerMs.size() > 4096)
        for (auto it = _playerMs.begin(); it != _playerMs.end();)
            it = nowMs - it->second >= settings.PlayerCooldownMs ? _playerMs.erase(it) : std::next(it);
    draw = bot_random::Below(bot_random::Draw(seed, bot, 0xA2, n), 1u << 20);
    return true;
}

}  // namespace city_greet
