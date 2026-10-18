// Unit tests for the auction decisions (src/auction_policy.h). No core or SDK needed.
#include "auction_policy.h"

#include <cstdio>

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
        CHECK(plan[0].ItemGuid == 4 && plan[0].Bid == 4000 && plan[0].Buyout == 4800 && plan[0].RunMinutes == 1440);   // epic 800 -> 4000
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

} // namespace

int main()
{
    PricesAreMultiplesOfTheVendorPrice();
    TheSecondListingIsCheaperAndNeverBelowTheVendor();
    OnlyTheRightItemsAreListable();
    AVisitNeedsFullBagsAndTime();
    ThePlanTakesTheDearestFirstWithinTheLimits();
    ARelistingIsCheaperAndTheThirdTimeGoesToTheVendor();
    TheBuyBudgetKeepsTheReserve();
    TheBestUpgradePerCopperIsChosen();
    TheCallBudgetLimitsATick();
    if (failures == 0)
        std::printf("auction_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
