// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for batched auction listing (src/ah_batch.h, wired into auction_policy.cpp): off by default and then identical to the old rules; with an
// interval the visits are spaced by it and goods are listed in stacks of 1, 5 or 20 per item.
#include "ah_batch.h"
#include "auction_policy.h"

#include <cstdio>
#include <set>

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

constexpr std::uint64_t kHour = 3600ull * 1000;

void OffKeepsTheOldBehaviour()
{
    CHECK(auction::Settings{}.BatchIntervalHours == 0);
    CHECK(ah_batch::StackSize(0, 123, 37, 20) == 20);
    CHECK(ah_batch::StackSize(0, 123, 7, 20) == 7);
    CHECK(ah_batch::StackSize(0, 123, 0, 20) == 0);
    CHECK(ah_batch::VisitEveryMs(0, 1800000) == 1800000);
}

void StackSizesAreOneFiveOrTwentyAndFitWhatTheBotHas()
{
    std::set<std::uint32_t> seen;
    for (std::uint32_t entry = 1000; entry < 1100; ++entry)
    {
        std::uint32_t const size = ah_batch::StackSize(6, entry, 200, 20);
        CHECK(size == 1 || size == 5 || size == 20);
        CHECK(size == ah_batch::StackSize(6, entry, 200, 20));   // the same for the same item on every visit
        seen.insert(size);
        CHECK(ah_batch::StackSize(6, entry, 3, 20) <= 3);
        CHECK(ah_batch::StackSize(6, entry, 7, 20) <= 7);
        CHECK(ah_batch::StackSize(6, entry, 200, 5) <= 5);       // the old cap still holds
    }
    CHECK(seen.size() == 3);
}

auction::Bags FullBags()
{
    auction::Bags bags;
    bags.TotalSlots = 20;
    bags.UsedSlots = 18;
    return bags;
}

auction::Settings On(std::uint32_t hours)
{
    auction::Settings settings;
    settings.Enabled = true;
    settings.BatchIntervalHours = hours;
    return settings;
}

void VisitsAreSpacedByTheInterval()
{
    auction::Standing standing;
    standing.LastVisitMs = 1000;
    // Off: the old 30 minutes.
    CHECK(!auction::VisitDue(On(0), FullBags(), standing, true, 1000 + 29 * 60000));
    CHECK(auction::VisitDue(On(0), FullBags(), standing, true, 1000 + 31 * 60000));
    // On: relisting waits for the interval as well.
    CHECK(!auction::VisitDue(On(6), FullBags(), standing, true, 1000 + 31 * 60000));
    CHECK(!auction::VisitDue(On(6), FullBags(), standing, true, 1000 + 6 * kHour - 1));
    CHECK(auction::VisitDue(On(6), FullBags(), standing, true, 1000 + 6 * kHour));
    CHECK(auction::VisitDue(On(6), FullBags(), auction::Standing{}, false, 5));   // the first visit is never delayed
}

void GoodsAreListedInBatchSizes()
{
    auction::Settings settings = On(6);
    settings.GoodsSell = true;
    settings.PerVisit = 20;
    settings.ListedMax = 50;
    std::vector<auction::Candidate> items;
    for (std::uint32_t i = 0; i < 12; ++i)
    {
        auction::Candidate item;
        item.ItemGuid = 100 + i;
        item.Entry = 2000 + i;
        item.Count = 60;
        item.Goods = true;
        item.Stackable = true;
        item.VendorSellCopper = 6000;
        items.push_back(item);
    }
    std::set<std::uint32_t> sizes;
    for (auction::GoodsListing const& listing : auction::PlanGoods(settings, auction::Standing{}, items))
    {
        CHECK(listing.Count == 1 || listing.Count == 5 || listing.Count == 20);
        sizes.insert(listing.Count);
    }
    CHECK(sizes.size() >= 2);
    settings.BatchIntervalHours = 0;
    for (auction::GoodsListing const& listing : auction::PlanGoods(settings, auction::Standing{}, items))
        CHECK(listing.Count == settings.GoodsStackMax);   // the old rule: the largest stack
}

}  // namespace

int main()
{
    OffKeepsTheOldBehaviour();
    StackSizesAreOneFiveOrTwentyAndFitWhatTheBotHas();
    VisitsAreSpacedByTheInterval();
    GoodsAreListedInBatchSizes();
    if (failures == 0)
        std::printf("ah_batch_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
