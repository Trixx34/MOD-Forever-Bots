// SPDX-License-Identifier: GPL-2.0-or-later
#include "auction_policy.h"
#include "ah_batch.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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
    // The core's own words (ModuleActions.cpp, kList): the auctioneer is out of reach (or not one with a house: a place fault, not the item's), the owner is
    // offline, or the money is short.
    static char const* const kRetry[] = { "auctioneer not found", "auctioneer has no auction house", "not enough money", "no online player" };
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

bool ListingVisitDue(Settings const& settings, Bags const& bags, Standing const& standing, bool inReach, bool hasListable, std::uint64_t nowMs)
{
    if (!settings.Enabled || !inReach)
        return false;
    if (!hasListable)
        return VisitDue(settings, bags, standing, false, nowMs);
    if (standing.LastVisitMs != 0 && nowMs - standing.LastVisitMs < ah_batch::VisitEveryMs(settings.BatchIntervalHours, settings.EveryMs))
        return false;
    return standing.ListedNow < settings.ListedMax;
}

void StampLootLevel(std::vector<Candidate*> const& items, std::uint32_t botLevel)
{
    if (botLevel == 0)
        return;
    for (Candidate* item : items)
        if (item && item->LootLevel == 0)
            item->LootLevel = botLevel;
}

bool GearReady(Settings const& settings, Candidate const& item, std::uint32_t botLevel)
{
    if (!item.Equipment)
        return true;
    if (!settings.GearSell)
        return false;
    if (item.Returned)
        return true;
    if (item.LootLevel == 0 || botLevel == 0)
        return false;
    if (settings.GearExact && item.HasFacts)
    {
        // An upgrade it wears now is Needed (Listable refuses it). A piece it will wear within the hold's levels waits until it is one level past it.
        bool const later = item.Usable && item.RequiredLevel > item.LootLevel && item.RequiredLevel <= item.LootLevel + settings.GearHoldLevels;
        return !later || botLevel > item.RequiredLevel;
    }
    return botLevel >= item.LootLevel + settings.GearHoldLevels;
}

std::vector<Candidate> ReadyItems(Settings const& settings, std::vector<Candidate> const& items, std::uint32_t botLevel)
{
    std::vector<Candidate> ready;
    ready.reserve(items.size());
    for (Candidate const& item : items)
        if (GearReady(settings, item, botLevel))
            ready.push_back(item);
    return ready;
}

bool HasListable(Settings const& settings, std::vector<Candidate> const& items, std::uint32_t botLevel)
{
    if (!settings.Enabled)
        return false;
    std::vector<Candidate> const ready = ReadyItems(settings, items, botLevel);
    for (Candidate const& item : ready)
        if (Listable(settings, item))
            return true;
    return !PlanGoods(settings, Standing{}, ready).empty();
}

std::optional<NearUnit> FindLive(std::uint64_t spotRef, float ax, float ay, std::vector<NearUnit> const& nearby)
{
    if (spotRef == 0)
        return std::nullopt;
    std::optional<NearUnit> best;
    float bestSq = kResolveYards * kResolveYards;
    for (NearUnit const& unit : nearby)
    {
        if ((unit.Ref >> 32) != (spotRef >> 32))
            continue;
        float const dx = unit.X - ax;
        float const dy = unit.Y - ay;
        float const sq = dx * dx + dy * dy;
        if (sq <= bestSq)
        {
            bestSq = sq;
            best = unit;
        }
    }
    return best;
}

std::uint64_t ResolveNearby(std::uint64_t spotRef, float ax, float ay, std::vector<NearUnit> const& nearby)
{
    std::optional<NearUnit> const live = FindLive(spotRef, ax, ay, nearby);
    return live ? live->Ref : 0;
}

CallBlock JudgeCall(CallFacts const& facts)
{
    if (!facts.AuctioneerValid)
        return CallBlock::kNoAuctioneer;
    if (facts.InFlight)
        return CallBlock::kInFlight;
    float const dx = facts.BotX - facts.AuctioneerX;
    float const dy = facts.BotY - facts.AuctioneerY;
    float const dz = facts.BotZ - facts.AuctioneerZ;
    float const sq2d = dx * dx + dy * dy;
    if (sq2d > kReachYards * kReachYards)
        return CallBlock::kFar2d;
    if (sq2d + dz * dz > kCoreReach3dYards * kCoreReach3dYards)
        return CallBlock::kFarHeight;
    if (facts.Moving)
        return CallBlock::kMoving;
    return CallBlock::kOk;
}

char const* CallBlockName(CallBlock block)
{
    switch (block)
    {
        case CallBlock::kOk: return "ok";
        case CallBlock::kNoAuctioneer: return "no_auctioneer";
        case CallBlock::kInFlight: return "in_flight";
        case CallBlock::kFar2d: return "far_2d";
        case CallBlock::kFarHeight: return "far_height";
        case CallBlock::kMoving: return "moving";
    }
    return "?";
}

char const* RefusalCode(std::string const& reason)
{
    // The core's words (ModuleActions.cpp, AuctionHouseAction), most specific first.
    static struct
    {
        char const* Text;
        char const* Code;
    } const kCodes[] = {
        { "auctioneer not found on the player's map", "auctioneer_missing" },
        { "auctioneer not found or cannot interact", "cannot_interact" },
        { "auctioneer has no auction house", "no_house" },
        { "not enough money", "no_money" },
        { "no online player", "no_player" },
        { "has no acting player", "no_player" },
        { "no longer in the player's bags", "item_gone" },
        { "item entry does not match", "item_mismatch" },
        { "auction was not listed", "not_listed" },
        { "cannot bid on your own auction", "own_auction" },
        { "auction not found", "auction_gone" },
    };
    for (auto const& entry : kCodes)
        if (reason.find(entry.Text) != std::string::npos)
            return entry.Code;
    return "other";
}

