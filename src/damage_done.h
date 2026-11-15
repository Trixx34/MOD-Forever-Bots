// SPDX-License-Identifier: GPL-2.0-or-later
// Damage done by the bots of a party (estimate), for the run telemetry (DamageTelemetry). The snapshot has no outbound damage, so the health a target loses between
// two ticks is shared among the bots that have it as their target that tick. Healing a target (a rise) is not counted; a target seen for the first time, or after a
// gap, counts from the next tick. The sum over bots is the health the party took off its targets; the split between bots that share a target is equal (an estimate).
// Pure logic, unit-tested in tests/damage_done_test.cpp.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace damage_done
{

constexpr std::uint64_t kMaxGapMs = 5000; // between two observations of a target: a longer gap drops the baseline

struct Sample
{
    std::uint64_t Bot = 0;
    std::uint32_t Class = 0;
    std::uint64_t Target = 0;   // the unit the bot attacks (0 = none)
    std::uint32_t Health = 0;   // the target's health now
};

class Tracker
{
public:
    // One call per snapshot tick, with a sample for each living bot that has a target in view.
    void Observe(std::vector<Sample> const& samples, std::uint64_t nowMs);
    void Clear();                                   // forgets the totals (the target baselines stay)
    std::uint64_t Total() const;
    std::map<std::uint64_t, std::uint64_t> const& ByBot() const { return _byBot; }
    std::map<std::uint32_t, std::uint64_t> const& ByClass() const { return _byClass; }
    // " dmg_done_by_class=<class>:<damage>:<bots> ... dmg_done_by_bot=<bot>:<class>:<damage> ..." (leading space), empty when nothing was dealt.
    std::string Format() const;

private:
    struct Seen
    {
        std::uint32_t Health = 0;
        std::uint64_t AtMs = 0;
    };
    std::unordered_map<std::uint64_t, Seen> _seen;  // target -> its health at the last observation
    std::map<std::uint64_t, std::uint64_t> _byBot;
    std::map<std::uint32_t, std::uint64_t> _byClass;
    std::map<std::uint64_t, std::uint32_t> _classOfBot;
};

} // namespace damage_done
