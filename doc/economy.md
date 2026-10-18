# Bot economy and the Auction House: design

Status: **a design, nothing is built.** It says what exists, what is missing, a first version small enough to test, and the decisions that are the
owner's. The brief it replaces (`economy-design-prompt.md` in the old `.opencode/docs`) is not on the new servers.

## What exists

| Piece | Where | State |
|---|---|---|
| A bot's money and bag room | snapshot `copper`, `bag_free_slots` | built |
| Selling grey items, repairing, buying supplies at vendors | `kVendor`; `doc/supplies-2.md` | built, not run on the sim for long |
| Money reserve | core `Bot.Supplies.ReserveCopperPerLevel` | built (protocol 11) |
| The nearest auctioneer | snapshot `auctioneer_guid` (an NPC reference) | built |
| Auction house operations: browse, bid, buyout, list, cancel | Tier 1 `ModuleClient::AuctionHouseForBot`, core `ModuleActions.cpp` (`AuctionHouseAction`) | built in the core and SDK; **no module uses it**; a browse answers with an `AuctionListingEvent` |
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
  Stacks of trade goods are listed whole.
- *Price:* a multiple of the vendor sell price by quality: green x3, blue x4, epic x5 (knob `SellMultiplier<q>`), rounded; buyout equals the starting bid
  times 1.2. A multiple of the vendor price is a floor the market cannot go under: a player who undercuts it loses money to the vendor, so the house price
  stays near it.
- *Duration:* 12 hours, relisted once at 90 percent of the price if it did not sell, then sold to a vendor.
- *When:* an idle bot with at least `SellBagFraction` (default 60 percent) of its bag slots used, standing near an auctioneer or due its service visit
  (`ServiceEverySec`), lists at most `SellPerVisit` (default 5) items per visit.
- *Limit:* at most `ListedMax` (default 10) listings per bot at once. Gold from sales is collected from the mailbox by the core's own mail handling
  (to be checked: bots do not open mail yet; if not, the core credits them, which is a core change).

**Buying.**
- *Gear upgrade:* when `AutoGear` shows a slot a level behind (the bot's item in that slot is below its level band), the bot browses its auctioneer for
  its slot and class (`kBrowse`, filtered by level range and quality), and buys the cheapest item that is an upgrade by the same score `AutoGear` uses and
  costs at most `BuyMaxFraction` (default 40 percent) of its money above the reserve.
- *Supplies:* food, drink, potions and bandages stay a vendor purchase. The house is not used for them (vendor stock is unlimited and cheaper).

**Pace.** A bot visits the house at most every `AuctionEverySec` (default 1800) and the module makes at most `AuctionBudgetPerTick` house calls per tick across
all bots: each call is a browse that walks the house's lists in the core.

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
- Mail: whether a sold item's money reaches a bot (open question above).
- Telemetry: `auction_listed`, `auction_sold`, `auction_bought` events.

## Test plan

1. Unit tests of the pure parts (price rule, what to list, what to buy), like `supply_test.cpp`.
2. Sim: 100 bots, `Presence` off, `AuctionHouseBot.Sellers.Count = 0`; watch listings per hour, sales, the money total, and the core's world tick time
   (browses are the cost to watch).
3. Compare: the money total after 8 hours with and without the house.

## Decisions for the owner

- Whether bot listings and the core's `AuctionHouseBot` seller are both wanted; the first makes a market that players can use, the second is simpler and does not
  depend on the bots' play.
- Whether players may buy from bots at all (it makes bot gold real); and whether bot-sold gear may be soulbound-free items only (as above) or anything.
- The price multiples, which are guesses until the sim shows what bots earn.
- Whether any of this is wanted before bots level reliably: with no levelling there is nothing worth selling.