std::string RefusalCause(std::string const& reason, CallFacts const& facts)
{
    std::string const code = RefusalCode(reason);
    if (code != "cannot_interact" && code != "auctioneer_missing")
        return "-";
    if (CallBlock const block = JudgeCall(facts); block != CallBlock::kOk)
        return CallBlockName(block);
    if (facts.LiveLooked && !facts.LiveSeen)
        return "not_seen";
    return "in_reach_other";
}

std::string RefusalFields(std::string const& reason, CallFacts const& facts)
{
    return std::string(" code=") + RefusalCode(reason) + " cause=" + RefusalCause(reason, facts);
}

RefPoint StandPoint(RefPoint const& anchor, float fromX, float fromY, std::uint32_t attempt)
{
    float const dx = fromX - anchor.X;
    float const dy = fromY - anchor.Y;
    float const base = dx * dx + dy * dy > 0.01f ? std::atan2(dy, dx) : 0.0f;
    // 0, +45, -45, +90, -90, +135, -135, 180 degrees from the side the bot comes from.
    std::uint32_t const k = attempt % kStandTries;
    float const step = float((k + 1) / 2) * (3.14159265f / 4.0f);
    float const angle = base + (k % 2 == 1 ? step : -step);
    return { anchor.X + kStandRingYards * std::cos(angle), anchor.Y + kStandRingYards * std::sin(angle), anchor.Z };
}

StandNext JudgeStandTry(std::uint32_t attempt, bool noRoute, std::uint64_t near2dSinceMs, std::uint64_t nowMs)
{
    bool const stuck = near2dSinceMs != 0 && nowMs >= near2dSinceMs && nowMs - near2dSinceMs >= kStandTryMs;
    if (!noRoute && !stuck)
        return StandNext::kStay;
    return attempt + 1 < kStandTries ? StandNext::kNextPoint : StandNext::kGiveUp;
}

bool InReach(bool auctioneerValid, float botX, float botY, float auctioneerX, float auctioneerY)
{
    if (!auctioneerValid)
        return false;
    float const dx = botX - auctioneerX;
    float const dy = botY - auctioneerY;
    return dx * dx + dy * dy <= kReachYards * kReachYards;
}

bool InReach3D(bool auctioneerValid, float botX, float botY, float botZ, float auctioneerX, float auctioneerY, float auctioneerZ)
{
    if (!auctioneerValid)
        return false;
    float const dx = botX - auctioneerX;
    float const dy = botY - auctioneerY;
    float const dz = botZ - auctioneerZ;
    return dx * dx + dy * dy + dz * dz <= kReach3dYards * kReach3dYards;
}

std::string ReachFields(float botX, float botY, float botZ, float auctioneerX, float auctioneerY, float auctioneerZ)
{
    float const dx = botX - auctioneerX;
    float const dy = botY - auctioneerY;
    float const dz = botZ - auctioneerZ;
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), " dist2d=%.1f dist3d=%.1f bot_z=%.1f auctioneer_z=%.1f", std::sqrt(dx * dx + dy * dy), std::sqrt(dx * dx + dy * dy + dz * dz),
                  botZ, auctioneerZ);
    return buffer;
}

StandJudgement JudgeStandZ(float auctioneerX, float auctioneerY, float auctioneerZ, std::optional<float> groundZ, std::vector<RefPoint> const& refs)
{
    StandJudgement judgement;
    float bestSq = kStandRefNearYards * kStandRefNearYards;
    for (RefPoint const& ref : refs)
    {
        float const dx = ref.X - auctioneerX;
        float const dy = ref.Y - auctioneerY;
        float const sq = dx * dx + dy * dy;
        if (sq <= bestSq)
        {
            bestSq = sq;
            judgement.RefKnown = true;
            judgement.RefZ = ref.Z;
        }
    }
    if (judgement.RefKnown && std::fabs(auctioneerZ - judgement.RefZ) > kStandRefMaxDz)
        judgement.Verdict = StandZ::kSkip;
    else if (groundZ && std::fabs(auctioneerZ - *groundZ) >= kStandGroundMaxDz)
        judgement.Verdict = StandZ::kIgnoreZ;
    return judgement;
}

char const* StandZName(StandZ verdict)
{
    switch (verdict)
    {
        case StandZ::kUse3D: return "use_3d";
        case StandZ::kIgnoreZ: return "ignore_z";
        case StandZ::kSkip: return "skip_bad_z";
    }
    return "?";
}

bool InReachJudged(StandZ verdict, bool auctioneerValid, float botX, float botY, float botZ, float auctioneerX, float auctioneerY, float auctioneerZ)
{
    switch (verdict)
    {
        case StandZ::kUse3D: return InReach3D(auctioneerValid, botX, botY, botZ, auctioneerX, auctioneerY, auctioneerZ);
        case StandZ::kIgnoreZ: return InReach(auctioneerValid, botX, botY, auctioneerX, auctioneerY);
        case StandZ::kSkip: return false;
    }
    return false;
}

char const* ReachCheckName(StandZ verdict, bool inReach3d)
{
    if (verdict == StandZ::kSkip)
        return "skip_bad_z";
    if (verdict == StandZ::kIgnoreZ)
        return "ignore_z";
    return inReach3d ? "ok_3d" : "failed_3d";
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
