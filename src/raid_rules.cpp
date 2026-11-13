// SPDX-License-Identifier: GPL-2.0-or-later
#include "raid_rules.h"

#include "text_number.h"

#include <cmath>

namespace raid_rules {

namespace {

std::vector<std::string> Split(std::string_view line)
{
    std::vector<std::string> field;
    std::size_t from = 0;
    while (true)
    {
        std::size_t const bar = line.find('|', from);
        field.emplace_back(line.substr(from, bar == std::string_view::npos ? std::string_view::npos : bar - from));
        if (bar == std::string_view::npos)
            break;
        from = bar + 1;
    }
    return field;
}

bool Id(std::string const& text, std::uint32_t& out, bool allowZero = false)
{
    return text_number::ParseWhole(text, out) && (allowZero || out != 0);
}

bool Radius(std::string const& text, float& out)
{
    return text_number::ParseDecimal(text, out) && std::isfinite(out) && out > 0.0f && out <= 200.0f;
}

bool Coordinate(std::string const& text, float& out)
{
    return text_number::ParseDecimal(text, out) && std::isfinite(out) && std::fabs(out) <= 100000.0f;
}

std::optional<DispelKind> KindOf(std::string const& text)
{
    if (text == "magic")
        return DispelKind::Magic;
    if (text == "curse")
        return DispelKind::Curse;
    if (text == "disease")
        return DispelKind::Disease;
    if (text == "poison")
        return DispelKind::Poison;
    return std::nullopt;
}

bool Yards(std::string const& text, float& out)
{
    return text_number::ParseDecimal(text, out) && std::isfinite(out) && out >= 2.0f && out <= 60.0f;
}

std::optional<std::uint8_t> WhoOf(std::string const& text)
{
    if (text == "all")
        return kWhoAll;
    if (text == "ranged")
        return kWhoRanged;
    if (text == "healers")
        return kWhoHealers;
    if (text == "melee")
        return kWhoMelee;
    return std::nullopt;
}

} // namespace

Table Parse(std::string_view text, std::vector<std::string>& errors)
{
    Table table;
    std::size_t lineNo = 0, at = 0;
    while (at <= text.size())
    {
        std::size_t end = text.find('\n', at);
        if (end == std::string_view::npos)
            end = text.size();
        std::string_view line = text.substr(at, end - at);
        at = end + 1;
        ++lineNo;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.remove_suffix(1);
        if (line.empty() || line.front() == '#')
            continue;
        std::vector<std::string> const f = Split(line);
        auto const bad = [&](std::string const& why) { errors.push_back("line " + std::to_string(lineNo) + ": " + why); };
        std::uint32_t map = 0;
        if (f.size() < 3 || !Id(f[1], map))
        {
            bad("expected 'kind|map|...'");
            continue;
        }
        std::string const& kind = f[0];
        std::uint32_t a = 0, b = 0, c = 0;
        float radius = 0.0f;
        if (kind == "boss")
        {
            if (!Id(f[2], a))
                bad("boss: entry is not a number");
            else
                table.Edit(map).Bosses[a] = f.size() > 3 ? f[3] : std::string();
        }
        else if (kind == "add")
        {
            if (f.size() < 4 || !Id(f[2], a) || !Id(f[3], b, true))
                bad("add: expected 'add|map|entry|rank|name'");
            else if (!table.Edit(map).Adds.emplace(a, b).second)
                bad("add: entry " + std::to_string(a) + " is listed twice; the first rank is kept");
        }
        else if (kind == "interrupt")
        {
            if (!Id(f[2], a))
                bad("interrupt: spell id is not a number");
            else
                table.Edit(map).InterruptFirst.insert(a);
        }
        else if (kind == "dispel")
        {
            auto const k = f.size() > 3 ? KindOf(f[3]) : std::nullopt;
            if (!Id(f[2], a) || !k)
                bad("dispel: expected 'dispel|map|aura_id|magic|curse|disease|poison|name'");
            else
                table.Edit(map).Dispels[a] = *k;
        }
        else if (kind == "avoid")
        {
            if (f.size() < 5 || (f[2] != "spell" && f[2] != "object") || !Id(f[3], a) || !Radius(f[4], radius))
                bad("avoid: expected 'avoid|map|spell|object|id|radius|name' (radius 0..200 yards, above 0)");
            else
                table.Edit(map).Avoid.push_back({ a, f[2] == "object", radius });
        }
        else if (kind == "swap")
        {
            if (f.size() < 5 || !Id(f[2], a) || !Id(f[3], b) || !Id(f[4], c))
                bad("swap: expected 'swap|map|boss_entry|aura_id|stacks|name'");
            else
                table.Edit(map).Swaps.push_back({ a, b, c });
        }
        else if (kind == "leash")
        {
            if (!Id(f[2], a) || a < 10 || a > 80)
                bad("leash: expected 'leash|map|yards|name' (yards 10..80)");
            else
                table.Edit(map).PullLeash = a;
        }
        else if (kind == "pullpos")
        {
            float x = 0.0f, y = 0.0f, z = 0.0f, yards = 0.0f;
            if (f.size() < 7 || !Id(f[2], a) || !Coordinate(f[3], x) || !Coordinate(f[4], y) || !Coordinate(f[5], z) || !Radius(f[6], yards) || yards < 2.0f || yards > 40.0f)
                bad("pullpos: expected 'pullpos|map|entry|x|y|z|yards|name' (yards 2..40)");
            else if (!table.Edit(map).PullPositions.emplace(a, PullPosition{ x, y, z, yards }).second)
                bad("pullpos: entry " + std::to_string(a) + " is listed twice; the first position is kept");
        }
        else if (kind == "noaoe")
        {
            if (!Id(f[2], a, true))
                bad("noaoe: boss entry is not a number");
            else
                table.Edit(map).NoAoe.insert(a);
        }
        else if (kind == "frontal")
        {
            if (f.size() < 4 || !Id(f[2], a) || !Id(f[3], b))
                bad("frontal: expected 'frontal|map|boss_entry|spell_id|name'");
            else
                table.Edit(map).Frontal[a].insert(b);
        }
        else if (kind == "spread" || kind == "avoid_enemy")
        {
            auto const who = f.size() > 4 ? WhoOf(f[4]) : std::nullopt;
            bool const any = kind == "spread";
            if (f.size() < 5 || !Id(f[2], a, any) || !Yards(f[3], radius) || !who)
                bad(kind + ": expected '" + kind + "|map|" + (any ? "boss_entry (0 = any)" : "entry") + "|yards (2..60)|all/ranged/healers/melee|name'");
            else if (any)
                table.Edit(map).Spread.push_back({ a, radius, *who });
            else
                table.Edit(map).AvoidEnemy.push_back({ a, radius, *who });
        }
        else if (kind == "purge" || kind == "mindcontrol")
        {
            if (!Id(f[2], a))
                bad(kind + ": expected '" + kind + "|map|aura_spell_id|name'");
            else
                (kind == "purge" ? table.Edit(map).Purge : table.Edit(map).MindControl)[a] = f.size() > 3 ? f[3] : std::string();
        }
        else
            bad("unknown row kind '" + kind + "'");
    }
    return table;
}

void MergeKillPriority(Table const& table, std::uint32_t map, std::map<std::uint32_t, std::uint32_t>& killPriority)
{
    if (MapRules const* rules = table.Find(map))
        for (auto const& [entry, rank] : rules->Adds)
            killPriority.emplace(entry, rank);
}

MapRules const* Table::Find(std::uint32_t map) const
{
    auto const it = _maps.find(map);
    return it == _maps.end() ? nullptr : &it->second;
}

bool Table::IsBoss(std::uint32_t map, std::uint32_t entry) const
{
    MapRules const* r = Find(map);
    return r && r->Bosses.count(entry) != 0;
}

std::optional<std::uint32_t> Table::AddRank(std::uint32_t map, std::uint32_t entry) const
{
    MapRules const* r = Find(map);
    if (!r)
        return std::nullopt;
    auto const it = r->Adds.find(entry);
    if (it == r->Adds.end())
        return std::nullopt;
    return it->second;
}

bool Table::InterruptFirst(std::uint32_t map, std::uint32_t spell) const
{
    MapRules const* r = Find(map);
    return r && r->InterruptFirst.count(spell) != 0;
}

std::optional<DispelKind> Table::DispelKindOf(std::uint32_t map, std::uint32_t aura) const
{
    MapRules const* r = Find(map);
    if (!r)
        return std::nullopt;
    auto const it = r->Dispels.find(aura);
    if (it == r->Dispels.end())
        return std::nullopt;
    return it->second;
}

std::vector<AvoidZone> Table::AvoidOf(std::uint32_t map, bool objects) const
{
    std::vector<AvoidZone> out;
    if (MapRules const* r = Find(map))
        for (AvoidZone const& zone : r->Avoid)
            if (zone.Object == objects)
                out.push_back(zone);
    return out;
}

std::optional<std::uint32_t> Table::SwapStacks(std::uint32_t map, std::uint32_t aura) const
{
    if (MapRules const* r = Find(map))
        for (TankSwap const& swap : r->Swaps)
            if (swap.Aura == aura)
                return swap.Stacks;
    return std::nullopt;
}

std::uint32_t Table::PullLeash(std::uint32_t map) const
{
    MapRules const* r = Find(map);
    return r ? r->PullLeash : 0u;
}

std::optional<PullPosition> Table::PullPositionOf(std::uint32_t map, std::uint32_t entry) const
{
    if (MapRules const* r = Find(map))
        if (auto const it = r->PullPositions.find(entry); it != r->PullPositions.end())
            return it->second;
    return std::nullopt;
}

bool Table::NoAoe(std::uint32_t map, std::uint32_t bossEntry) const
{
    MapRules const* r = Find(map);
    return r && (r->NoAoe.count(0) != 0 || r->NoAoe.count(bossEntry) != 0);
}

bool Table::IsFrontalAny(std::uint32_t map, std::uint32_t spell) const
{
    MapRules const* r = Find(map);
    if (!r)
        return false;
    for (auto const& boss : r->Frontal)
        if (boss.second.count(spell) != 0)
            return true;
    return false;
}

bool Table::IsLethalSpell(std::uint32_t map, std::uint32_t spell) const
{
    MapRules const* r = Find(map);
    if (!r)
        return false;
    for (AvoidZone const& zone : r->Avoid)
        if (!zone.Object && zone.Id == spell)
            return true;
    return false;
}

std::vector<SpreadRule> Table::SpreadFor(std::uint32_t map, std::uint32_t entry) const
{
    std::vector<SpreadRule> out;
    if (MapRules const* r = Find(map))
        for (SpreadRule const& rule : r->Spread)
            if (rule.Entry == 0 || rule.Entry == entry)
                out.push_back(rule);
    return out;
}

std::vector<EnemyZone> Table::AvoidEnemyFor(std::uint32_t map, std::uint32_t entry) const
{
    std::vector<EnemyZone> out;
    if (MapRules const* r = Find(map))
        for (EnemyZone const& zone : r->AvoidEnemy)
            if (zone.Entry == entry)
                out.push_back(zone);
    return out;
}

bool Table::HasAvoidEnemy(std::uint32_t map) const
{
    MapRules const* r = Find(map);
    return r && !r->AvoidEnemy.empty();
}

bool Table::IsPurgeAura(std::uint32_t map, std::uint32_t aura) const
{
    MapRules const* r = Find(map);
    return r && r->Purge.count(aura) != 0;
}

bool Table::IsMindControlAura(std::uint32_t map, std::uint32_t aura) const
{
    MapRules const* r = Find(map);
    return r && r->MindControl.count(aura) != 0;
}

bool Table::IsFrontal(std::uint32_t map, std::uint32_t bossEntry, std::uint32_t spell) const
{
    MapRules const* r = Find(map);
    if (!r)
        return false;
    auto const it = r->Frontal.find(bossEntry);
    return it != r->Frontal.end() && it->second.count(spell) != 0;
}

} // namespace raid_rules
