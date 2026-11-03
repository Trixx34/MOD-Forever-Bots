// SPDX-License-Identifier: GPL-2.0-or-later
// Party mana sync (config PartyMana, off by default): a healer or caster of a dungeon party that is below 20 percent mana, out of combat, says one line in
// party chat, and the party's non-combat movement holds until that bot is at 80 percent or more, for at most PartyManaHoldSec. It never holds in combat.
// Pure logic, tests/party_mana_test.cpp. Lines: data/party_mana.txt.
#pragma once

#include <cstdint>
#include <map>

namespace party_mana {

struct Settings
{
    bool Enabled = false;                 // PartyMana
    std::uint32_t HoldSec = 60;           // PartyManaHoldSec: the hard cap of one hold
    std::uint32_t LowPct = 20;
    std::uint32_t ReadyPct = 80;
};

struct Facts
{
    bool InDungeonParty = false;
    bool ManaUser = false;                // a healer or a caster (power type mana)
    bool InCombat = false;
    std::uint32_t Power = 0;
    std::uint32_t MaxPower = 0;
};

// One tracker per party (keyed by the party leader).
class Tracker
{
public:
    // Feed each mana user of the party once per tick. Returns true exactly when this bot should say its line now (a new low-mana episode).
    bool Note(Settings const& settings, Facts const& facts, std::uint64_t bot, std::uint64_t nowMs);
    // Whether the party's non-combat movement holds now. Never during combat; ends at the cap.
    bool Holding(Settings const& settings, bool partyInCombat, std::uint64_t nowMs);

private:
    std::map<std::uint64_t, std::uint64_t> _low;   // bot -> when its hold began
    std::map<std::uint64_t, bool> _spoke;
};

}  // namespace party_mana
