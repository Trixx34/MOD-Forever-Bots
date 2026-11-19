// SPDX-License-Identifier: GPL-2.0-or-later
// Fight-risk retreat (fight_risk.h).
#include "fight_risk.h"

#include <algorithm>

namespace fight_risk
{

namespace
{
constexpr float kNever = 1e9f;
}

void Watch::Reset()
{
    _count = 0;
    _fightSinceMs = 0;
}

void Watch::Note(std::uint64_t nowMs, std::uint32_t health, std::uint64_t target, std::uint32_t targetHealth)
{
    if (_count == 0)
        _fightSinceMs = nowMs;
    if (_count == kSamples)
    {
        std::copy(_samples.begin() + 1, _samples.end(), _samples.begin());
        --_count;
    }
    _samples[_count++] = Sample{ nowMs, health, target, targetHealth };
}

Estimate Watch::Judge(Options const& options, std::uint64_t nowMs, std::uint32_t healthPct, std::uint32_t hitDamage) const
{
    Estimate estimate;
    if (_count < 2 || nowMs < _fightSinceMs + kMinFightMs)
        return estimate;
    Sample const& last = _samples[_count - 1];
    // The oldest sample still inside the window (at least the one before the newest).
    std::size_t first = _count - 2;
    while (first > 0 && last.AtMs - _samples[first - 1].AtMs <= kWindowMs)
        --first;
    Sample const& from = _samples[first];
    float const seconds = float(last.AtMs - from.AtMs) / 1000.0f;
    if (seconds <= 0.0f)
        return estimate;
    estimate.Known = true;
    float const lost = from.Health > last.Health ? float(from.Health - last.Health) : 0.0f;
    estimate.InDps = std::max(lost / seconds, float(hitDamage) / (float(kWindowMs) / 1000.0f));
    // The target's loss: from the oldest sample of the window on the same target.
    float dealt = 0.0f;
    float dealtSeconds = 0.0f;
    if (last.Target != 0)
        for (std::size_t i = first; i + 1 < _count; ++i)
            if (_samples[i].Target == last.Target)
            {
                dealt = _samples[i].TargetHealth > last.TargetHealth ? float(_samples[i].TargetHealth - last.TargetHealth) : 0.0f;
                dealtSeconds = float(last.AtMs - _samples[i].AtMs) / 1000.0f;
                break;
            }
    estimate.OutDps = dealtSeconds > 0.0f ? dealt / dealtSeconds : 0.0f;
    estimate.TimeToDieS = estimate.InDps > 0.0f ? float(last.Health) / estimate.InDps : kNever;
    estimate.TimeToKillS = estimate.OutDps > 0.0f ? float(last.TargetHealth) / estimate.OutDps : kNever;
    estimate.Losing = healthPct <= kJudgeBelowHealthPct && estimate.InDps > 0.0f &&
                      estimate.TimeToDieS < estimate.TimeToKillS * float(options.MarginPct) / 100.0f;
    return estimate;
}

char const* StepName(Step step)
{
    switch (step)
    {
        case Step::kPotion: return "potion";
        case Step::kBandage: return "bandage";
        case Step::kDisengage: return "disengage";
        case Step::kTeleport: return "teleport";
        default: return "none";
    }
}

Step Next(Facts const& facts)
{
    if (!facts.Losing)
        return Step::kNone;
    if (facts.HasPotion && facts.PotionReady)
        return Step::kPotion;
    if (facts.HasBandage && !facts.Bandaged && !facts.HitRecently)
        return Step::kBandage;
    if (facts.Disengaging && facts.DisengageForMs >= kDisengageMs && facts.TeleportAllowed && facts.TeleportReady)
        return Step::kTeleport;
    return Step::kDisengage;
}

} // namespace fight_risk
