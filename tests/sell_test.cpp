// Unit tests for the vendor sale decisions (src/sell_policy.h). No core or SDK needed.
#include "sell_policy.h"

#include <cstdio>

using namespace sell;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

constexpr std::uint64_t kMin = 60000;

Settings On()
{
    Settings settings;
    settings.Enabled = true;
    return settings;
}

Item White(std::uint32_t entry, std::uint32_t count, std::uint64_t vendor)
{
    Item item;
    item.Entry = entry;
    item.Count = count;
    item.Quality = 1;
    item.VendorSellCopper = vendor;
    return item;
}

void WhiteItemsAreSellable()
{
    CHECK(Sellable(White(1, 1, 50)));
}

void OtherItemsAreNot()
{
    Item grey = White(1, 1, 50);
    grey.Quality = 0;                           // grey: the service visit sells it
    CHECK(!Sellable(grey));

    Item green = White(1, 1, 50);
    green.Quality = 2;                          // green and better: the Auction House
    CHECK(!Sellable(green));

    Item quest = White(1, 1, 50);
    quest.QuestItem = true;
    CHECK(!Sellable(quest));

    Item gear = White(1, 1, 50);
    gear.Equipment = true;
    CHECK(!Sellable(gear));

    CHECK(!Sellable(White(1, 1, 0)));           // a vendor will not pay for it
    CHECK(!Sellable(White(1, 0, 50)));          // none of it
}

void NothingHappensWhenOff()
{
    Settings settings;                          // off by default
    CHECK(!VisitDue(settings, true, 0, 10 * kMin));
    CHECK(PlanSales(settings, { White(1, 1, 50) }).size() == 1);   // the plan itself does not gate on Enabled; VisitDue does
}

void VisitWaitsForTheInterval()
{
    Settings settings = On();
    settings.EveryMs = 30 * kMin;
    CHECK(VisitDue(settings, true, 0, 1 * kMin));                 // never visited
    CHECK(!VisitDue(settings, true, 10 * kMin, 20 * kMin));       // too soon
    CHECK(VisitDue(settings, true, 10 * kMin, 40 * kMin));
}

void NothingToSellNoVisit()
{
    CHECK(!VisitDue(On(), false, 0, 10 * kMin));
}

void OneSalePerEntryWithTheTotal()
{
    std::vector<Item> items = { White(7, 2, 50), White(7, 3, 50), White(8, 1, 10) };
    std::vector<Sale> sales = PlanSales(On(), items);
    CHECK(sales.size() == 2);
    CHECK(sales.size() == 2 && sales[0].Entry == 7 && sales[0].Copper == 250);
    CHECK(sales.size() == 2 && sales[0].Count == 0);             // 0 = every one the bot carries
    CHECK(sales.size() == 2 && sales[1].Entry == 8 && sales[1].Copper == 10);
}

void MostMoneyFirst()
{
    std::vector<Item> items = { White(1, 1, 5), White(2, 1, 90), White(3, 1, 40) };
    std::vector<Sale> sales = PlanSales(On(), items);
    CHECK(sales.size() == 3);
    CHECK(sales.size() == 3 && sales[0].Entry == 2);
    CHECK(sales.size() == 3 && sales[2].Entry == 1);
}

void CappedPerVisit()
{
    Settings settings = On();
    settings.MaxSalesPerVisit = 2;
    std::vector<Item> items = { White(1, 1, 5), White(2, 1, 90), White(3, 1, 40) };
    std::vector<Sale> sales = PlanSales(settings, items);
    CHECK(sales.size() == 2);
    CHECK(sales.size() == 2 && sales[0].Entry == 2 && sales[1].Entry == 3);
}

void UnsellableItemsLeftOut()
{
    Item grey = White(9, 1, 500);
    grey.Quality = 0;
    CHECK(PlanSales(On(), { grey, White(1, 1, 0) }).empty());
}

InFlight Sent(std::uint32_t sequence, std::uint64_t sentMs)
{
    InFlight flight;
    flight.Sent.Entry = 1;
    flight.Sequence = sequence;
    flight.SentMs = sentMs;
    return flight;
}

void ReplyDecidesTheSale()
{
    InFlight flight = Sent(5, 0);
    constexpr std::uint64_t kSoon = 5000;       // inside the reply timeout
    // No new reply yet: still waiting.
    CHECK(Resolve(flight, 5, false, false, kSoon) == Outcome::kWaiting);
    // A new reply that is a sale and applied: sold.
    CHECK(Resolve(flight, 6, true, true, kSoon) == Outcome::kSold);
    // A new reply that is a sale and refused: the items stay.
    CHECK(Resolve(flight, 6, true, false, kSoon) == Outcome::kRefused);
    // A new reply for some other action (a policy move left over from before): keep waiting for ours.
    CHECK(Resolve(flight, 6, false, true, kSoon) == Outcome::kWaiting);
}

void NoReplyInTimeIsRefused()
{
    InFlight flight = Sent(5, 0);
    CHECK(Resolve(flight, 5, false, false, kReplyTimeoutMs - 1) == Outcome::kWaiting);
    CHECK(Resolve(flight, 5, false, false, kReplyTimeoutMs) == Outcome::kRefused);
}

void VisitEndsAfterTheTimeout()
{
    CHECK(!VisitExpired(10 * kMin, 19 * kMin));
    CHECK(VisitExpired(10 * kMin, 20 * kMin));
}

} // namespace

int main()
{
    WhiteItemsAreSellable();
    OtherItemsAreNot();
    NothingHappensWhenOff();
    VisitWaitsForTheInterval();
    NothingToSellNoVisit();
    OneSalePerEntryWithTheTotal();
    MostMoneyFirst();
    CappedPerVisit();
    UnsellableItemsLeftOut();
    ReplyDecidesTheSale();
    NoReplyInTimeIsRefused();
    VisitEndsAfterTheTimeout();
    if (failures == 0)
        std::printf("sell_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
