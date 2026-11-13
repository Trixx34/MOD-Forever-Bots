// SPDX-License-Identifier: GPL-2.0-or-later
// The damage event (protocol 29, hook PlayerScript.OnDamaged): what the core says hit a bot, kept per bot in a ring of the last 16 hits and added up per party and per
// boss attempt for the run telemetry (run_telemetry.h). The core sends only killing blows and hits of at least Modules.DamageEvent.MinPct of the victim's max health,
// so these are the big hits, not a damage meter. Pure: no core or SDK types here (advisor_send.cpp decodes the event into a Hit).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace damage_events
{

constexpr std::size_t kRingSize = 16;         // hits kept per bot
constexpr std::size_t kMaxVictims = 4096;     // bots with a ring; beyond it the hits of a new bot are not kept
constexpr std::size_t kMaxKeys = 64;          // spells and attackers one Aggregate counts apart; further ones are dropped (counted in Dropped())
constexpr std::size_t kTopN = 5;              // entries of each list of Aggregate::Format
constexpr std::size_t kLastHitsInDied = 5;    // hits of last_hits= in the died: line
constexpr std::uint64_t kBlowFreshMs = 15000; // a killing blow older than this when the death is seen is not the cause of it

struct Hit
{
    std::uint64_t AtMs = 0;       // the module's clock when the event was taken in
    std::uint64_t Seq = 0;        // set by HitLog::Add
    std::uint64_t Victim = 0;     // raw guid counter of the bot
    std::uint64_t Attacker = 0;   // raw guid counter of the attacker (0 none)
    std::uint32_t Entry = 0;      // creature entry of the attacker (0 for a player, no attacker, a game object)
    std::uint32_t Spell = 0;      // 0 = melee or auto-attack
    std::uint32_t School = 0;
    std::uint32_t Amount = 0;
    std::uint32_t Overkill = 0;
    bool Killing = false;
    std::uint32_t HealthAfter = 0;
    std::uint32_t MaxHealth = 0;
};

// False for a non-lethal hit below `minPct` percent of the bot's max health (the module's own floor on top of the core's Modules.DamageEvent.MinPct). A killing blow, and a
// hit whose max health is unknown, always pass.
bool PassesMinPct(Hit const& hit, std::uint32_t minPct);

// The bot's health before the hit, in percent of its maximum (0 when the maximum is unknown, capped at 100).
std::uint32_t HpPctBefore(Hit const& hit);

// Hits of one bot, newest first in Recent(). Bounded to kMaxVictims bots; Forget() drops one (a bot that left).
class HitLog
{
public:
    void Add(Hit hit);
    std::vector<Hit> Recent(std::uint64_t victim, std::size_t count = kRingSize) const;
    // The newest killing blow on `victim` that is at most kBlowFreshMs old and was not taken by an earlier call (so a second death does not reuse it).
    std::optional<Hit> TakeKillingBlow(std::uint64_t victim, std::uint64_t nowMs);
    void Forget(std::uint64_t victim) { _rings.erase(victim); }
    std::size_t Victims() const { return _rings.size(); }

private:
    struct Ring
    {
        std::array<Hit, kRingSize> Hits{};
        std::size_t Next = 0;
        std::size_t Count = 0;
        std::uint64_t TakenSeq = 0;
    };
    std::unordered_map<std::uint64_t, Ring> _rings;
    std::uint64_t _seq = 0;
};

// "20566:3100,0:900" newest first, spell:amount, spell 0 is melee; "none" for no hits.
std::string FormatLastHits(std::vector<Hit> const& hits, std::size_t count = kLastHitsInDied);

// The fields the died: line gets from the event: " killer_spell=20566 killer_src=event overkill=250 hit_amount=3100 hit_hp_pct=62 last_hits=20566:3100,0:900".
// With no killing blow: " killer_spell=none killer_src=guess overkill=none hit_amount=none hit_hp_pct=none last_hits=..." (the guess is the old killer_entry= field).
std::string DiedFields(std::optional<Hit> const& blow, std::vector<Hit> const& recent);

struct Stat
{
    std::uint32_t Hits = 0;
    std::uint64_t Damage = 0;
    std::uint32_t Deaths = 0;
    std::uint64_t Overkill = 0;
};

// What hit a party in a window (a boss attempt, or 30 s of the timeline), by spell and by attacker entry.
class Aggregate
{
public:
    void Add(Hit const& hit);
    bool Empty() const { return _bySpell.empty() && _byEntry.empty(); }
    void Clear();
    std::uint32_t Dropped() const { return _dropped; }
    std::map<std::uint32_t, Stat> const& BySpell() const { return _bySpell; }
    std::map<std::uint32_t, Stat> const& ByEntry() const { return _byEntry; }
    // " dmg_by_spell=20566:3100:4:1,0:900:10:0 dmg_by_attacker=11502:5200:6:1 death_by_spell=20566:1:250 death_by_attacker=11502:1:250" (a leading space; empty when Empty()).
    // dmg_by_*: key:damage:hits:deaths, the top kTopN by damage. death_by_*: key:deaths:overkill, the top kTopN by deaths, only when there was a death. Spell 0 is melee.
    std::string Format() const;

private:
    std::map<std::uint32_t, Stat> _bySpell;
    std::map<std::uint32_t, Stat> _byEntry;
    std::uint32_t _dropped = 0;
};

} // namespace damage_events
