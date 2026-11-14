// SPDX-License-Identifier: GPL-2.0-or-later
#include "mount_plan.h"

#include "text_number.h"
#include "text_split.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace mounts {

namespace {

bool ParseWhole64(std::string_view text, std::uint64_t& out)
{
    text = text_split::TrimBlanks(text);
    if (text.empty() || text.size() > 20)
        return false;
    std::uint64_t value = 0;
    for (char c : text)
    {
        if (c < '0' || c > '9')
            return false;
        value = value * 10 + std::uint64_t(c - '0');
    }
    out = value;
    return true;
}

bool ParseWhole32(std::string_view text, std::uint32_t& out)
{
    std::uint64_t value = 0;
    if (!ParseWhole64(text, value) || value > 0xFFFFFFFFull)
        return false;
    out = static_cast<std::uint32_t>(value);
    return true;
}

bool ParseFloat(std::string_view text, float& out)
{
    return text_number::ParseDecimal(std::string(text_split::TrimBlanks(text)), out) && std::isfinite(out);
}

} // namespace

Table ParseTable(std::string_view text, std::vector<std::string>& errors)
{
    Table table;
    std::size_t lineNo = 0;
    std::size_t start = 0;
    while (start <= text.size())
    {
        std::size_t const end = text.find('\n', start);
        std::string_view line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        start = end == std::string_view::npos ? text.size() + 1 : end + 1;
        ++lineNo;
        line = text_split::TrimBlanks(line);
        if (line.empty() || line.front() == '#')
            continue;
        auto const fail = [&](char const* why) { errors.push_back("line " + std::to_string(lineNo) + ": " + why); };
        auto const f = text_split::Split(line, '|');
        if (f[0] == "vendor")
        {
            Vendor v;
            auto const xyz = f.size() > 5 ? text_split::Split(text_split::TrimBlanks(f[5]), ' ') : std::vector<std::string_view>{};
            if (f.size() != 8 || !ParseWhole32(f[1], v.RaceMask) || v.RaceMask == 0 || !ParseWhole32(f[2], v.Map) || !ParseWhole32(f[3], v.Entry) || v.Entry == 0 ||
                !ParseWhole64(f[4], v.Guid) || xyz.size() != 3 || !ParseFloat(xyz[0], v.X) || !ParseFloat(xyz[1], v.Y) || !ParseFloat(xyz[2], v.Z))
            {
                fail("expected vendor|raceMask|map|entry|guid|x y z|name|subname");
                continue;
            }
            v.Name = std::string(text_split::TrimBlanks(f[6]));
            v.Subname = std::string(text_split::TrimBlanks(f[7]));
            table.Vendors.push_back(std::move(v));
        }
        else if (f[0] == "item")
        {
            Item item;
            if (f.size() != 7 || !ParseWhole32(f[1], item.VendorEntry) || !ParseWhole32(f[2], item.Entry) || item.Entry == 0 || !ParseWhole32(f[3], item.Level) ||
                !ParseWhole32(f[4], item.Skill) || !ParseWhole64(f[5], item.Copper) || item.Copper == 0)
            {
                fail("expected item|vendorEntry|itemEntry|level|skill|copper|name");
                continue;
            }
            item.Name = std::string(text_split::TrimBlanks(f[6]));
            table.Items.push_back(std::move(item));
        }
        else
            fail("a line starts with vendor or item");
    }
    // An item needs a vendor line.
    table.Items.erase(std::remove_if(table.Items.begin(), table.Items.end(),
                                     [&](Item const& item)
                                     {
                                         return std::none_of(table.Vendors.begin(), table.Vendors.end(), [&](Vendor const& v) { return v.Entry == item.VendorEntry; });
                                     }),
                      table.Items.end());
    return table;
}

std::uint32_t RaceBit(std::uint32_t race)
{
    return race >= 1 && race <= 8 ? (1u << (race - 1)) : 0u;
}

std::uint64_t Spendable(std::uint64_t copper, std::uint32_t level, std::uint64_t reservePerLevel)
{
    std::uint64_t const reserve = reservePerLevel * level;
    return copper > reserve ? copper - reserve : 0;
}

