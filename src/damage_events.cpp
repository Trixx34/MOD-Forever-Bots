// SPDX-License-Identifier: GPL-2.0-or-later
#include "damage_events.h"

#include <algorithm>

namespace damage_events
{

std::uint32_t HpPctBefore(Hit const& hit)
{
    if (hit.MaxHealth == 0)
        return 0;
    std::uint64_t const before = hit.Killing ? (hit.Amount > hit.Overkill ? hit.Amount - hit.Overkill : 0) : std::uint64_t(hit.HealthAfter) + hit.Amount;
    return std::uint32_t(std::min<std::uint64_t>(before * 100 / hit.MaxHealth, 100));
}

bool PassesMinPct(Hit const& hit, std::uint32_t minPct)
{
    if (hit.Killing || hit.MaxHealth == 0)
        return true;
    return std::uint64_t(hit.Amount) * 100 >= std::uint64_t(hit.MaxHealth) * minPct;
}

void HitLog::Add(Hit hit)
{
    auto it = _rings.find(hit.Victim);
    if (it == _rings.end())
    {
        if (_rings.size() >= kMaxVictims)
            return;
        it = _rings.emplace(hit.Victim, Ring{}).first;
    }
    Ring& ring = it->second;
    hit.Seq = ++_seq;
    ring.Hits[ring.Next] = hit;
    ring.Next = (ring.Next + 1) % kRingSize;
    ring.Count = std::min(ring.Count + 1, kRingSize);
}

std::vector<Hit> HitLog::Recent(std::uint64_t victim, std::size_t count) const
{
    std::vector<Hit> out;
    auto const it = _rings.find(victim);
    if (it == _rings.end())
        return out;
    Ring const& ring = it->second;
    for (std::size_t i = 0; i < ring.Count && out.size() < count; ++i)
        out.push_back(ring.Hits[(ring.Next + kRingSize - 1 - i) % kRingSize]);
    return out;
}

std::optional<Hit> HitLog::TakeKillingBlow(std::uint64_t victim, std::uint64_t nowMs)
{
    auto const it = _rings.find(victim);
    if (it == _rings.end())
        return std::nullopt;
    Ring& ring = it->second;
    for (std::size_t i = 0; i < ring.Count; ++i)
    {
        Hit const& hit = ring.Hits[(ring.Next + kRingSize - 1 - i) % kRingSize];
        if (hit.Seq <= ring.TakenSeq)
            break; // older than the blow taken last
        if (!hit.Killing)
            continue;
        if (nowMs > hit.AtMs && nowMs - hit.AtMs > kBlowFreshMs)
            return std::nullopt;
        ring.TakenSeq = _seq;
        return hit;
    }
    return std::nullopt;
}

std::string FormatLastHits(std::vector<Hit> const& hits, std::size_t count)
{
    std::string out;
    for (std::size_t i = 0; i < hits.size() && i < count; ++i)
    {
        if (!out.empty())
            out += ',';
        out += std::to_string(hits[i].Spell) + ":" + std::to_string(hits[i].Amount);
    }
    return out.empty() ? "none" : out;
}

std::string DiedFields(std::optional<Hit> const& blow, std::vector<Hit> const& recent)
{
    if (!blow)
        return " killer_spell=none killer_src=guess overkill=none hit_amount=none hit_hp_pct=none last_hits=" + FormatLastHits(recent);
    return " killer_spell=" + std::to_string(blow->Spell) + " killer_src=event overkill=" + std::to_string(blow->Overkill) + " hit_amount=" + std::to_string(blow->Amount) +
           " hit_hp_pct=" + std::to_string(HpPctBefore(*blow)) + " last_hits=" + FormatLastHits(recent);
}

void Aggregate::Add(Hit const& hit)
{
    auto const bump = [this, &hit](std::map<std::uint32_t, Stat>& map, std::uint32_t key)
    {
        auto it = map.find(key);
        if (it == map.end())
        {
            if (map.size() >= kMaxKeys)
            {
                ++_dropped;
                return;
            }
            it = map.emplace(key, Stat{}).first;
        }
        ++it->second.Hits;
        it->second.Damage += hit.Amount;
        if (hit.Killing)
        {
            ++it->second.Deaths;
            it->second.Overkill += hit.Overkill;
        }
    };
    bump(_bySpell, hit.Spell);
    bump(_byEntry, hit.Entry);
}

void Aggregate::Clear()
{
    _bySpell.clear();
    _byEntry.clear();
    _dropped = 0;
}

namespace
{

using Entry = std::pair<std::uint32_t, Stat>;

std::vector<Entry> Sorted(std::map<std::uint32_t, Stat> const& map, bool byDeaths)
{
    std::vector<Entry> out;
    for (auto const& pair : map)
        if (!byDeaths || pair.second.Deaths != 0)
            out.push_back(pair);
    std::stable_sort(out.begin(), out.end(), [byDeaths](Entry const& a, Entry const& b)
    {
        if (byDeaths && a.second.Deaths != b.second.Deaths)
            return a.second.Deaths > b.second.Deaths;
        return a.second.Damage > b.second.Damage; // equal: the map order (the lower key) stays
    });
    if (out.size() > kTopN)
        out.resize(kTopN);
    return out;
}

std::string List(std::vector<Entry> const& entries, bool byDeaths)
{
    std::string out;
    for (Entry const& e : entries)
    {
        if (!out.empty())
            out += ',';
        out += std::to_string(e.first) + ":";
        if (byDeaths)
            out += std::to_string(e.second.Deaths) + ":" + std::to_string(e.second.Overkill);
        else
            out += std::to_string(e.second.Damage) + ":" + std::to_string(e.second.Hits) + ":" + std::to_string(e.second.Deaths);
    }
    return out;
}

} // namespace

std::string Aggregate::Format() const
{
    if (Empty())
        return {};
    std::string out = " dmg_by_spell=" + List(Sorted(_bySpell, false), false) + " dmg_by_attacker=" + List(Sorted(_byEntry, false), false);
    std::vector<Entry> const deathSpells = Sorted(_bySpell, true);
    if (!deathSpells.empty())
        out += " death_by_spell=" + List(deathSpells, true) + " death_by_attacker=" + List(Sorted(_byEntry, true), true);
    return out;
}

} // namespace damage_events
