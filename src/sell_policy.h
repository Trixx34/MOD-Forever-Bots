// Bots and the vendors (doc/economy.md): which items a bot sells to a vendor on a visit, and how often it visits. Pure logic with no SDK dependency,
// unit-tested in tests/sell_test.cpp. Nothing here talks to the core: the module feeds it the items it has seen the bot loot and carries out the sales
// it returns with kSellToVendor.
//
// Rules of the first version (the owner's decisions): a bot sells only white (common) items it does not need. Grey items are sold by the service visit
// that already runs (kVendor, bit 1), and green or better items go to the Auction House (auction_policy.h). Weapons and armour are never sold here: the
// module does not know what a bot keeps for an upgrade.
#pragma once

#include <cstdint>
#include <vector>

namespace sell {

struct Settings
{
    bool Enabled = false;                       // config key SellVendor (off by default)
    std::uint64_t EveryMs = 1800000;            // a bot visits a vendor to sell at most this often (config key SellEverySec)
    std::uint32_t MaxSalesPerVisit = 8;         // distinct items sold on one visit (config key SellPerVisit)
};

// One item the bot may sell, as the module saw it looted.
struct Item
{
    std::uint32_t Entry = 0;
    std::uint32_t Count = 1;
    std::uint8_t Quality = 1;                   // item quality as the game numbers it: 0 grey (poor), 1 white (common)
    std::uint64_t VendorSellCopper = 0;         // what a vendor pays for one of the item
    bool QuestItem = false;
    bool Equipment = false;                     // a weapon or a piece of armour
};

// One kSellToVendor action: the item entry and how many to sell (0 = every one the bot carries of it).
struct Sale
{
    std::uint32_t Entry = 0;
    std::uint32_t Count = 0;
    std::uint64_t Copper = 0;                   // what the vendor pays for the sale
};

// Whether an item may be sold on a vendor visit at all: white, not a quest item, not equipment, and worth something to a vendor.
bool Sellable(Item const& item);

// Whether a bot should visit a vendor to sell now: the feature is on, the bot has something to sell, and its last visit was long enough ago
// (lastVisitMs 0 means it has never visited).
bool VisitDue(Settings const& settings, bool hasSellable, std::uint64_t lastVisitMs, std::uint64_t nowMs);

// The sales for one visit: one per distinct entry, most money first, at most MaxSalesPerVisit. Items that cannot be sold are left out.
std::vector<Sale> PlanSales(Settings const& settings, std::vector<Item> const& items);

// A visit that has not finished this long after it started is dropped: the bot could not reach the vendor, and the items stay for a later visit.
constexpr std::uint64_t kVisitTimeoutMs = 600000;
bool VisitExpired(std::uint64_t visitStartMs, std::uint64_t nowMs);

// A sale that has been sent and not yet answered by the core. The core's reply is the bot's last action: it is the answer to this sale once the
// sequence has moved on and the last action was a sale of the same entry.
struct InFlight
{
    Sale Sent;
    std::uint32_t Sequence = 0;                 // the bot's last_action_sequence when the sale was sent
    std::uint64_t SentMs = 0;
};

enum class Outcome
{
    kWaiting,                                   // no reply yet
    kSold,                                      // the core applied the sale: the items are gone
    kRefused,                                   // the core refused it, or no reply came in time: the items stay
};

// kReplyTimeoutMs: a sale with no reply after this long counts as refused, so a dropped tick does not hold the bot.
constexpr std::uint64_t kReplyTimeoutMs = 20000;

// What the core's reply means for an in-flight sale. lastSequence and lastWasSale describe the bot's last action in the current snapshot;
// applied is whether the core applied that action.
Outcome Resolve(InFlight const& flight, std::uint32_t lastSequence, bool lastWasSale, bool applied, std::uint64_t nowMs);

} // namespace sell
