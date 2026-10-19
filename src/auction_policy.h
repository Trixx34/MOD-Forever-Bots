// Bots and the Auction House (doc/economy.md): what a bot lists, at what price, when it goes to the house, and which listing it buys. Pure logic with no SDK
// dependency, unit-tested in tests/auction_test.cpp. Nothing here talks to the core: the module feeds it facts (the bag contents the core describes, the
// listings a browse returned) and carries out the plan it returns.
//
// Rules of the first version (the owner's decisions): bots only sell, players may buy their listings, a bot lists tradeable items of green quality or
// better that it will not use, and prices are multiples of the vendor price. The core lists one non-stackable item at a time (the kList of
// ModuleActions.cpp), so in practice these are pieces of equipment.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace auction {

// Item quality, as the game numbers it.
enum Quality : std::uint8_t { kPoor = 0, kCommon = 1, kUncommon = 2, kRare = 3, kEpic = 4, kLegendary = 5 };

struct Settings
{
    bool Enabled = false;                       // config key Auction (off by default)
    // Listing price as a percent of the vendor sell price, by quality (config keys AuctionSellPctGreen, AuctionSellPctBlue, AuctionSellPctEpic). The vendor price
    // is the floor the house cannot fall under: nobody pays less for it than a vendor does.
    std::uint32_t SellPctGreen = 300;
    std::uint32_t SellPctBlue = 400;
    std::uint32_t SellPctEpic = 500;
    std::uint32_t BuyoutPct = 120;              // the buyout is the starting bid times this percent
    std::uint32_t RelistPct = 90;               // a listing that ran out is listed again at this percent of the last price, once; then it goes to a vendor
    std::uint32_t RunMinutes = 120;             // 2 hours: the core's kList always sends this run (the wire has no run time), and the fork accepts 120, 480 and 1440
    std::uint64_t MinVendorCopper = 100;        // an item a vendor would pay less than this is not worth a listing fee
    std::uint32_t BagFullPct = 60;              // a bot lists when this share of its bag slots is used ...
    std::uint32_t PerVisit = 5;                 // ... up to this many items per visit ...
    std::uint32_t ListedMax = 10;               // ... and never more than this many listings at once
    std::uint64_t EveryMs = 1800000;            // a bot goes to the house at most this often (config key AuctionEverySec)
    std::uint32_t BuyMaxPct = 40;               // a purchase costs at most this share of the money the bot can spend (above the reserve)
    std::uint32_t ReserveCopperPerLevel = 5;    // as the core's Bot.Supplies.ReserveCopperPerLevel: what a bot keeps back
    std::uint32_t BrowsePerVisit = 3;           // item entries a bot looks up on one visit
    std::uint32_t MaxCallsPerTick = 4;          // house calls the whole module makes in one tick (a browse walks the house's lists in the core)
};

// One item in a bot's bags as the core describes it for selling.
struct Candidate
{
    std::uint64_t ItemGuid = 0;
    std::uint32_t Entry = 0;
    std::uint32_t Count = 1;
    std::uint8_t Quality = kCommon;
    std::uint64_t VendorSellCopper = 0;         // what a vendor pays for the whole item
    bool Bound = false;                         // soulbound or account bound: cannot be listed
    bool QuestItem = false;
    bool Needed = false;                        // the bot uses it: equipped, a better piece than what it wears, a reagent or supply it needs
    bool Stackable = false;                     // the core's kList takes single items only
};

struct Listing
{
    std::uint64_t ItemGuid = 0;
    std::uint32_t Entry = 0;
    std::uint32_t Bid = 0;                      // copper
    std::uint32_t Buyout = 0;
    std::uint32_t RunMinutes = 0;
    std::uint32_t Attempt = 1;                  // 1 first listing, 2 the relisting
};

