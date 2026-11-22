// SPDX-License-Identifier: GPL-2.0-or-later
// Bots and the Auction House (doc/economy.md): what a bot lists, at what price, when it goes to the house, and which listing it buys. Pure logic with no SDK
// dependency, unit-tested in tests/auction_test.cpp. Nothing here talks to the core: the module feeds it facts (the bag contents the core describes, the
// listings a browse returned) and carries out the plan it returns.
//
// Rules of the first version (the owner's decisions): bots only sell, players may buy their listings, a bot lists tradeable items of green quality or
// better that it will not use, and prices are multiples of the vendor price. The core lists one non-stackable item at a time (the kList of
// ModuleActions.cpp), so in practice these are pieces of equipment.
#pragma once

#include <cstddef>
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
    // Config key AuctionResolveNearby (off by default): the snapshot's auctioneer reference is built from the spawn id, but the core numbers a creature
    // it spawns from its own per-map counter, so the core never finds that reference (every house call answers "auctioneer not found on the player's map").
    // With this on, a bot in reach names the live auctioneer from its nearby units instead (ResolveNearby).
    bool ResolveNearby = false;
    // Config key AuctionWalk3D (off by default; the sim first): the reach to the auctioneer is measured in 3D (kReach3dYards) instead of 5 yards in 2D, for the
    // listing visit (main_feeds.cpp) and the auction errand, which walks the bot to the auctioneer until it is in that reach (a bot under the Ironforge house
    // at z=19 with the auctioneers at z=505 was "in reach" in 2D and every call was refused).
    bool Walk3D = false;
    // Config key AuctionLogReach (on by default, logging only): a refused house call logs the bot's 2D and 3D distance to the auctioneer and both heights.
    bool LogReach = true;
    bool BuyEnabled = false;                    // config key AuctionBuy (off by default): a bot also buys upgrades; only with Enabled
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
    // Goods (config keys AuctionGoodsSell, AuctionGoodsPct, AuctionGoodsStackMax, AuctionGoodsPerEntryMax, AuctionGoodsMinQuality; off by default): stacks of
    // trade goods (herbs, ore, cloth, leather, reagents) listed as commodities, one unit price for the stack (the core's kListCommodity).
    bool GoodsSell = false;                     // needs Enabled
    std::uint32_t GoodsPct = 150;               // the unit price is this percent of the vendor's price for one unit
    std::uint32_t GoodsStackMax = 20;           // a listing sells at most this many units
    std::uint32_t GoodsPerEntryMax = 1;         // listings of one item kind on the house at once
    std::uint8_t GoodsMinQuality = kCommon;     // white or better
    // Round-number prices (config keys AuctionRoundStep, AuctionRoundMaxPct; off by default, backlog item 1): a goods unit price is moved to a human number,
    // a whole step (a gold), half a step, or a hundredth of a step under a whole step, when one lies within RoundMaxPct percent of the computed price. Never
    // below the vendor's unit price, never above the computed price plus RoundMaxPct, so a listing's total stays under the cap it had.
    std::uint32_t RoundStepCopper = 0;          // 0: off; 10000 is a gold
    std::uint32_t RoundMaxPct = 5;
    // Batched listing (config AhBatchIntervalHours, 0 = off, ah_batch.h): a bot visits the house at most once per this many hours (relisting and cancelling included),
    // and lists goods in stacks of 1, 5 or 20 chosen per item. With 0 the rules above are unchanged.
    std::uint32_t BatchIntervalHours = 0;
    // Equipment (config keys AuctionGearSell, AuctionGearHoldLevels): a looted green-or-better weapon or piece of armour is listed once the bot has gained
    // GearHoldLevels levels since it looted it. Until then it is kept: the core's bag equip pass (kVendor bit 4 on the service visits) wears it if it is an
    // upgrade, and an equipped BoE item is soulbound, which the core's HandleAuctionSellItem refuses (CanBeTraded). The hold covers the items the bot
    // could not wear yet when it looted them (a required level a little above its own). The loot event does not say the item's required level or
    // upgrade score (doc/economy-and-professions.md, the core change), so the levels gained stand in for it.
    bool GearSell = true;
    std::uint32_t GearHoldLevels = 2;
    // The exact rule (config key AuctionGearExact, off by default): when the loot event carries the core's item facts (Candidate::HasFacts), the hold
    // above gives way to them. An upgrade the bot would wear now (upgrade_score > 0) is Needed and never listed; a piece the bot may use (Usable) whose
    // required level is above the bot's and at most GearHoldLevels above its loot level is kept until the bot is one level past that required level
    // (the bag equip pass wears it first); any other piece is ready at once. Without the facts (an older core) the hold applies as before.
    bool GearExact = false;
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
    bool Goods = false;                         // trade goods: a stack for a commodity listing when Stackable (see PlanGoods)
    bool Equipment = false;                     // a weapon or a piece of armour: listed only when GearReady
    std::uint32_t LootLevel = 0;                // the bot's level when it looted the item (0: not known yet; StampLootLevel sets it)
    // The core's item facts (PlayerItemLootedEvent fields 9 to 14), for Settings::GearExact. HasFacts is false when the core did not send them.
    bool HasFacts = false;
    std::uint32_t RequiredLevel = 0;            // 0: none
    bool Usable = false;                        // the bot's class, race and skills allow it, the required level aside
    bool Returned = false;                      // a listing of it ran out and the core gave it back from the mail: it already waited out its hold
};

