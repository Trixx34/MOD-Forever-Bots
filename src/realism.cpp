// SPDX-License-Identifier: GPL-2.0-or-later
#include "realism.h"

#include "bot_random.h"

#include <algorithm>
#include <functional>

namespace realism {
namespace {

constexpr std::uint32_t kSaltFidgetGap = 0x71;
constexpr std::uint32_t kSaltFidgetRoll = 0x72;
constexpr std::uint32_t kSaltFidgetPick = 0x73;
constexpr std::uint32_t kSaltRead = 0x74;
constexpr std::uint32_t kSaltAfkRoll = 0x75;
constexpr std::uint32_t kSaltAfkLength = 0x76;

std::uint64_t InRange(std::uint64_t lo, std::uint64_t hi, std::uint64_t draw)
{
    if (hi < lo)
        hi = lo;
    return lo + draw % (hi - lo + 1);
}

} // namespace

Fidget NextFidget(FidgetSettings const& settings, FidgetState& state, std::uint64_t seed, std::uint64_t bot, std::uint64_t nowMs)
{
    if (!settings.Enabled)
        return Fidget::None;
    if (state.NextMs == 0)
    {
        // The first check of a wait is a random time after the start, so two bots that began together do not act together.
        state.NextMs = nowMs + InRange(settings.MinGapSec * 1000ull, settings.MaxGapSec * 1000ull, bot_random::Draw(seed, bot, kSaltFidgetGap, state.Counter));
        return Fidget::None;
    }
    if (nowMs < state.NextMs)
        return Fidget::None;
    std::uint32_t const n = state.Counter++;
    state.NextMs = nowMs + InRange(settings.MinGapSec * 1000ull, settings.MaxGapSec * 1000ull, bot_random::Draw(seed, bot, kSaltFidgetGap, state.Counter));
    if (bot_random::Below(bot_random::Draw(seed, bot, kSaltFidgetRoll, n), 100) >= std::min<std::uint32_t>(settings.ChancePct, 100))
        return Fidget::None;
    std::uint64_t const total = std::uint64_t(settings.StandWeight) + settings.StepWeight + settings.TurnWeight;
    if (total == 0)
        return Fidget::None;
    std::uint64_t pick = bot_random::Draw(seed, bot, kSaltFidgetPick, n) % total;
    if (pick < settings.StandWeight)
        return Fidget::Stand;
    pick -= settings.StandWeight;
    return pick < settings.StepWeight ? Fidget::Step : Fidget::Turn;
}

std::uint64_t ReadDelayMs(ReadSettings const& settings, std::uint64_t seed, std::uint64_t bot, std::uint64_t counter)
{
    std::uint64_t const lo = std::uint64_t(settings.MinSec) * 1000;
    std::uint64_t const hi = std::uint64_t(settings.MaxSec) * 1000;
    return InRange(lo, hi, bot_random::Draw(seed, bot, kSaltRead, counter));
}

bool ReadReady(ReadSettings const& settings, ReadGate& gate, std::uint64_t key, std::uint64_t seed, std::uint64_t bot, std::uint64_t nowMs)
{
    if (!settings.Enabled)
        return true;
    if (gate.OpenedMs == 0 || gate.Key != key)
    {
        gate.Key = key;
        gate.OpenedMs = nowMs == 0 ? 1 : nowMs;
        gate.DelayMs = ReadDelayMs(settings, seed, bot, key ^ (nowMs / 1000));
        return false;
    }
    return nowMs - gate.OpenedMs >= gate.DelayMs;
}

std::uint32_t ReserveMana(std::vector<std::uint32_t> costs, std::uint32_t casts)
{
    std::sort(costs.begin(), costs.end(), std::greater<>());
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < costs.size() && i < casts; ++i)
        sum += costs[i];
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(sum, 0xFFFFFFFFu));
}

bool BuffAllowed(BuffSettings const& settings, BuffFacts const& facts, std::uint64_t nowMs, std::uint64_t botNextMs)
{
    if (!settings.Enabled || facts.InCombat || facts.HasTarget || !facts.Alive || facts.Moving || facts.Casting)
        return false;
    if (!facts.ManaUser || facts.MaxPower == 0 || facts.BuffCost == 0)
        return false;
    if (facts.TargetDistance > settings.RangeYards)
        return false;
    if (nowMs < botNextMs)
        return false;
    if (std::uint64_t(facts.Power) * 100 < std::uint64_t(facts.MaxPower) * settings.ManaAbovePct)
        return false;
    // After the cast the bot must still be able to pay for its own fight rotation.
    return std::uint64_t(facts.Power) >= std::uint64_t(facts.BuffCost) + facts.RotationReserve;
}

bool ZoneRate::TryGrant(std::uint32_t zone, std::uint64_t nowMs, std::uint32_t perMinute)
{
    std::vector<std::uint64_t>& grants = _grants[zone];
    grants.erase(std::remove_if(grants.begin(), grants.end(), [nowMs](std::uint64_t at) { return nowMs - at >= 60000; }), grants.end());
    if (grants.size() >= perMinute)
        return false;
    grants.push_back(nowMs);
    return true;
}

std::uint64_t AfkDurationMs(AfkSettings const& settings, std::uint64_t seed, std::uint64_t bot, std::uint64_t counter)
{
    return InRange(std::uint64_t(settings.MinMin) * 60000, std::uint64_t(settings.MaxMin) * 60000, bot_random::Draw(seed, bot, kSaltAfkLength, counter));
}

bool AfkRoll(AfkSettings const& settings, std::uint64_t seed, std::uint64_t bot, std::uint64_t counter)
{
    return settings.Enabled && bot_random::Below(bot_random::Draw(seed, bot, kSaltAfkRoll, counter), 100) < std::min<std::uint32_t>(settings.ChancePct, 100);
}

bool AfkZones::TryStart(std::uint32_t zone, std::uint64_t bot, std::uint64_t nowMs, std::uint64_t untilMs, std::uint32_t zoneMax)
{
    std::map<std::uint64_t, std::uint64_t>& stays = _stays[zone];
    for (auto it = stays.begin(); it != stays.end();)
        it = it->second <= nowMs ? stays.erase(it) : std::next(it);
    if (stays.size() >= zoneMax && stays.count(bot) == 0)
        return false;
    stays[bot] = untilMs;
    return true;
}

void AfkZones::End(std::uint64_t bot)
{
    for (auto& zone : _stays)
        zone.second.erase(bot);
}

std::size_t AfkZones::Count(std::uint32_t zone, std::uint64_t nowMs)
{
    std::size_t n = 0;
    auto const at = _stays.find(zone);
    if (at != _stays.end())
        for (auto const& stay : at->second)
            n += stay.second > nowMs ? 1 : 0;
    return n;
}

} // namespace realism
