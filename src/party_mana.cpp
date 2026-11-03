// SPDX-License-Identifier: GPL-2.0-or-later
#include "party_mana.h"

namespace party_mana {

bool Tracker::Note(Settings const& settings, Facts const& facts, std::uint64_t bot, std::uint64_t nowMs)
{
    if (!settings.Enabled || !facts.InDungeonParty || !facts.ManaUser || facts.MaxPower == 0)
        return false;
    std::uint64_t const pct = std::uint64_t(facts.Power) * 100 / facts.MaxPower;
    auto const low = _low.find(bot);
    if (low != _low.end())
    {
        // Already waiting for this bot: it is ready again at ReadyPct (a bot in combat is not tracked further, the hold simply does not apply).
        if (pct >= settings.ReadyPct)
        {
            _low.erase(low);
            _spoke.erase(bot);
        }
        return false;
    }
    if (facts.InCombat || pct >= settings.LowPct)
        return false;
    _low[bot] = nowMs;
    bool const first = !_spoke[bot];
    _spoke[bot] = true;
    return first;
}

bool Tracker::Holding(Settings const& settings, bool partyInCombat, std::uint64_t nowMs)
{
    if (!settings.Enabled || partyInCombat || _low.empty())
        return false;
    // The hold runs from the first low bot; the cap counts from there so a second low bot cannot extend it.
    std::uint64_t since = UINT64_MAX;
    for (auto const& [bot, at] : _low)
        if (at < since)
            since = at;
    if (nowMs - since >= std::uint64_t(settings.HoldSec) * 1000)
    {
        // The cap is spent: forget the episode so the party moves on, and so those bots do not hold it again until they have recovered.
        for (auto& [bot, at] : _low)
            at = nowMs - std::uint64_t(settings.HoldSec) * 1000;
        return false;
    }
    return true;
}

}  // namespace party_mana