// The bot's level as the loot level of the items that have none yet (the loot event does not carry the level; the first feed after the loot does).
void StampLootLevel(std::vector<Candidate*> const& items, std::uint32_t botLevel);

// Whether a piece of equipment may be listed now: gear selling is on, the loot level is known, and the bot has gained GearHoldLevels since (or, with
// GearExact and the core's facts, the exact rule of Settings::GearExact).
// Anything that is not equipment is ready (its own rules apply: Listable, PlanGoods).
bool GearReady(Settings const& settings, Candidate const& item, std::uint32_t botLevel);

// The items of `items` that may be listed now (GearReady), for PlanListings and PlanGoods.
std::vector<Candidate> ReadyItems(Settings const& settings, std::vector<Candidate> const& items, std::uint32_t botLevel);

// Whether the bot has anything to list now: a ready item that is Listable, or a goods stack PlanGoods would list. A town errand to the auctioneer is
// only worth it then.
bool HasListable(Settings const& settings, std::vector<Candidate> const& items, std::uint32_t botLevel);

// Whether the bot stands in reach of its auctioneer: the snapshot gives the auctioneer of the bot's map, so only the 2D distance counts, at most
// kReachYards. The core's handler (Player::GetNPCIfCanInteractWith) allows the auctioneer's combat reach plus 4 yards, about 5.5.
constexpr float kReachYards = 5.0f;
bool InReach(bool auctioneerValid, float botX, float botY, float auctioneerX, float auctioneerY);
// AuctionWalk3D: the same in 3D, at most kReach3dYards (the core measures in 3D; a bot below or above the house floor is not in reach, whatever the 2D distance).
constexpr float kReach3dYards = 4.5f;
bool InReach3D(bool auctioneerValid, float botX, float botY, float botZ, float auctioneerX, float auctioneerY, float auctioneerZ);
// AuctionLogReach: " dist2d=D dist3d=E bot_z=Z auctioneer_z=A" (one decimal), for the refusal lines.
std::string ReachFields(float botX, float botY, float botZ, float auctioneerX, float auctioneerY, float auctioneerZ);

