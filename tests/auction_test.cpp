// Unit tests for the auction decisions (src/auction_policy.h). No core or SDK needed.
#include "auction_policy.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace auction;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

Settings On()
{
    Settings settings;
    settings.Enabled = true;
    return settings;
}

Candidate Gear(std::uint64_t guid, std::uint8_t quality, std::uint64_t vendor)
{
    Candidate item;
    item.ItemGuid = guid;
    item.Entry = std::uint32_t(1000 + guid);
    item.Quality = quality;
    item.VendorSellCopper = vendor;
    return item;
}

void PricesAreMultiplesOfTheVendorPrice()
{
    Settings s = On();
    CHECK(StartingBid(s, kUncommon, 1000, 1) == 3000);
    CHECK(StartingBid(s, kRare, 1000, 1) == 4000);
    CHECK(StartingBid(s, kEpic, 1000, 1) == 5000);
    CHECK(StartingBid(s, kLegendary, 1000, 1) == 5000);
    CHECK(StartingBid(s, kCommon, 1000, 1) == 0 && StartingBid(s, kPoor, 1000, 1) == 0);   // white and grey are the vendor's
    CHECK(StartingBid(s, kUncommon, 99, 1) == 0);                                          // below MinVendorCopper
    CHECK(StartingBid(s, kUncommon, 100, 1) == 300);
    CHECK(BuyoutOf(s, 3000) == 3600);
    s.BuyoutPct = 50;                                                                      // a buyout is never below the bid
    CHECK(BuyoutOf(s, 3000) == 3000);
}

void TheSecondListingIsCheaperAndNeverBelowTheVendor()
{
    Settings s = On();
    CHECK(StartingBid(s, kUncommon, 1000, 2) == 2700);                                     // 90 percent of 3000
    s.SellPctGreen = 100;                                                                  // a multiple that would undercut the vendor
    CHECK(StartingBid(s, kUncommon, 1000, 1) == 1001);                                     // floor: one copper above the vendor
    CHECK(StartingBid(s, kUncommon, 1000, 2) == 1001);
    s.SellPctGreen = 300;
    CHECK(StartingBid(s, kEpic, 4000000000ull, 1) == 4000000000u);                         // capped at what the core accepts
}

void OnlyTheRightItemsAreListable()
{
    Settings s = On();
    Candidate ok = Gear(1, kUncommon, 500);
    CHECK(Listable(s, ok));
    Candidate bound = ok; bound.Bound = true;
    Candidate quest = ok; quest.QuestItem = true;
    Candidate needed = ok; needed.Needed = true;
    Candidate stack = ok; stack.Stackable = true; stack.Count = 20;
    Candidate two = ok; two.Count = 2;
    Candidate none = ok; none.ItemGuid = 0;
    CHECK(!Listable(s, bound) && !Listable(s, quest) && !Listable(s, needed) && !Listable(s, stack) && !Listable(s, two) && !Listable(s, none));
    CHECK(!Listable(s, Gear(2, kCommon, 5000)));
    CHECK(!Listable(s, Gear(3, kUncommon, 50)));
}

void AVisitNeedsFullBagsAndTime()
{
    Settings s = On();
    Bags full{ 20, 12 };                 // 60 percent
    Bags roomy{ 20, 11 };
    Standing fresh;
    CHECK(VisitDue(s, full, fresh, false, 1000));
    CHECK(!VisitDue(s, roomy, fresh, false, 1000));
    CHECK(VisitDue(s, roomy, fresh, true, 1000));          // a relisting is due: the bags do not matter
    Standing visited;
    visited.LastVisitMs = 1000;
    CHECK(!VisitDue(s, full, visited, false, 1000 + 1799000));
    CHECK(VisitDue(s, full, visited, false, 1000 + 1800000));
    Standing busy;
    busy.ListedNow = 10;
    CHECK(!VisitDue(s, full, busy, false, 1000));          // the listing cap is reached
    CHECK(VisitDue(s, full, busy, true, 1000));            // but a relisting frees a slot
    CHECK(!VisitDue(Settings{}, full, fresh, false, 1000)); // off by default
    CHECK(!VisitDue(s, Bags{ 0, 0 }, fresh, true, 1000));
}

