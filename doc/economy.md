# Bot economy and the Auction House: design

Status: **the decision logic is built and unit-tested** (`src/auction_policy.*`, `tests/auction_test.cpp`, settings `Auction*`, off by default). **The module drives
the listings** (`main.cpp`, a visit timer every 10 s on the Run() thread, `AuctionHouseForBot` `kList`), from the bots' loot (`PlayerScript.OnLootItem`) and the pump's feed.
It does **not** walk a bot to an auctioneer (that is the policy in `bot_policy.cpp`, not done), does not sell to vendors, and does not relist expired items. Bots receive the pay
of their sales: the core takes a bot's delivered **auction house mail** for it (`Bot.Mail.Collect`, `Bots/BotMail.cpp`: only `MAIL_AUCTION` mail, letters are left alone). Nothing has run on a sim. It says what exists, what is missing, a first version small enough to test, and the decisions that are the
owner's. The brief it replaces (`economy-design-prompt.md` in the old `.opencode/docs`) is not on the new servers.

## Checks of the core (read from the code, 2026-10-19)

- **Mail** (the owner's requirement: bots get the money). A sale, a buyout, an expired or cancelled listing and an outbid bid all arrive as mail; the core delivers the sale mail after
  `MailDeliveryDelay` (3600 s by default). No code ever opened a mailbox for a bot, so the money and items would have stayed there. Built: `WorldSession::CollectMailAsBot` takes the money and
  the items of each delivered **auction house** mail (`messageType` `MAIL_AUCTION` = 2; the sender of such mail is the house id, not a character; player, GM, creature and calendar mail is not touched),
  without a mailbox (the same steps as `HandleMailTakeMoney` and `HandleMailTakeItem`), skips cash on delivery, and deletes the auction mail that is empty. `BotMgr` runs it for every
  online bot that is not an alt every `Bot.Mail.CollectEverySec` (30) seconds and at login, so mail that arrives while a bot is away (presence) is taken when it comes back. Compiles; **not run**.
- **Listing** (`kList`, `ModuleActions.cpp`): works for a bot (the item's owner is found among the online players, or `PlayerGuid`). The core's handler lists **one non-stackable item
  at a time** (`UseCount` 1, `item->GetCount() == 1`); stackable goods go through `HandleAuctionSellCommodity`, which the module action does not call. So the first version lists pieces of equipment
  only. The fork's auctions run **2, 8 or 24 hours** (`RunTime` 120, 480 or 1440). The action sends 120 for the deposit and the run, and the wire has no run time, so every
  auction runs **2 hours**: the module's `RunMinutes` is 120 and `AuctionRunHours` accepts only 2. Making 8 or 24 hours needs a run-time field in `ActionRequestMsg` (a layout change).
- **Browse** (`kBrowse`): by **item entry only**; it cannot ask for "upgrades for my slot". It answers with one `AuctionListingEvent` per listing, carrying the asking bot's guid and the request id, so
  results can be matched to a bot. It needs one online player to take a locale from (a bot is one).
- **Bid and buyout**: need an acting player; with `AuctionHouseForBot` the bot is that player.
- **Bag contents**: `kListBags` gives only guid, entry, count and name. Quality, vendor price, whether the item is bound and whether the bot would wear it are not in it.

### Still to build

1. Core: let `kList` take the run time (and optionally stackable commodities); a browse mode that returns the cheapest listings of an equipment slot and level range (so the buy side can
   work), or a core-side "best upgrade on the house for this bot" using the `AutoGear` score.
2. Core/SDK: a bag description with what `auction::Candidate` needs: quality, vendor sell price, bound, quest item, stackable, and "needed" (equipped, a better piece than the worn one, a
   supply or reagent). The same score `AutoGear` uses decides "needed" and "upgrade".
3. Module: call the above from `main.cpp` (a visit timer on the client thread like presence, the call budget `AuctionCallsPerTick`), keep the `Standing` of each bot (listings made, attempts per item guid;
   the first listing of an item that comes back by mail keeps its guid), sell to a vendor what `ForVendor` names, and write `auction_listed`, `auction_sold`, `auction_bought` events.
4. Sim test as described at the end.

## What exists

| Piece | Where | State |
|---|---|---|
| A bot's money and bag room | snapshot `copper`, `bag_free_slots` | built |
| Selling grey items, repairing, buying supplies at vendors | `kVendor`; `doc/supplies-2.md` | built, not run on the sim for long |
| Money reserve | core `Bot.Supplies.ReserveCopperPerLevel` | built (protocol 11) |
| The nearest auctioneer | snapshot `auctioneer_guid` (an NPC reference) | built |
| Auction house operations: browse, bid, buyout, list, cancel | Tier 1 `ModuleClient::AuctionHouseForBot`, core `ModuleActions.cpp` (`AuctionHouseAction`) | built in the core and SDK; the module lists (`kList`, `main.cpp`); browse, bid and buyout are not used yet; a browse answers with an `AuctionListingEvent` |
| The core's own seller bot | worldserver `AuctionHouseBot.*` (off, `Sellers.Count = 0`) | exists in the fork; fills the house with items from a fixed list, not with what bots find |
| Walking to an auctioneer, standing in reach | the vendor walk (`bot_policy.cpp`, kTravel...) | works for vendors; auctioneers are indexed separately |

## What is missing

1. A reason for a bot to sell on the house: today a bot sells greys to a vendor and keeps everything else until the bags are full. Nothing says which
   item is worth listing, at what price, for how long.
2. A reason to buy: gear upgrades are taken from the bags and from drops (`AutoGear`); a bot never looks at the house.
3. Prices. The database has vendor prices (`ItemTemplate::GetSellPrice`, `GetBuyPrice`) but no market. A price must come from a rule.
4. Sinks and a ceiling, so bots do not make money out of nothing: the deposit and the house cut are the sinks; the rest needs a limit.
5. Visibility for the owner: how much money the bots hold, how much moves through the house, what lists and never sells.

## First version (small on purpose)

Bots **sell** surplus on the house and **buy** upgrades and supplies they already know they want. No crafting, no gathering, no arbitrage.

**Selling.**
- *What:* any non-soulbound item of quality uncommon (green) or better that the bot will not use (`AutoGear` found it no better than what is worn, and it is not
  a quest item, a reagent the bot needs, or food/drink/ammunition/potion/bandage it uses) and that a vendor would pay at least `SellMinCopper` for.
  The core lists single non-stackable items only, so in practice this is equipment; stacks of trade goods stay with the vendor for now.
- *Price:* a multiple of the vendor sell price by quality: green x3, blue x4, epic x5 (keys `AuctionSellPctGreen`, `AuctionSellPctBlue`, `AuctionSellPctEpic`: 300, 400, 500 percent), rounded; buyout equals the starting bid
  times 1.2 (`AuctionBuyoutPct` 120). A multiple of the vendor price is a floor the market cannot go under: a player who undercuts it loses money to the vendor, so the house price
  stays near it.
- *Duration:* 2 hours (what the core sends; see Checks), relisted once at 90 percent of the price if it did not sell, then sold to a vendor. **Not built:** the relisting and the vendor sale.
- *When:* an idle bot with at least `AuctionBagFullPct` (default 60 percent) of its bag slots used (the snapshot gives the free slots; the module assumes 16 in all), standing near an auctioneer or due its service visit
  (`ServiceEverySec`), lists at most `AuctionPerVisit` (default 5) items per visit.
- *Limit:* at most `AuctionListedMax` (default 10) listings per bot at once; a listing counts until its run ends. The gold from sales is taken from the bot's mail by the core (`Bot.Mail.Collect`).

**Buying.**
- *Gear upgrade:* when `AutoGear` shows a slot a level behind (the bot's item in that slot is below its level band), the bot browses its auctioneer for
  its slot and class (`kBrowse`, filtered by level range and quality), and buys the cheapest item that is an upgrade by the same score `AutoGear` uses and
  costs at most `AuctionBuyMaxPct` (default 40 percent) of its money above the reserve.
- *Supplies:* food, drink, potions and bandages stay a vendor purchase. The house is not used for them (vendor stock is unlimited and cheaper).

**Pace.** A bot visits the house at most every `AuctionEverySec` (default 1800) and the module makes at most `AuctionCallsPerTick` (default 4) house calls per tick across
all bots, each one a listing (`kList`); browses are not wired yet.

## Controls that keep it from running away

- **Money in, money out per hour** are counted by the module (sum of `copper` over bots at each telemetry line, sales and purchases from the replies).
  A knob `BotMoneyTarget` (copper per level) lets the owner see the drift; nothing is adjusted automatically in the first version.
- **Deposit and cut** are the house's (the core's `Rate.Auction.*`); set them, do not work around them.
- **Fixed price base:** listings are multiples of the vendor price, so a bot-made market cannot inflate past those multiples. A run of listings that never
  sell is reported (not priced down).
- **Off by default**, like every bot feature.

## Core and SDK work needed

- None for browse / list / buyout / cancel (they exist). The bot-as-actor variant `AuctionHouseForBot` is the one to use.
- A list of a bot's sellable items with their quality, vendor price and slot, which today needs a bag listing: `BotTrade` has `kListBags` (Tier 1) for trade; the same
  call can serve here, or the snapshot gains the single "best item to sell" (a layout field), which keeps the walk to the house to one decision.
- Mail: done (see Checks).
- Telemetry: `auction_listed`, `auction_sold`, `auction_bought` events.

## Test plan

1. Unit tests of the pure parts (price rule, what to list, what to buy), like `supply_test.cpp`.
2. Sim: 100 bots, `Presence` off, `AuctionHouseBot.Sellers.Count = 0`; watch listings per hour, sales, the money total, and the core's world tick time.
   Not yet done: the bots do not walk to an auctioneer, so a visit happens only when a bot is already at one.
3. Compare: the money total after 8 hours with and without the house.

## Decisions (answered by the owner, 2026-10-19)

- **Sellers:** the bots only. The core's `AuctionHouseBot` stays off (`Sellers.Count = 0`).
- **Players may buy** what bots list. Bot gold becomes real; no rule hides bot listings.
- **What bots list:** tradeable items of green quality or better that the bot will not use.
- **Prices:** vendor price multiples (green x3, blue x4, epic x5, buyout x1.2), tunable; still guesses until the sim shows what bots earn.

Still open: whether any of this is wanted before bots level reliably (with no levelling there is nothing worth selling). The mail question is answered: bots take their mail.