// The vendor price rounded into a starting bid by quality and attempt; 0 when the item is not worth listing (poor or common quality, bound, or a vendor
// would pay less than MinVendorCopper).
std::uint32_t StartingBid(Settings const& settings, std::uint8_t quality, std::uint64_t vendorSellCopper, std::uint32_t attempt);
std::uint32_t BuyoutOf(Settings const& settings, std::uint32_t bid);

// Whether an item may be listed at all: green or better, tradeable, not a quest item, not needed, a single piece, worth the fee.
bool Listable(Settings const& settings, Candidate const& item);

// What a bot's bags hold.
struct Bags
{
    std::uint32_t TotalSlots = 0;
    std::uint32_t UsedSlots = 0;
};

// What a bot has on the house now, and how often each of its items has been listed (the item guid is kept by the core when a listing runs out and the
// item comes back by mail).
struct Standing
{
    std::uint32_t ListedNow = 0;
    std::unordered_map<std::uint64_t, std::uint32_t> Attempts;   // item guid -> times listed
    std::uint64_t LastVisitMs = 0;                               // 0: never
};

// Whether this bot may go to the house now: it is switched on, enough bag slots are used (or it has a relisting due), and the last visit is long enough ago.
bool VisitDue(Settings const& settings, Bags const& bags, Standing const& standing, bool relistingDue, std::uint64_t nowMs);

// The listings to make this visit, most valuable first: at most PerVisit, and ListedMax minus what is listed already. An item listed twice before is left
// for a vendor (the caller sells it).
std::vector<Listing> PlanListings(Settings const& settings, Bags const& bags, Standing const& standing, std::vector<Candidate> const& items);

// Items that ran their course on the house and go to a vendor now: listed `attempts` times already (the second listing is the last).
std::vector<std::uint64_t> ForVendor(Settings const& settings, Standing const& standing, std::vector<Candidate> const& items);

// Drops the listings whose run has ended (their end time is at or before nowMs); what is left is still on the house.
void PruneExpired(std::vector<std::uint64_t>& endsMs, std::uint64_t nowMs);

// What a refused listing means. A refusal for the bot's place or its money is tried again on a later visit; any other refusal is about the item
// (not in the bags, bound, conjured, not a single piece), so the item is not listed again.
enum class ListFailure : std::uint8_t { Retry, Skip };
ListFailure ClassifyListFailure(std::string const& reason);

// ---- buying ----

// A listing a browse returned.
struct Offer
{
    std::uint64_t AuctionGuid = 0;
    std::uint32_t Entry = 0;
    std::uint32_t Count = 1;
    std::uint64_t Bid = 0;
    std::uint64_t Buyout = 0;                   // 0: no buyout, so it cannot be bought outright
    bool OwnListing = false;                    // the bot's own
};

// How much better the item is for the bot than what it wears in that place, as one number (> 0 is an upgrade). The caller supplies it from the same score
// the core's AutoGear uses; this module does not know item stats.
using UpgradeScore = std::function<double(Offer const&)>;

// The money a bot may spend on the house: its money less the reserve for its level, and at most BuyMaxPct of that for one purchase.
std::uint64_t BuyBudget(Settings const& settings, std::uint64_t copper, std::uint32_t level);

// The offer to buy outright, or none: the best upgrade per copper among the offers it can afford with the budget. A tie goes to the lower buyout.
std::optional<Offer> ChooseBuy(Settings const& settings, std::uint64_t budget, std::vector<Offer> const& offers, UpgradeScore const& score);

// ---- pace ----

// A budget of house calls for the whole module in one tick: bots ask for a call, and some are told to wait.
class CallBudget
{
public:
    explicit CallBudget(std::uint32_t perTick) : _perTick(perTick) {}
    void NewTick() { _used = 0; }
    bool Take() { return _used < _perTick && ++_used; }
    void SetLimit(std::uint32_t perTick) { _perTick = perTick; }

private:
    std::uint32_t _perTick;
    std::uint32_t _used = 0;
};

} // namespace auction
