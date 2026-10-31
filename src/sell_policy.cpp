// SPDX-License-Identifier: GPL-2.0-or-later
#include "sell_policy.h"

#include <algorithm>
#include <map>

namespace sell {

namespace {

constexpr std::uint8_t kCommon = 1;

} // namespace

bool Sellable(Item const& item)
{
    return item.Quality == kCommon && !item.QuestItem && !item.Equipment && item.VendorSellCopper > 0 && item.Count > 0;
}

bool VisitDue(Settings const& settings, bool hasSellable, std::uint64_t lastVisitMs, std::uint64_t nowMs)
{
    if (!settings.Enabled || !hasSellable)
        return false;
    return lastVisitMs == 0 || nowMs >= lastVisitMs + settings.EveryMs;
}

std::vector<Sale> PlanSales(Settings const& settings, std::vector<Item> const& items)
{
    // One sale per entry: the bot may carry several stacks of an item, and kSellToVendor names the entry, not a stack.
    std::map<std::uint32_t, Sale> byEntry;
    for (Item const& item : items)
    {
        if (!Sellable(item))
            continue;
        Sale& sale = byEntry[item.Entry];
        sale.Entry = item.Entry;
        sale.Count = 0;                         // 0 = every one the bot carries of it
        sale.Copper += item.VendorSellCopper * item.Count;
    }

    std::vector<Sale> sales;
    for (auto const& entry : byEntry)
        sales.push_back(entry.second);
    std::stable_sort(sales.begin(), sales.end(), [](Sale const& a, Sale const& b) { return a.Copper > b.Copper; });
    if (sales.size() > settings.MaxSalesPerVisit)
        sales.resize(settings.MaxSalesPerVisit);
    return sales;
}

bool VisitExpired(std::uint64_t visitStartMs, std::uint64_t nowMs)
{
    return nowMs >= visitStartMs + kVisitTimeoutMs;
}

Outcome Resolve(InFlight const& flight, std::uint32_t lastSequence, bool lastWasSale, bool applied, std::uint64_t nowMs)
{
    if (lastSequence != flight.Sequence && lastWasSale)
        return applied ? Outcome::kSold : Outcome::kRefused;
    if (nowMs >= flight.SentMs + kReplyTimeoutMs)
        return Outcome::kRefused;
    return Outcome::kWaiting;
}

} // namespace sell
