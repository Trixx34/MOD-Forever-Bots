# Economy and professions: what to switch on, in which order, and why the auction house stayed empty

Status 2026-11-14. Owner direction: **autogear stays off on prod**; bots earn gear from drops, quest rewards, crafting and the auction house.
This page traces each link of that chain from config key to core handler, says what the sim and prod show, and gives the exact keys and data
files to switch the chain on in prod, in order. Design background: `doc/economy.md`. All module keys below are `Module.forever-bots.<Key>` in
`worldserver.conf.d/modules.d/forever-bots.conf`; `Bot.*` keys are the core's (`worldserver.conf` or `worldserver.conf.d/*.conf`).
Why the profession skills stay at their start value on prod (2026-11-20) and the `Professions` keys: `doc/professions.md`.

## Why no bot ever listed anything (found 2026-11-14, fixed on `feature/economy-professions`)

The sim had `Auction = 1`, `Errands = 1`, `Taxi = 1`, `SellVendor = 1` and still never wrote a single `auction:` line, and no errand of
`kind=auction` ever started (179 errand starts on 2026-11-14, all `kind=train`). Four module bugs, each enough on its own:

1. **Equipment was never a candidate.** `CandidateOf` (`src/main_sales.cpp`) marked every looted weapon and piece of armour `Needed`. The core's
   `kList` lists single non-stackable items, which in practice are equipment, so the book of listable items stayed empty, no auction errand was
   ever due, and no visit had anything to list. Now equipment is marked `Equipment` and listed after a hold (below).
2. **The bag rule never fired.** A visit needed `AuctionBagFullPct` of the bags used, computed as `16 - min(free, 16)` over an assumed 16-slot
   backpack, while the snapshot's `bag_free_slots` counts every bag (the sim gives bots four bags, `Bot.Bags.Count = 4`). A bot with 17 or more free
   slots counted as 0 percent full. Now a bot in reach of the auctioneer with something to list visits whatever its bags hold
   (`auction::ListingVisitDue`); the bag rule only remains for a bot with nothing ready.