void ThePlanTakesTheDearestFirstWithinTheLimits()
{
    Settings s = On();
    s.PerVisit = 3;
    std::vector<Candidate> items = { Gear(1, kUncommon, 200), Gear(2, kRare, 900), Gear(3, kUncommon, 400), Gear(4, kEpic, 800), Gear(5, kCommon, 9000) };
    items.push_back(Gear(6, kUncommon, 150));
    Standing standing;
    std::vector<Listing> plan = PlanListings(s, Bags{ 20, 15 }, standing, items);
    CHECK(plan.size() == 3);
    if (plan.size() == 3)
    {
        CHECK(plan[0].ItemGuid == 4 && plan[0].Bid == 4000 && plan[0].Buyout == 4800 && plan[0].RunMinutes == 120);   // epic 800 -> 4000
        CHECK(plan[1].ItemGuid == 2 && plan[1].Bid == 3600);                                                       // rare 900 -> 3600
        CHECK(plan[2].ItemGuid == 3 && plan[2].Bid == 1200);                                                       // green 400 -> 1200
        CHECK(plan[0].Attempt == 1);
    }
    standing.ListedNow = 9;                                // only one more listing fits
    CHECK(PlanListings(s, Bags{ 20, 15 }, standing, items).size() == 1);
    standing.ListedNow = 10;
    CHECK(PlanListings(s, Bags{ 20, 15 }, standing, items).empty());
    CHECK(PlanListings(Settings{}, Bags{ 20, 15 }, Standing{}, items).empty());
}

void ARelistingIsCheaperAndTheThirdTimeGoesToTheVendor()
{
    Settings s = On();
    std::vector<Candidate> items = { Gear(1, kUncommon, 1000), Gear(2, kUncommon, 1000) };
    Standing standing;
    standing.Attempts[1] = 1;                              // listed once, ran out
    standing.Attempts[2] = 2;                              // listed twice
    std::vector<Listing> plan = PlanListings(s, Bags{ 20, 15 }, standing, items);
    CHECK(plan.size() == 1 && plan[0].ItemGuid == 1 && plan[0].Attempt == 2 && plan[0].Bid == 2700);
    std::vector<std::uint64_t> vendor = ForVendor(s, standing, items);
    CHECK(vendor.size() == 1 && vendor[0] == 2);
    items[1].Needed = true;                                // never sell what the bot uses
    CHECK(ForVendor(s, standing, items).empty());
    CHECK(ForVendor(Settings{}, standing, items).empty());
}

void TheBuyBudgetKeepsTheReserve()
{
    Settings s = On();
    CHECK(BuyBudget(s, 10000, 20) == (10000 - 100) * 40 / 100);   // reserve 20 x 5 = 100
    CHECK(BuyBudget(s, 100, 20) == 0);
    CHECK(BuyBudget(s, 50, 20) == 0);
    CHECK(BuyBudget(s, 1000000, 60) == (1000000 - 300) * 40 / 100);
}

void TheBestUpgradePerCopperIsChosen()
{
    Settings s = On();
    std::vector<Offer> offers;
    auto offer = [](std::uint64_t id, std::uint32_t entry, std::uint64_t buyout) { Offer o; o.AuctionGuid = id; o.Entry = entry; o.Buyout = buyout; return o; };
    offers.push_back(offer(1, 10, 5000));   // gain 10: 0.002 per copper
    offers.push_back(offer(2, 11, 1000));   // gain 4: 0.004 per copper
    offers.push_back(offer(3, 12, 800));    // gain 0: no upgrade
    offers.push_back(offer(4, 13, 20000));  // too dear
    offers.push_back(offer(5, 14, 0));      // no buyout
    Offer own = offer(6, 15, 100); own.OwnListing = true;
    offers.push_back(own);
    auto score = [](Offer const& o) -> double
    {
        switch (o.Entry) { case 10: return 10; case 11: return 4; case 12: return 0; case 13: return 100; case 14: return 50; case 15: return 50; default: return -1; }
    };
    std::optional<Offer> pick = ChooseBuy(s, 6000, offers, score);
    CHECK(pick && pick->AuctionGuid == 2);
    CHECK(!ChooseBuy(s, 500, offers, score));              // nothing affordable
    CHECK(!ChooseBuy(s, 0, offers, score));
    CHECK(!ChooseBuy(Settings{}, 6000, offers, score));    // off
    CHECK(!ChooseBuy(s, 6000, offers, UpgradeScore{}));    // no way to score
    // The same value per copper: the cheaper one.
    std::vector<Offer> tied = { offer(7, 20, 2000), offer(8, 21, 1000) };
    auto equal = [](Offer const& o) -> double { return o.Entry == 20 ? 8.0 : 4.0; };
    std::optional<Offer> cheap = ChooseBuy(s, 6000, tied, equal);
    CHECK(cheap && cheap->AuctionGuid == 8);
}

