// SPDX-License-Identifier: GPL-2.0-or-later
#include "auction_policy.h"
#include "ah_batch.h"

#include <algorithm>
#include <string>

namespace auction {

std::uint32_t StartingBid(Settings const& settings, std::uint8_t quality, std::uint64_t vendorSellCopper, std::uint32_t attempt)
{
    std::uint32_t pct = 0;
    switch (quality)
    {
        case kUncommon: pct = settings.SellPctGreen; break;
        case kRare: pct = settings.SellPctBlue; break;
        case kEpic:
        case kLegendary: pct = settings.SellPctEpic; break;
        default: return 0;
    }
    if (vendorSellCopper < settings.MinVendorCopper)
        return 0;
    if (attempt >= 2)
        pct = std::uint32_t(std::uint64_t(pct) * settings.RelistPct / 100);   // the second listing is cheaper
    std::uint64_t const bid = vendorSellCopper * pct / 100;
    // Never below what a vendor pays: the floor of the market.
    std::uint64_t const floor = vendorSellCopper + 1;
    constexpr std::uint64_t kMaxBid = 4000000000ull;                           // the core takes 32-bit copper amounts
    return std::uint32_t(std::min(std::max(bid, floor), kMaxBid));
}

std::uint32_t BuyoutOf(Settings const& settings, std::uint32_t bid)
{
    std::uint64_t const buyout = std::uint64_t(bid) * settings.BuyoutPct / 100;
    constexpr std::uint64_t kMax = 4000000000ull;
    return std::uint32_t(std::min(std::max<std::uint64_t>(buyout, bid), kMax));   // never below the bid
}

bool Listable(Settings const& settings, Candidate const& item)
{
    if (item.ItemGuid == 0 || item.Bound || item.QuestItem || item.Needed || item.Stackable || item.Count != 1)
        return false;
    return StartingBid(settings, item.Quality, item.VendorSellCopper, 1) != 0;
}

void PruneExpired(std::vector<std::uint64_t>& endsMs, std::uint64_t nowMs)
{
    endsMs.erase(std::remove_if(endsMs.begin(), endsMs.end(), [nowMs](std::uint64_t end) { return end <= nowMs; }), endsMs.end());
}

ListFailure ClassifyListFailure(std::string const& reason)
{
    // The core's own words (ModuleActions.cpp, kList): the auctioneer is out of reach, the owner is offline, or the money is short.
    static char const* const kRetry[] = { "auctioneer not found", "not enough money", "no online player" };
    for (char const* text : kRetry)
        if (reason.find(text) != std::string::npos)
            return ListFailure::Retry;
    return ListFailure::Skip;
}

bool VisitDue(Settings const& settings, Bags const& bags, Standing const& standing, bool relistingDue, std::uint64_t nowMs)
{
    if (!settings.Enabled || bags.TotalSlots == 0)
        return false;
    if (standing.LastVisitMs != 0 && nowMs - standing.LastVisitMs < ah_batch::VisitEveryMs(settings.BatchIntervalHours, settings.EveryMs))
        return false;
    if (standing.ListedNow >= settings.ListedMax && !relistingDue)
        return false;
    if (relistingDue)
        return true;
    return std::uint64_t(bags.UsedSlots) * 100 >= std::uint64_t(bags.TotalSlots) * settings.BagFullPct;
}

std::vector<Listing> PlanListings(Settings const& settings, Bags const&, Standing const& standing, std::vector<Candidate> const& items)
{
    std::vector<Listing> plan;
    if (!settings.Enabled || standing.ListedNow >= settings.ListedMax)
        return plan;

    struct Ranked
    {
        Candidate const* Item;
        std::uint32_t Attempt;
        std::uint32_t Bid;
    };
    std::vector<Ranked> ranked;
    for (Candidate const& item : items)
    {
        if (!Listable(settings, item))
            continue;
        auto const seen = standing.Attempts.find(item.ItemGuid);
        std::uint32_t const attempt = seen == standing.Attempts.end() ? 1 : seen->second + 1;
        if (attempt > 2)
            continue; // listed twice already: the vendor gets it
        std::uint32_t const bid = StartingBid(settings, item.Quality, item.VendorSellCopper, attempt);
        if (bid != 0)
            ranked.push_back({ &item, attempt, bid });
    }
    // The dearest first: the fee is a share of the price, so a few valuable items are worth more than many cheap ones.
    std::sort(ranked.begin(), ranked.end(), [](Ranked const& a, Ranked const& b)
    {
        return a.Bid != b.Bid ? a.Bid > b.Bid : a.Item->ItemGuid < b.Item->ItemGuid;
    });
    std::uint32_t const room = settings.ListedMax - standing.ListedNow;
    std::uint32_t const take = std::min(room, settings.PerVisit);
    for (Ranked const& entry : ranked)
    {
        if (plan.size() >= take)
            break;
        plan.push_back({ entry.Item->ItemGuid, entry.Item->Entry, entry.Bid, BuyoutOf(settings, entry.Bid), settings.RunMinutes, entry.Attempt });
    }
    return plan;
}

std::uint64_t RoundUnitPrice(Settings const& settings, std::uint64_t unit, std::uint64_t vendorUnit, std::uint64_t salt)
{
    std::uint64_t const step = settings.RoundStepCopper;
    if (step == 0 || unit == 0)
        return unit;
    std::uint64_t const pct = std::min<std::uint32_t>(settings.RoundMaxPct, 50);
    std::uint64_t const cap = unit + unit * pct / 100;
    std::uint64_t lowest = unit - unit * pct / 100;
    lowest = std::max<std::uint64_t>({ lowest, vendorUnit, 1 });
    if (lowest > cap)
        return unit;
    // Three kinds of round number, chosen by the salt: a whole step, half a step, a hundredth of a step under a whole step.
    std::uint64_t mix = salt * 0x9E3779B97F4A7C15ull;
    mix ^= mix >> 29;
    std::uint64_t const first = mix % 3;
    for (std::uint64_t k = 0; k < 3; ++k)
    {
        std::uint64_t const kind = (first + k) % 3;
        std::uint64_t grid = kind == 1 ? step / 2 : step;
        std::uint64_t const under = kind == 2 ? std::max<std::uint64_t>(1, step / 100) : 0;
        if (grid == 0)
            continue;
        // The candidates around the price on the grid; the one closest to the price that lies in the window wins.
        std::uint64_t const base = (unit + under) / grid;
        std::uint64_t best = 0;
        for (std::uint64_t m = base > 0 ? base - 1 : 0; m <= base + 1; ++m)
        {
            if (m * grid <= under)
                continue;
            std::uint64_t const value = m * grid - under;
            if (value < lowest || value > cap)
                continue;
            auto const gap = [unit](std::uint64_t v) { return v > unit ? v - unit : unit - v; };
            if (best == 0 || gap(value) < gap(best))
                best = value;
        }
        if (best != 0)
            return best;
    }
    return unit;
}

std::vector<GoodsListing> PlanGoods(Settings const& settings, Standing const& standing, std::vector<Candidate> const& items)
{
    std::vector<GoodsListing> plan;
    if (!settings.Enabled || !settings.GoodsSell || standing.ListedNow >= settings.ListedMax)
        return plan;

    struct Ranked
    {
        GoodsListing Listing;
        std::uint64_t Value; // copper the whole listing is worth at the unit price
    };
    std::vector<Ranked> ranked;
    for (Candidate const& item : items)
    {
        if (!item.Goods || !item.Stackable || item.ItemGuid == 0 || item.Bound || item.QuestItem || item.Needed || item.Count < 2)
            continue;
        if (item.Quality < settings.GoodsMinQuality || item.VendorSellCopper == 0)
            continue;
        if (item.VendorSellCopper < settings.MinVendorCopper)
            continue;
        auto const listed = standing.GoodsListed.find(item.Entry);
        if (listed != standing.GoodsListed.end() && listed->second >= settings.GoodsPerEntryMax)
            continue;
        std::uint64_t const vendorUnit = item.VendorSellCopper / item.Count;
        std::uint64_t unitPrice = vendorUnit * settings.GoodsPct / 100;
        if (unitPrice == 0)
            unitPrice = 1;
        unitPrice = RoundUnitPrice(settings, unitPrice, vendorUnit, item.ItemGuid);
        std::uint32_t const count = ah_batch::StackSize(settings.BatchIntervalHours, item.Entry, item.Count, settings.GoodsStackMax);
        if (count == 0 || unitPrice > 0xFFFFFFFFu)
            continue;
        ranked.push_back({ { item.ItemGuid, item.Entry, count, std::uint32_t(unitPrice) }, unitPrice * count });
    }
    // The dearest first, as PlanListings does.
    std::sort(ranked.begin(), ranked.end(), [](Ranked const& a, Ranked const& b)
    {
        return a.Value != b.Value ? a.Value > b.Value : a.Listing.ItemGuid < b.Listing.ItemGuid;
    });
    std::uint32_t const room = settings.ListedMax - standing.ListedNow;
    std::uint32_t const take = std::min(room, settings.PerVisit);
    for (Ranked const& entry : ranked)
    {
        if (plan.size() >= take)
            break;
        plan.push_back(entry.Listing);
    }
    return plan;
}

std::vector<std::uint64_t> ForVendor(Settings const& settings, Standing const& standing, std::vector<Candidate> const& items)
{
    std::vector<std::uint64_t> sell;
    if (!settings.Enabled)
        return sell;
    for (Candidate const& item : items)
    {
        auto const seen = standing.Attempts.find(item.ItemGuid);
        if (seen != standing.Attempts.end() && seen->second >= 2 && !item.Bound && !item.QuestItem && !item.Needed)
            sell.push_back(item.ItemGuid);
    }
    return sell;
}

} // namespace auction