// AuctionWalk3D, the z-aware stand point (arm C, 2026-11-19): 194 auction_reach lines, all check=failed_3d, failed on height, not distance: 2D 3.7 yards
// but 3D 14.2 (bot_z -43.1, auctioneer_z -56.8: another floor of the house), and 2D 1.9 with bot_z 7.4 against auctioneer_z 505.2. Before the auctioneer's
// stored height is trusted as the walk target and the 3D reach, it is judged:
//   - kSkip: the stored z lies more than kStandRefMaxDz from the ground of the nearest reference near the auctioneer (a vendor, food or goods vendor or
//     banker of the snapshot, a graveyard seen; within kStandRefNearYards in 2D): bad data, the auctioneer is not used (no errand, no listing);
//   - kIgnoreZ: a ground height at the auctioneer's (x, y) is known and the stored z lies kStandGroundMaxDz or more from it: the z is not trusted and the
//     2D reach counts, as without AuctionWalk3D;
//   - kUse3D: otherwise: the walk goes to the auctioneer's own (x, y, z) and the bot is in reach within kReach3dYards in 3D.
// The module has no ground height query of its own (the core's kProbePoint has no sender in the module yet, and a probe action would take the place of the
// walk's result in the snapshot): the callers pass no ground height, so kIgnoreZ is not reached today and the z is checked only against the references.
// No reference near the auctioneer: nothing to compare with, kUse3D.
constexpr float kStandGroundMaxDz = 40.0f;
constexpr float kStandRefMaxDz = 60.0f;
constexpr float kStandRefNearYards = 200.0f;
enum class StandZ : std::uint8_t { kUse3D, kIgnoreZ, kSkip };
struct RefPoint
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};
struct StandJudgement
{
    StandZ Verdict = StandZ::kUse3D;
    bool RefKnown = false;   // a reference lies within kStandRefNearYards of the auctioneer; RefZ is the nearest one's z
    float RefZ = 0.0f;
};
StandJudgement JudgeStandZ(float auctioneerX, float auctioneerY, float auctioneerZ, std::optional<float> groundZ, std::vector<RefPoint> const& refs);
char const* StandZName(StandZ verdict);   // "use_3d", "ignore_z", "skip_bad_z"
// The reach with the judgement: kUse3D in 3D (InReach3D), kIgnoreZ in 2D (InReach), kSkip never.
bool InReachJudged(StandZ verdict, bool auctioneerValid, float botX, float botY, float botZ, float auctioneerX, float auctioneerY, float auctioneerZ);
// The auction_reach line's check= value for a bot in 2D reach: "skip_bad_z", "ignore_z" (the 2D reach counts), "ok_3d" or "failed_3d".
char const* ReachCheckName(StandZ verdict, bool inReach3d);
// The references of a bot snapshot (a template, so this file needs no SDK header): its nearest vendor, food vendor, goods vendor and banker, when valid.
template <typename Snapshot>
std::vector<RefPoint> SnapshotRefs(Snapshot const& bot)
{
    std::vector<RefPoint> refs;
    if (bot.vendor_valid != 0)
        refs.push_back({ bot.vendor_x, bot.vendor_y, bot.vendor_z });
    if (bot.food_vendor_valid != 0)
        refs.push_back({ bot.food_vendor_x, bot.food_vendor_y, bot.food_vendor_z });
    if (bot.goods_vendor_valid != 0)
        refs.push_back({ bot.goods_vendor_x, bot.goods_vendor_y, bot.goods_vendor_z });
    if (bot.banker_valid != 0)
        refs.push_back({ bot.banker_x, bot.banker_y, bot.banker_z });
    return refs;
}

// A unit near a bot, as ResolveNearby needs it (no SDK dependency).
struct NearUnit
{
    std::uint64_t Ref = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;   // the live unit's height (the auctioneer's real z, FindLive)
};