void TheCallBudgetLimitsATick()
{
    CallBudget budget(2);
    CHECK(budget.Take() && budget.Take() && !budget.Take());
    budget.NewTick();
    CHECK(budget.Take());
    budget.SetLimit(0);
    budget.NewTick();
    CHECK(!budget.Take());
}

void TheListingsThatRanOutAreDropped()
{
    std::vector<std::uint64_t> ends = { 100, 500, 300 };
    PruneExpired(ends, 300);
    CHECK(ends.size() == 1 && ends[0] == 500);
    PruneExpired(ends, 1000);
    CHECK(ends.empty());
}

void ARefusalForThePlaceOrTheMoneyIsTriedAgain()
{
    CHECK(ClassifyListFailure("auctioneer not found or cannot interact with it") == ListFailure::Retry);
    CHECK(ClassifyListFailure("auctioneer not found on any map") == ListFailure::Retry);
    CHECK(ClassifyListFailure("not enough money for the deposit") == ListFailure::Retry);
    CHECK(ClassifyListFailure("no online player owns the listed item") == ListFailure::Retry);
    // Anything about the item is not tried again: not in the bags, the wrong entry, or a refusal the core gives with no reason (bound, conjured).
    CHECK(ClassifyListFailure("item is no longer in the player's bags") == ListFailure::Skip);
    CHECK(ClassifyListFailure("item entry does not match the request") == ListFailure::Skip);
    CHECK(ClassifyListFailure("") == ListFailure::Skip);
}

void TheJournalLinesAreKeyValueAndKeepTheOldText()
{
    Listing listing;
    listing.ItemGuid = 900;
    listing.Entry = 12345;
    listing.Bid = 3000;
    listing.Buyout = 3600;
    CHECK(ListedLine(7, listing, 1) == "auction: bot 7 listed item 900 for 3000 copper (buyout 3600) entry=12345 count=1 price=3000");
    CHECK(ListedLine(7, listing, 1).rfind("auction: bot 7 listed item 900 for 3000 copper (buyout 3600)", 0) == 0);
    CHECK(ListRefusedLine(7, listing, "not enough money for the deposit") ==
          "auction: bot 7 list_refused item 900 entry=12345 price=3000 reason=not_enough_money_for_the_deposit class=retry");
    CHECK(ListRefusedLine(7, listing, "") == "auction: bot 7 list_refused item 900 entry=12345 price=3000 reason=unknown class=skip");
    CHECK(LogToken("a, b=c") == "a_bc");
    CHECK(LogToken(std::string(100, 'x')).size() == 40);
}

void TheBuyVisitFollowsTheSwitchesAndThePace()
{
    Settings s = On();
    Standing standing;
    CHECK(!BuyVisitDue(s, standing, 1000));                // AuctionBuy is off by default, whatever Auction says
    s.BuyEnabled = true;
    CHECK(BuyVisitDue(s, standing, 1000));                 // never browsed
    standing.LastBuyMs = 1000;
    CHECK(!BuyVisitDue(s, standing, 1000 + s.EveryMs - 1));
    CHECK(BuyVisitDue(s, standing, 1000 + s.EveryMs));
    Settings off;                                          // Auction off: no buying either
    off.BuyEnabled = true;
    CHECK(!BuyVisitDue(off, Standing{}, 1000));
    // the listing visit and the buy visit keep their own time
    standing.LastVisitMs = 1000;
    standing.LastBuyMs = 0;
    CHECK(BuyVisitDue(s, standing, 1001));
}

