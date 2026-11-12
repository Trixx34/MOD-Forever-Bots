// SPDX-License-Identifier: GPL-2.0-or-later
// The browse slots, the buyout bound and the auction log lines (auction_policy.h).
#include "auction_policy.h"

#include <string>

namespace auction {

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

std::string GoodsListedLine(std::uint64_t bot, GoodsListing const& listing)
{
    return "auction: bot " + std::to_string(bot) + " listed goods item " + std::to_string(listing.ItemGuid) + " entry=" + std::to_string(listing.Entry) +
           " count=" + std::to_string(listing.Count) + " unit_price=" + std::to_string(listing.UnitPrice);
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