namespace
{

// The vendors a bot of this race may use on this map, with the nearest first.
Vendor const* VendorOf(Table const& table, std::uint32_t entry)
{
    for (Vendor const& v : table.Vendors)
        if (v.Entry == entry)
            return &v;
    return nullptr;
}

} // namespace

std::optional<Pick> ChooseMount(Table const& table, std::uint32_t race, std::uint32_t level, std::uint32_t ridingSkill, std::uint64_t spendable, std::uint32_t map,
                                float x, float y)
{
    std::uint32_t const bit = RaceBit(race);
    if (bit == 0)
        return std::nullopt;
    std::optional<Pick> best;
    float bestDist = 0.0f;
    for (std::size_t i = 0; i < table.Items.size(); ++i)
    {
        Item const& item = table.Items[i];
        if (item.Level > level || item.Skill > ridingSkill || item.Copper > spendable)
            continue;
        Vendor const* vendor = VendorOf(table, item.VendorEntry);
        if (!vendor || (vendor->RaceMask & bit) == 0 || vendor->Map != map)
            continue;
        float const dist = std::hypot(vendor->X - x, vendor->Y - y);
        bool better = !best;
        if (best)
        {
            Item const& cur = table.Items[best->Item];
            if (item.Level != cur.Level)
                better = item.Level > cur.Level;
            else if (item.Copper != cur.Copper)
                better = item.Copper < cur.Copper;
            else if (dist != bestDist)
                better = dist < bestDist;
            else
                better = item.Entry < cur.Entry;
        }
        if (better)
        {
            best = Pick{ i, static_cast<std::size_t>(vendor - table.Vendors.data()) };
            bestDist = dist;
        }
    }
    return best;
}

std::uint64_t CheapestMount(Table const& table, std::uint32_t race, std::uint32_t level, std::uint32_t ridingSkill)
{
    std::uint32_t const bit = RaceBit(race);
    std::uint64_t cheapest = 0;
    for (Item const& item : table.Items)
    {
        if (item.Level > level || item.Skill > ridingSkill)
            continue;
        Vendor const* vendor = VendorOf(table, item.VendorEntry);
        if (!vendor || (vendor->RaceMask & bit) == 0)
            continue;
        if (cheapest == 0 || item.Copper < cheapest)
            cheapest = item.Copper;
    }
    return cheapest;
}

std::uint32_t SkillAfterTraining(std::vector<riding::Rank> const& ranks, std::uint32_t level, std::uint32_t skill, std::uint64_t spendable)
{
    for (std::size_t i = 0; i < ranks.size(); ++i)
    {
        riding::Rank const& rank = ranks[i];
        if (level >= rank.MinLevel && skill < rank.Skill && spendable >= rank.CostCopper)
        {
            spendable -= rank.CostCopper;
            skill = rank.Skill;
        }
    }
    return skill;
}

Plan Decide(std::vector<riding::Rank> const& ranks, Table const& table, bool refused, std::uint32_t race, std::uint32_t level, std::uint32_t ridingSkill,
            std::uint64_t copper, std::uint64_t reservePerLevel, std::uint32_t map, float x, float y)
{
    Plan plan;
    if (!refused || RaceBit(race) == 0 || table.Items.empty())
        return plan;
    std::uint64_t const spendable = Spendable(copper, level, reservePerLevel);
    if (auto const rank = riding::NextRank(ranks, level, ridingSkill, spendable))
    {
        riding::Rank const& r = ranks[*rank];
        // The trip pays for the rank and still leaves the price of the cheapest mount the new rank allows; without a mount to buy the rank is no use.
        std::uint64_t const mount = CheapestMount(table, race, level, r.Skill);
        if (mount != 0 && spendable >= r.CostCopper + mount)
        {
            plan.What = Step::Train;
            plan.Rank = *rank;
            return plan;
        }
    }
    if (auto const pick = ChooseMount(table, race, level, ridingSkill, spendable, map, x, y))
    {
        plan.What = Step::Buy;
        plan.Mount = *pick;
    }
    return plan;
}

} // namespace mounts