void TheBrowseSlotsTurnAndWrap()
{
    std::size_t const count = BrowseSlotCount();
    CHECK(count > 10);
    std::vector<std::uint32_t> seen;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        std::uint32_t const slot = BrowseSlot(i);
        CHECK(slot >= 1 && slot <= 26);
        CHECK(slot != 4 && slot != 12 && slot != 18 && slot != 19 && slot != 24 && slot != 25);   // shirt, trinket, bag, tabard, ammunition, thrown
        for (std::uint32_t before : seen)
            CHECK(before != slot);                         // each slot once per round
        seen.push_back(slot);
    }
    CHECK(BrowseSlot(static_cast<std::uint32_t>(count)) == BrowseSlot(0));
    CHECK(BrowseSlot(static_cast<std::uint32_t>(count) + 3) == BrowseSlot(3));
    CHECK(BuyoutBound(5000) == 5000u);
    CHECK(BuyoutBound(0x1FFFFFFFFull) == 0xFFFFFFFFu);
}

void TheCoresUpgradeScoreDecidesWhatIsBought()
{
    Settings s = On();
    s.BuyEnabled = true;
    auto offer = [](std::uint64_t id, std::uint64_t buyout, std::uint32_t upgrade)
    {
        Offer o;
        o.AuctionGuid = id;
        o.Entry = 100 + static_cast<std::uint32_t>(id);
        o.Buyout = buyout;
        o.Upgrade = upgrade;
        return o;
    };
    std::vector<Offer> offers = { offer(1, 4000, 40), offer(2, 1000, 20), offer(3, 500, 0), offer(4, 9000, 500) };
    std::optional<Offer> pick = ChooseBuy(s, 5000, offers, UpgradeOf);
    CHECK(pick && pick->AuctionGuid == 2);                 // 0.02 per copper beats 0.01; the score 0 is no upgrade; the dear one is over the budget
    CHECK(!ChooseBuy(s, 400, offers, UpgradeOf));
    CHECK(UpgradeOf(offer(5, 1, 7)) == 7.0 && UpgradeOf(Offer{}) == 0.0);
    // the budget: a quarter of the money above the reserve
    s.BuyMaxPct = 25;
    CHECK(BuyBudget(s, 100000, 10) == (100000 - 10 * s.ReserveCopperPerLevel) * 25 / 100);
    CHECK(BuyBudget(s, 40, 10) == 0);                      // under the reserve: nothing to spend
}

void TheBuyJournalLinesAreKeyValue()
{
    Offer o;
    o.AuctionGuid = 901;
    o.Entry = 2000;
    o.Count = 1;
    o.Buyout = 1500;
    CHECK(BoughtLine(7, o, 1500) == "auction: bot 7 bought item 901 entry=2000 count=1 price=1500");
    CHECK(BuyFailedLine(7, o, "not enough money") == "auction: bot 7 buy_failed item 901 entry=2000 price=1500 reason=not_enough_money");
    CHECK(BuyFailedLine(7, o, "") == "auction: bot 7 buy_failed item 901 entry=2000 price=1500 reason=unknown");
    CHECK(BuyFailedLine(7, o, "auction not found, sorry") == "auction: bot 7 buy_failed item 901 entry=2000 price=1500 reason=auction_not_found_sorry");
}

} // namespace

int main()
{
    TheListingsThatRanOutAreDropped();
    ARefusalForThePlaceOrTheMoneyIsTriedAgain();
    PricesAreMultiplesOfTheVendorPrice();
    TheSecondListingIsCheaperAndNeverBelowTheVendor();
    OnlyTheRightItemsAreListable();
    AVisitNeedsFullBagsAndTime();
    ThePlanTakesTheDearestFirstWithinTheLimits();
    ARelistingIsCheaperAndTheThirdTimeGoesToTheVendor();
    TheBuyBudgetKeepsTheReserve();
    TheBestUpgradePerCopperIsChosen();
    TheCallBudgetLimitsATick();
    TheJournalLinesAreKeyValueAndKeepTheOldText();
    TheBuyVisitFollowsTheSwitchesAndThePace();
    TheBrowseSlotsTurnAndWrap();
    TheCoresUpgradeScoreDecidesWhatIsBought();
    TheBuyJournalLinesAreKeyValue();
    if (failures == 0)
        std::printf("auction_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
