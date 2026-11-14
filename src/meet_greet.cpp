// SPDX-License-Identifier: GPL-2.0-or-later
#include "meet_greet.h"

#include "bot_random.h"

#include <algorithm>

namespace meet_greet {

namespace {
std::uint64_t PairKey(std::uint64_t a, std::uint64_t b)
{
    std::uint64_t const lo = std::min(a, b);
    std::uint64_t const hi = std::max(a, b);
    return lo * 0x9E3779B97F4A7C15ull ^ (hi + 0x7F4A7C15ull + (lo << 6));   // order-free; a rare collision only means one skipped greeting
}

bool Contains(std::deque<std::size_t> const& ring, std::size_t value)
{
    return std::find(ring.begin(), ring.end(), value) != ring.end();
}
}  // namespace

bool Greeter::Try(Settings const& settings, Facts const& facts, std::uint64_t seed, std::uint64_t bot, std::uint64_t other, std::uint32_t map,
                  std::uint64_t nowMs, Greeting& out)
{
    if (!settings.Enabled || !facts.Quiet || !facts.Free || facts.Distance > settings.RangeYards || bot == other)
        return false;
    if (auto const it = _botMs.find(bot); it != _botMs.end() && nowMs - it->second < std::uint64_t(settings.BotCooldownSec) * 1000)
        return false;
    std::uint64_t const pair = PairKey(bot, other);
    if (auto const it = _pairMs.find(pair); it != _pairMs.end() && nowMs - it->second < std::uint64_t(settings.PairCooldownSec) * 1000)
        return false;
    std::uint64_t const n = _counter++;
    if (bot_random::Below(bot_random::Draw(seed, bot, 0xB1, n), 100) >= settings.ChancePct)
        return false;
    if (!_zone.TryGrant(map, nowMs, settings.ZoneMax))
        return false;

    Greeting g;
    bool const emoteOnly = bot_random::Below(bot_random::Draw(seed, bot, 0xB2, n), 100) < settings.WavePct;
    if (emoteOnly)
    {
        g.Emote = true;
    }
    else
    {
        g.Say = true;
        g.Emote = bot_random::Below(bot_random::Draw(seed, bot, 0xB3, n), 100) >= settings.SayPct;   // SayPct below 100: some say-turns turn into an emote alone
        if (g.Emote)
            g.Say = false;
    }
    g.EmoteKind = bot_random::Below(bot_random::Draw(seed, bot, 0xB4, n), 5) == 0 ? 4 : 3;
    g.Draw = bot_random::Below(bot_random::Draw(seed, bot, 0xB5, n), 1u << 20);
    out = g;

    _botMs[bot] = nowMs;
    _pairMs[pair] = nowMs;
    Prune(settings, nowMs);
    return true;
}

std::size_t Greeter::PickLine(std::uint64_t bot, std::size_t poolSize, std::uint32_t draw)
{
    if (poolSize == 0)
        return 0;
    std::deque<std::size_t>& mine = _botLines[bot];
    std::size_t chosen = draw % poolSize;
    bool found = false;
    // First pass: not said by anyone lately; second: not said by this bot lately; third: anything but this bot's last line.
    for (int pass = 0; pass < 3 && !found; ++pass)
    {
        for (std::size_t i = 0; i < poolSize; ++i)
        {
            std::size_t const idx = (draw + i) % poolSize;
            bool const ok = pass == 0 ? !Contains(_recent, idx) && !Contains(mine, idx)
                          : pass == 1 ? !Contains(mine, idx)
                                      : mine.empty() || mine.back() != idx;
            if (ok)
            {
                chosen = idx;
                found = true;
                break;
            }
        }
    }
    mine.push_back(chosen);
    while (mine.size() > kBotMemory)
        mine.pop_front();
    _recent.push_back(chosen);
    while (_recent.size() > kGlobalMemory)
        _recent.pop_front();
    return chosen;
}

void Greeter::Prune(Settings const& settings, std::uint64_t nowMs)
{
    if (_pairMs.size() > kMaxTracked)
    {
        std::uint64_t const keep = std::uint64_t(settings.PairCooldownSec) * 1000;
        for (auto it = _pairMs.begin(); it != _pairMs.end();)
            it = nowMs - it->second >= keep ? _pairMs.erase(it) : std::next(it);
    }
    if (_botMs.size() > kMaxTracked)
    {
        std::uint64_t const keep = std::uint64_t(settings.BotCooldownSec) * 1000;
        for (auto it = _botMs.begin(); it != _botMs.end();)
            it = nowMs - it->second >= keep ? _botMs.erase(it) : std::next(it);
    }
    if (_botLines.size() > kMaxTracked)
        _botLines.clear();   // only the repeat memory is lost
}

}  // namespace meet_greet