3. **Visits far from the auctioneer spent the pace.** The snapshot's `auctioneer_guid` is the nearest auctioneer *on the map*, at any distance,
   so the visit (and the upgrade browse) ran for bots anywhere on the continent, the core refused them ("auctioneer not found or cannot interact
   with it", a retry), and `LastVisitMs` was set anyway: a bot that then arrived waited `AuctionEverySec`. Now visits and browses are made only in
   reach (`auction::InReach`, 5 yards 2D, the core allows the auctioneer's combat reach + 4).
4. **The bot walked on before the visit.** An auction errand ended the moment the bot came in reach, but the visit is a separate 10 s timer fed
   every 2 s. Now the bot stands at the auctioneer for 25 s (`kAuctionDwellMs`) before the errand ends.

A fifth fix concerns crafting: **vendor sales sold the crafting materials.** `SellVendor` sold every white non-equipment item a bot looted, by
entry and all of it, so cloth, meat, ore, herbs and leather went to the vendor. Trade goods (item class 7) are now kept unless `SellVendorGoods = 1`.

Tests: `tests/auction_test.cpp` (`ABotWithBagsVisitsWhenInReachWithSomethingToList`, `TheAuctioneerIsInReachWithinFiveYards`,
`EquipmentIsListedAfterItsHold`), `tests/policy_test_10.cpp` (`AuctionErrandStandsAtTheAuctioneerBeforeItEnds`), `tests/sell_test.cpp`
(`TradeGoodsAreKeptUnlessSwitchedOn`), `tests/settings_test.cpp` (the new keys). Not run on the sim yet.

### "Not listing items worth equipping": the hold

The loot event carries entry, count, quality, class and vendor price, nothing about required level, binding or whether the item beats what the bot
wears. So a looted green weapon or armour piece is held until the bot is `AuctionGearHoldLevels` (default 2) levels above the level it looted it
at. Meanwhile the module's bag gear pass (kAutoGear spell 2, `Bot.Actions.EquipUpgrades`, on a timer, also with `AutoGearing = 0`) wears it if it
is an upgrade; a worn BoE item is soulbound, and the core's `HandleAuctionSellItem` refuses a soulbound item (`CanBeTraded`), so a worn item can
never be listed (the refusal drops it from the book). The hold covers the items the bot could not wear yet when it looted them. It is a proxy;
the exact rule needs the core change at the end of this page.

**The exact rule (`AuctionGearExact`, off by default; branch `feature/loot-item-facts` in core, SDK and module).** A core with the change sends
the item facts in the loot event (`required_level`, `inventory_type`, `bound`, `upgrade_score`, `usable`, `source`). With `AuctionGearExact = 1`:
an item the core scores as an upgrade the bot would wear now is `Needed` and never listed; a soulbound item is `Bound`; a piece the bot may use
(`usable`) whose required level is above the bot's and at most `AuctionGearHoldLevels` above its loot level is kept until the bot is one level
past that required level (the bag pass wears it first); anything else is listed at once. A core without the facts: the hold, as before.

## The chain, link by link

| Link | Wired? | Module keys | Core keys (default) | Data files | Sim (2026-11-14) | Prod (read-only 2026-11-14) |
|---|---|---|---|---|---|---|
| Learning professions | Yes, core-side | none needed; `TrainEverySec` only adds a `kTrain` when a bot happens to stand by a trainer | `Bot.Professions.Enabled` (1), `Bot.Learn.TrainerSpells` (1) | none | every bot has its two professions at 1/75 at level 1-7 | 749 bots; profession max reaches 300; ranks and recipes are taught |
| Gathering | Yes (`kGather`) | `Gathering = 1`; later `GatherWalk = 1` + `GatherNodesFile` | none | `data/gather_nodes.txt` is **empty** (needs `tools/gen_gather_nodes.py` against the world DB) | **off**: `forever-bots.conf` sets `Gathering = 1` but `modules.d/zz-dungeon-run.conf`, read later, sets `Gathering = 0`; all Mining, Herbalism and Skinning at 1 | key absent (off); Herbalism above 1 for 60 of 111, Skinning 21 of 113, Mining 2 of 132 (from an earlier config) |
| Crafting | Yes (`kSkillUp`, `kNeedsFocus` walk) | `Crafting` (1 by default), `CraftingFociFile` | none | `data/crafting_foci.txt` (74 anvils, forges, fires) | First Aid above 1 for 26 of 140 (max 22), Cooking 2, Tailoring 4 | `CraftingFociFile` absent: no walk to a forge or fire. Bags hold the unused materials: Stringy Wolf Meat in 243 bots (7779), Chunk of Boar Meat 158 (9403), Copper Ore 98 (427), Peacebloom 85, Light Leather 81 |
| Wearing what drops, is crafted, is bought | Yes (bag gear pass) | none (works with `AutoGearing = 0`) | `Bot.Actions.EquipUpgrades` (1) | none | 11 `autogear bag upgrade` lines in Server.log | n/a |
| Selling to vendors | Yes | `SellVendor`, `SellEverySec`, `SellPerVisit`, `SellVendorGoods` (new, 0); `VendorSellGear` (1) sells outclassed bag gear | `Bot.Vendor.SellGear` (1) | none | 121 `vendor_sell_gear` lines | `SellVendor` absent (off) |
| Supplies and service visits | Yes | `Supplies`, `ServiceEverySec` (600) | `Bot.Actions.Supplies` (1), `Bot.Supplies.ReserveCopperPerLevel` (5) | none | on | `Supplies` absent (off) |
| Walking to a capital (trainer, auctioneer) | Yes | `Errands`, `ErrandTrainerFile`, `Taxi` | `Bot.Taxi.Enabled` | `data/errand_trainers.txt` (absolute path: the default is relative) | 179 starts, 80 `no_route`, all `kind=train` | off |
| Listing on the auction house | **Was broken (above), fixed** | `Auction`, `AuctionEverySec`, `AuctionGearSell` (new, 1), `AuctionGearHoldLevels` (new, 2), prices and limits | `kList` needs no switch; `Rate.Auction.*`; `AuctionHouseBot.Sellers.Count = 0` | none | 0 listings, `auctionhouse` 0 rows | `auctionhouse` 0 rows |
| Buying upgrades | Wired, never reached (needs a bot in reach) | `AuctionBuy`, `AuctionBuyMaxPct` | protocol 19 browse by filter, `BotGearUpgradeScore` | none | `AuctionBuy` not set | off |
| Mail (sale money, bought items, expired listings) | Yes, core-side | none | `Bot.Mail.Collect` (1), `Bot.Mail.CollectEverySec` (30) | none | `mail` 0 rows | `mail` 0 rows |

Refusals: the sim journal since 2026-11-13 has 3301 `social_refused` lines, all guild ones (3266 invite refused, 35 guild name taken), none from
the economy. Prod bags hold at least 31 green weapons and armour pieces in 30 bots (counted from `item_sparse` hotfix rows only, so a lower bound):
the stock the fixed listing would offer.

### Notes per link

- **Learning.** `BotMgr::OnBotLevelChanged` (level-up and login) calls `BotProfessions::Teach`, which teaches every available spell of the bot's two
  professions and of First Aid and Cooking for free, ranks included (the passes repeat until nothing is taught). Skill gains between level-ups wait
  for the next level or login. The trainer errand is therefore not needed for professions; `TrainingDueSlot` sends every bot whose skill is below
  its maximum, which is every bot, so on the sim it was a stream of long trips at level 1-7, 80 of 179 ending in `no_route`.
- **Gathering.** The core gathers within 30 yards of the bot (`GatherAction`); without `GatherWalk` and a generated node table a bot gathers only
  what it passes. Skinning works on its own kills.
- **Crafting.** The core picks a recipe the bot knows and has materials for. Smelting needs a forge and cooking a fire: without `CraftingFociFile`
  the answer is `kNeedsFocus` and nothing happens, which is what prod's bags show (meat and ore piling up). Crafted items do not raise the loot event,
  so they are not auction candidates; a crafted upgrade is worn by the bag gear pass. With the core change and `Modules.ItemReceivedEvent.Enabled = 1`
  a bot's crafted items, quest rewards and items taken from auction mail raise the loot event too (with `source` 1, 2, 3): they join the auction
  book, never the vendor sale book.
- **Vendor.** `VendorSellGear` (on by default, and the core's `Bot.Vendor.SellGear` is on by default) sells green-or-lower bag gear the worn gear
  outclasses on every vendor visit. Those items are exactly the ones worth listing. With the auction on, set `VendorSellGear = 0`, or accept that a
  vendor visit before the hold ends takes them (a later listing is then refused "item is no longer in the player's bags" and dropped).
- **Listing.** Gates in the core (`ModuleActions.cpp` `AuctionHouseAction` case 3/5, then `WorldSession::HandleAuctionSellItem`): the bot online, the
  item in its inventory, the auctioneer in interaction reach (`GetNPCIfCanInteractWith`, combat reach + 4 yards), the deposit affordable, run time
  120 min (`MIN_AUCTION_TIME` is 2 h in the fork), the item a single non-stackable piece that `CanBeTraded` (not soulbound, not conjured, not an
  equipped bag), not already on the house. A refusal inside `HandleAuctionSellItem` comes back only as "auction was not listed".
- **Buying.** A browse and a buyout also need the bot in reach. A bot goes to the auctioneer only on an auction errand, and that starts only when it
  has something to list, so a bot with nothing to sell never shops. Module work still to do: a buy errand (for example when `BuyBudget` is above a
  threshold and the bot is in or near a capital).
- **Mail.** `WorldSession::CollectMailAsBot` takes money and items from delivered `MAIL_AUCTION` mail (sale money after `MailDeliveryDelay`, 3600 s),
  so the empty `mail` table is expected while nothing is listed. An expired listing comes back by mail into the bags but not into the module's book
  (no loot event): it is not listed again (`AuctionRelistPct` is not used yet) and stays until a vendor visit or the junk sale takes it. With the
  core change and `Modules.ItemReceivedEvent.Enabled = 1` it comes back into the book (source 3) and is listed a second time without a new hold;
  `PlanListings` leaves it after the second listing.

## Switching it on in prod, in order

Each step is worth a day of watching before the next. Paths assume the prod runtime layout
(`/srv/forever/prod/modules_runtime/forever-bots/data/`); copy the data files there from this repository first.

**Step 0: keep autogear off, check the core defaults.** Module `AutoGearing = 0` (default). Core `Bot.Actions.AutoGear = 0` as well, so a
player's `.autogear` order cannot dress bots either; `Bot.Actions.EquipUpgrades = 1` stays (it only wears what is in the bags). Check
`Bot.Professions.Enabled = 1`, `Bot.Learn.TrainerSpells = 1`, `Bot.Mail.Collect = 1`, `Bot.Mail.CollectEverySec = 30`,
`AuctionHouseBot.Sellers.Count = 0`, `Rate.Auction.Deposit = 1`, `Rate.Auction.Cut = 1` (all defaults or already set).

**Step 1: crafting and gathering.**
```
Module.forever-bots.Crafting = 1
Module.forever-bots.CraftingFociFile = /srv/forever/prod/modules_runtime/forever-bots/data/crafting_foci.txt
Module.forever-bots.Gathering = 1
```
Watch: First Aid, Cooking, Mining and the crafting skills rise in `character_skills`; meat and ore stacks shrink; `craft_walk` events.
`GatherWalk = 1` with `GatherNodesFile` only after `data/gather_nodes.txt` is generated (today it is empty and the walk does nothing).

**Step 2: vendors and supplies.**
```
Module.forever-bots.Supplies = 1
Module.forever-bots.ServiceEverySec = 600
Module.forever-bots.SellVendor = 1
Module.forever-bots.SellEverySec = 1800
Module.forever-bots.SellVendorGoods = 0
```
Gives bots money (deposits and purchases need it) without selling crafting materials.

**Step 3: errands to the capitals.**
```
Module.forever-bots.Taxi = 1
Module.forever-bots.Errands = 1
Module.forever-bots.ErrandTrainerFile = /srv/forever/prod/modules_runtime/forever-bots/data/errand_trainers.txt
Module.forever-bots.TrainEverySec = 600
```
and core `Bot.Taxi.Enabled = 1`. The errands are the only way a bot reaches an auctioneer. Train errands come with them (see Learning: they are
not needed for professions, and they are many); the defaults `ErrandDailyMin`, `ErrandGapMin`, `ErrandPerCity` keep the count down.

**Step 4: listing.**
```
Module.forever-bots.Auction = 1
Module.forever-bots.AuctionEverySec = 1800
Module.forever-bots.AuctionGearSell = 1
Module.forever-bots.AuctionGearHoldLevels = 2
Module.forever-bots.AuctionSellPctGreen = 300
Module.forever-bots.AuctionSellPctBlue = 400
Module.forever-bots.AuctionSellPctEpic = 500
Module.forever-bots.AuctionBuyoutPct = 120
Module.forever-bots.AuctionMinVendorCopper = 100
Module.forever-bots.AuctionPerVisit = 5
Module.forever-bots.AuctionListedMax = 10
Module.forever-bots.AuctionCallsPerTick = 4
Module.forever-bots.VendorSellGear = 0
```
Watch: `event errand_start ... kind=auction`, `auction: bot G listed item ...`, `auction: bot G list_refused ... reason=...`, `auctionhouse` rows,
then `mail` rows and bot copper after the 2 h run plus the 1 h mail delay. `AuctionGoodsSell` stays 0 until the gear listings behave.

**Step 5: buying.**
```
Module.forever-bots.AuctionBuy = 1
Module.forever-bots.AuctionBuyMaxPct = 25
```
Needs a core with protocol 19 (browse by filter with `upgrade_score`). Today it only happens during the 25 s a bot stands at the auctioneer to list.

**Step 6 (optional): goods.** `AuctionGoodsSell = 1`, `AuctionGoodsPct = 150`, `AuctionGoodsStackMax = 20`, `AuctionGoodsPerEntryMax = 1`.

## Risks and the limits that hold them

- **Price floor.** Every listing starts at `AuctionSellPct*` percent of the vendor price and never below vendor price + 1 copper
  (`auction::StartingBid`), so a bot never sells for less than a vendor pays. Buyout = bid x `AuctionBuyoutPct`.
- **Price ceiling.** Listings: at most 20x the vendor price (the key's range), so a bot cannot ask absurd prices. Purchases: at most
  `AuctionBuyMaxPct` of the money above the reserve (`Bot.Supplies.ReserveCopperPerLevel` x level) per purchase, and only an item the core scores
  as an upgrade; `ChooseBuy` takes the best upgrade per copper. A bot never spends its reserve.
- **Gold drain.** The deposit of an unsold listing is lost and the house takes its cut on a sale: both are sinks that make gold leave the bots
  slowly. `AuctionListedMax`, `AuctionPerVisit`, `AuctionEverySec` and `AuctionMinVendorCopper` cap what can be lost per bot. Bots buying from bots
  move gold between bots minus the cut. Watch the total of `characters.money` over bot accounts (162.8 gold on prod today) per day.
- **Listing what the bot needs.** Quest items and soulbound items are never listed (a refusal drops them); a worn item is soulbound; equipment waits
  `AuctionGearHoldLevels`. What the hold cannot see: an item for a level more than the hold above the bot, or for a slot the bag pass does not
  fill. The core change below closes that.
- **Listing what is worth equipping for someone else.** That is the point: players and other bots can buy it.
- **Crafting materials.** Kept from vendors by default (`SellVendorGoods = 0`); listed only with `AuctionGoodsSell`.

## Core changes needed (minimal)

Items 1 and 2 are made on `feature/loot-item-facts` (core, SDK, module; not merged, not built by Jenkins yet), additive within protocol 29: new
optional fields 9 to 14 of `PlayerItemLootedEvent`, the same hook `PlayerScript.OnLootItem`, no new hook (a new hook would need a new
`module approve` and a core that knows it), no protocol or layout bump. Switching on: core `Modules.ItemReceivedEvent.Enabled = 1` (sources 1 to 3,
bots only) and module `AuctionGearExact = 1`. Keep `Modules.ItemReceivedEvent.Enabled = 0` while the module that runs is older than this branch:
it would take a crafted item for loot and offer it to the vendor sale. Item 3 is not made: the snapshot is the Tier 2 layout, and any change
to it bumps `kTier2LayoutVersion` and the protocol (`ModuleTier2Layout.h`, the growth rule), which protocol 29 must not.

1. **Loot event facts** (the exact "do not list what the bot needs" rule): add to `PlayerItemLootedEvent` (`proto/events.proto` in the SDK and
   `ModuleWire::PlayerItemLooted` in the core) `required_level`, `inventory_type`, `bound` (`item->IsSoulBound()`) and `upgrade_score`
   (`BotGearUpgradeScore(*player, *item->GetTemplate())`, the score the browse already uses), filled where `ScriptMgr::OnPlayerItemLooted` builds the
   event. The module then marks `Needed` when `upgrade_score > 0`, or when the required level is above the bot's and within the bot's next few levels
   and the bot can use the item; `Bound` from `bound`. The hold becomes a fallback.
2. **Items that do not come from a loot window** (crafted items, quest rewards, an expired listing back by mail) never enter the book. Either raise
   the same event for a bot's `kSkillUp` result and for items taken from auction mail by `CollectMailAsBot`, or give bots a bag description action
   (entry, guid, count, quality, sell price, bound, upgrade score) as `doc/economy.md` already asks.
3. **Optional:** the total bag slots in the snapshot next to `bag_free_slots`, so `AuctionBagFullPct` means what it says.