// The reference of the live auctioneer for a bot that stands in reach of the one the snapshot names. `spotRef` is the snapshot's reference (unit type and
// creature entry in the top 32 bits, the spawn id below); the live creature has the same type and entry, and stands at the spawn position (ax, ay). Returns
// the nearest such unit within kResolveYards of the spawn position, or 0 when none is in the bot's nearby list (the caller then makes no house call).
constexpr float kResolveYards = 8.0f;
std::uint64_t ResolveNearby(std::uint64_t spotRef, float ax, float ay, std::vector<NearUnit> const& nearby);
// The same match, with the live unit itself: its reference and its own position (x, y, z), the place the core measures the reach from. None when the bot
// does not see it (not loaded, another phase, or crowded out of the nearby list).
std::optional<NearUnit> FindLive(std::uint64_t spotRef, float ax, float ay, std::vector<NearUnit> const& nearby);

// ---- the house call gate and the refusal causes (doc/auction-reach-fix.md, 2026-11-20) ----
//
// The core takes a house call (list, browse, buyout, cancel) only when Player::GetNPCIfCanInteractWith passes: the bot is not on a flight, alive, the
// auctioneer alive, not hostile (reaction above unfriendly), not in combat, in the bot's phase, and within the auctioneer's combat reach + 4 yards in 3D, the
// two combat reaches added (WorldObject::IsWithinDistInMap: 1.5 + 4 + 1.5 + 1.5 for a player and a humanoid auctioneer). The module's own reach was 5 yards
// in 2D, so a bot on another floor of the house (2D 3.7, 3D 14.2) or under the city (bot z 7, Ironforge's auctioneers at z 505), on a flight over the house,
// or walking past it was "in reach" and every call it made was refused with "auctioneer not found or cannot interact with it".
constexpr float kCoreReach3dYards = 8.5f;
// What a call from where the bot stands would meet (the facts of the bot's snapshot when the call is planned).
struct CallFacts
{
    bool AuctioneerValid = false;
    float BotX = 0.0f, BotY = 0.0f, BotZ = 0.0f;
    float AuctioneerX = 0.0f, AuctioneerY = 0.0f, AuctioneerZ = 0.0f;   // the live auctioneer's position when the bot sees it, else the spawn's
    bool Moving = false;      // the snapshot's moving: a bot walking through the 5 yards is gone when the call lands
    bool InFlight = false;    // on a taxi flight (the core refuses any interaction then)
    bool LiveLooked = false;  // the bot's nearby units were searched for the live auctioneer (FindLive) ...
    bool LiveSeen = false;    // ... and it was there
};
// kOk, or the first reason a call from here is refused: no auctioneer, on a flight, farther than kReachYards in 2D, in 2D reach but beyond kCoreReach3dYards in
// 3D (another floor, under or above the house), moving. The plain bug fix (no key): no call is made unless this is kOk.
enum class CallBlock : std::uint8_t { kOk, kNoAuctioneer, kInFlight, kFar2d, kFarHeight, kMoving };
CallBlock JudgeCall(CallFacts const& facts);
char const* CallBlockName(CallBlock block);   // "ok", "no_auctioneer", "in_flight", "far_2d", "far_height", "moving"
// A house refusal's short code, from the core's words (ModuleActions.cpp): "auctioneer_missing" (not found on the bot's map), "cannot_interact" (found, but
// GetNPCIfCanInteractWith said no), "no_money", "no_player", "item_gone", "item_mismatch", "no_house", "not_listed", "auction_gone", "own_auction", or "other".
char const* RefusalCode(std::string const& reason);
// What the module knows of a cannot_interact (or auctioneer_missing) refusal: a CallBlockName when the facts at send already broke the reach, "not_seen" when
// the bot did not see the live auctioneer, else "in_reach_other" (in reach as far as the module can tell: the auctioneer's phase, combat or the bot's standing
// with its faction, which the snapshot does not carry, or the bot moved between the snapshot and the call). "-" for the other codes.
std::string RefusalCause(std::string const& reason, CallFacts const& facts);
// " code=<RefusalCode> cause=<RefusalCause>", appended to the list_refused and buy_failed lines.
std::string RefusalFields(std::string const& reason, CallFacts const& facts);

