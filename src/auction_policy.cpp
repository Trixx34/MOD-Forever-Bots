#include "auction_policy.h"

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
    if (standing.LastVisitMs != 0 && nowMs - standing.LastVisitMs < settings.EveryMs)
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

std::uint64_t BuyBudget(Settings const& settings, std::uint64_t copper, std::uint32_t level)
{
    std::uint64_t const reserve = std::uint64_t(level) * settings.ReserveCopperPerLevel;
    std::uint64_t const spendable = copper > reserve ? copper - reserve : 0;
    return spendable * settings.BuyMaxPct / 100;
}

std::optional<Offer> ChooseBuy(Settings const& settings, std::uint64_t budget, std::vector<Offer> const& offers, UpgradeScore const& score)
{
    if (!settings.Enabled || budget == 0 || !score)
        return std::nullopt;
    std::optional<Offer> best;
    double bestValue = 0.0;
    for (Offer const& offer : offers)
    {
        if (offer.OwnListing || offer.Buyout == 0 || offer.Buyout > budget)
            continue;
        double const gain = score(offer);
        if (gain <= 0.0)
            continue;
        double const value = gain / double(offer.Buyout);
        if (!best || value > bestValue || (value == bestValue && offer.Buyout < best->Buyout))
        {
            best = offer;
            bestValue = value;
        }
    }
    return best;
}

double UpgradeOf(Offer const& offer)
{
    return double(offer.Upgrade);
}

bool BuyVisitDue(Settings const& settings, Standing const& standing, std::uint64_t nowMs)
{
    if (!settings.Enabled || !settings.BuyEnabled)
        return false;
    return standing.LastBuyMs == 0 || nowMs - standing.LastBuyMs >= settings.EveryMs;
}

namespace {
// InventoryType: 1 head, 2 neck, 3 shoulders, 5 chest, 6 waist, 7 legs, 8 feet, 9 wrists, 10 hands, 11 finger, 13 one-hand weapon, 14 shield, 15 bow,
// 16 cloak, 17 two-hand weapon, 20 robe, 21 main-hand weapon, 22 off-hand weapon, 26 gun, crossbow or wand.
constexpr std::uint32_t kBrowseSlots[] = { 1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 13, 14, 15, 16, 17, 20, 21, 22, 26 };
}

std::size_t BrowseSlotCount()
{
    return sizeof(kBrowseSlots) / sizeof(kBrowseSlots[0]);
}

std::uint32_t BrowseSlot(std::uint32_t cursor)
{
    return kBrowseSlots[cursor % BrowseSlotCount()];
}

std::uint32_t BuyoutBound(std::uint64_t budget)
{
    return budget > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(budget);
}

std::string LogToken(std::string const& text)
{
    std::string out;
    for (char const c : text)
    {
        if (out.size() >= 40)
            break;
        if (c == ' ')
            out += '_';
        else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == ':' || c == '-')
            out += c;
    }
    return out.empty() ? "unknown" : out;
}

std::string ListedLine(std::uint64_t bot, Listing const& listing, std::uint32_t count)
{
    return "auction: bot " + std::to_string(bot) + " listed item " + std::to_string(listing.ItemGuid) + " for " + std::to_string(listing.Bid) + " copper (buyout " +
           std::to_string(listing.Buyout) + ") entry=" + std::to_string(listing.Entry) + " count=" + std::to_string(count) + " price=" + std::to_string(listing.Bid);
}

std::string ListRefusedLine(std::uint64_t bot, Listing const& listing, std::string const& reason)
{
    return "auction: bot " + std::to_string(bot) + " list_refused item " + std::to_string(listing.ItemGuid) + " entry=" + std::to_string(listing.Entry) +
           " price=" + std::to_string(listing.Bid) + " reason=" + LogToken(reason) + " class=" +
           (ClassifyListFailure(reason) == ListFailure::Skip ? "skip" : "retry");
}

std::string BoughtLine(std::uint64_t bot, Offer const& offer, std::uint64_t paid)
{
    return "auction: bot " + std::to_string(bot) + " bought item " + std::to_string(offer.AuctionGuid) + " entry=" + std::to_string(offer.Entry) +
           " count=" + std::to_string(offer.Count) + " price=" + std::to_string(paid);
}

std::string BuyFailedLine(std::uint64_t bot, Offer const& offer, std::string const& reason)
{
    return "auction: bot " + std::to_string(bot) + " buy_failed item " + std::to_string(offer.AuctionGuid) + " entry=" + std::to_string(offer.Entry) +
           " price=" + std::to_string(offer.Buyout) + " reason=" + LogToken(reason);
}

} // namespace auction