// ---- AuctionWalk3D: the stand point (2026-11-20) ----
//
// The auction errand walked to the auctioneer's own spot. That spot is usually behind the counter, off the navmesh: the core answers kNoRoute (the errand
// failed with reason=no_route once the walk was shorter than a partial leg) and moves the bot to the nearest point the mesh snaps it to, often the floor
// above or below (the arm C sim: 194 auction_reach check=failed_3d, 2D 3.7 yards, 3D 14.2). With AuctionWalk3D the walk goes to a stand point instead: on
// a ring of kStandRingYards around the auctioneer, at its height (the live creature's when the bot sees it), first on the side the bot comes from, then
// 45 degrees to one side, to the other, 90, and so on, the far side last. A point the core finds no route to, or one that leaves the bot in 2D reach
// but out of the 3D reach for kStandTryMs, gives way to the next; after kStandTries points the errand fails.
constexpr float kStandRingYards = 2.5f;
constexpr std::uint32_t kStandTries = 8;
constexpr std::uint64_t kStandTryMs = 15000;
// Stand point `attempt` (0-based, taken modulo kStandTries) around `anchor` for a bot coming from (fromX, fromY); at the anchor's height.
RefPoint StandPoint(RefPoint const& anchor, float fromX, float fromY, std::uint32_t attempt);
enum class StandNext : std::uint8_t { kStay, kNextPoint, kGiveUp };
// After stand point `attempt`: kStay while it may still work; kNextPoint when the core found no route to it (noRoute) or the bot has been in 2D reach without
// the 3D reach since near2dSinceMs (0 = not) for kStandTryMs; kGiveUp instead of kNextPoint when it was the last point.
StandNext JudgeStandTry(std::uint32_t attempt, bool noRoute, std::uint64_t near2dSinceMs, std::uint64_t nowMs);

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
    std::uint64_t LastBuyMs = 0;                                 // the last browse for an upgrade; 0: never
    std::uint32_t BuyCursor = 0;                                 // which slot the next buy visit looks at (BrowseSlot)
    std::unordered_map<std::uint32_t, std::uint32_t> GoodsListed; // item entry -> commodity listings on the house now
};

// Whether this bot may go to the house now: it is switched on, enough bag slots are used (or it has a relisting due), and the last visit is long enough ago.
bool VisitDue(Settings const& settings, Bags const& bags, Standing const& standing, bool relistingDue, std::uint64_t nowMs);

// The listing visit the module makes: only for a bot in reach of its auctioneer. (A visit made far away is refused by the core, and it used to spend the
// pace, so a bot that then arrived on its errand waited EveryMs and had walked on.) A bot in reach with something to list visits as soon as the pace
// and ListedMax allow, whatever its bags hold: the errand that brought it decided the trip was worth it, and the bag rule of VisitDue assumes a
// 16-slot backpack while the snapshot's free slots cover every bag, so it never fired for a bot that carries bags. With nothing to list it is VisitDue.
bool ListingVisitDue(Settings const& settings, Bags const& bags, Standing const& standing, bool inReach, bool hasListable, std::uint64_t nowMs);

// The listings to make this visit, most valuable first: at most PerVisit, and ListedMax minus what is listed already. An item listed twice before is left
// for a vendor (the caller sells it).
std::vector<Listing> PlanListings(Settings const& settings, Bags const& bags, Standing const& standing, std::vector<Candidate> const& items);

// A commodity listing: `Count` units of one stack at one unit price (the core's kListCommodity).
struct GoodsListing
{
    std::uint64_t ItemGuid = 0;
    std::uint32_t Entry = 0;
    std::uint32_t Count = 0;
    std::uint32_t UnitPrice = 0;                // copper per unit
};

// The commodity listings to make this visit: goods stacks of at least GoodsMinQuality that the bot does not need, each at GoodsPct of the vendor's unit
// price (at least 1 copper), at most GoodsStackMax units, and at most GoodsPerEntryMax listings per item kind. The same PerVisit and ListedMax limits as
// PlanListings apply. A stack whose vendor value is under MinVendorCopper is left for a vendor. Empty when GoodsSell is off.
// The unit price moved to a round number as Settings::RoundStepCopper says (or `unit` unchanged): the same `salt` (the item's guid) gives the same price.
std::uint64_t RoundUnitPrice(Settings const& settings, std::uint64_t unit, std::uint64_t vendorUnit, std::uint64_t salt);

std::vector<GoodsListing> PlanGoods(Settings const& settings, Standing const& standing, std::vector<Candidate> const& items);

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
    std::uint32_t Upgrade = 0;                  // the core's upgrade_score for this bot: 0 = no upgrade
};

// How much better the item is for the bot than what it wears in that place, as one number (> 0 is an upgrade). The caller supplies it from the same score
// the core's AutoGear uses; this module does not know item stats.
using UpgradeScore = std::function<double(Offer const&)>;
// The score the core worked out and put on the offer (AuctionListingEvent.upgrade_score).
double UpgradeOf(Offer const& offer);

// Whether this bot looks for an upgrade now: AuctionBuy and Auction are on, and the last browse was at least EveryMs ago (the same pace as a listing visit,
// but without the bag condition: a bot with empty bags still buys). Money is judged by the caller, with BuyBudget.
bool BuyVisitDue(Settings const& settings, Standing const& standing, std::uint64_t nowMs);

// The inventory types (the game's InventoryType) a bot looks through, one per buy visit, in turn: the slots with equipment that has stats a class
// compares (no shirts, tabards, bags, ammunition, trinkets or held items). BrowseSlot(cursor) wraps around.
std::size_t BrowseSlotCount();
std::uint32_t BrowseSlot(std::uint32_t cursor);

// The buyout bound to send with a browse: the budget, as the wire's 32 bits.
std::uint32_t BuyoutBound(std::uint64_t budget);

// The money a bot may spend on the house: its money less the reserve for its level, and at most BuyMaxPct of that for one purchase.
std::uint64_t BuyBudget(Settings const& settings, std::uint64_t copper, std::uint32_t level);

// The offer to buy outright, or none: the best upgrade per copper among the offers it can afford with the budget. A tie goes to the lower buyout.
std::optional<Offer> ChooseBuy(Settings const& settings, std::uint64_t budget, std::vector<Offer> const& offers, UpgradeScore const& score);

// ---- the journal lines (after "forever-bots: "; key=value, no spaces or commas inside a value) ----

// A free-text reason as one token: spaces become '_', commas and other characters outside [A-Za-z0-9_.:-] are dropped, cut at 40 characters; empty gives "unknown".
std::string LogToken(std::string const& text);
// "auction: bot G listed item I for B copper (buyout X) entry=E count=N price=B": the text before " entry=" is the original line that dashboards read.
std::string ListedLine(std::uint64_t bot, Listing const& listing, std::uint32_t count);
// "auction: bot G list_refused item I entry=E price=B reason=TOKEN class=retry|skip"
// The commodity listing line, in the style of ListedLine.
std::string GoodsListedLine(std::uint64_t bot, GoodsListing const& listing);

std::string ListRefusedLine(std::uint64_t bot, Listing const& listing, std::string const& reason);

// "auction: bot G bought item A entry=E count=N price=P" (A is the auction id; P is the copper the core took)
std::string BoughtLine(std::uint64_t bot, Offer const& offer, std::uint64_t paid);
// "auction: bot G buy_failed item A entry=E price=P reason=TOKEN" (P is the buyout asked)
std::string BuyFailedLine(std::uint64_t bot, Offer const& offer, std::string const& reason);

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
