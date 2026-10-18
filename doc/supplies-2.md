# Supplies 2 (layout 16 / protocol 11): reserve, potions, bandages, reagents, reachable vendors

Status: built and unit-tested (module: `supply_test`, `policy_test`, `settings_test`; core: compiled, layout round trip in `ModuleTier2Test`).
**Not run on the sim.** Builds on the protocol 10 supplies (ammunition, food, drink; see the README).

| What | Where | Setting |
|---|---|---|
| Money reserve | core keeps `level x Bot.Supplies.ReserveCopperPerLevel` copper (default 5) back from every purchase except ammunition | core |
| Health potions, mana potions | snapshot `health_potion_count`, `mana_potion_count`; `kVendor` bits 64, 128; `kConsume` 3, 4 in a fight below `UseHealthPotionBelowPct` (35) and `UseManaPotionBelowPct` (15), at most every 2 minutes | module `SupplyPotions` (off); core `Bot.Supplies.HealthPotionTarget`, `ManaPotionTarget` (3 each) |
| Bandages | `bandage_count`; `kVendor` bit 256; `kConsume` 5 for a resting bot below `UseBandageBelowPct` (60) that has **no food**, at most once a minute | module `SupplyBandages` (off); core `BandageTarget` (10) |
| Reagents | the core notes the item a refused cast was missing (`SPELL_FAILED_REAGENTS`) and names it in `reagent_entry` until the bot carries enough; `kVendor` bit 512 buys it | module `SupplyReagents` (off); core `ReagentTarget` (10) |
| Reachable vendors | the core checks the 4 nearest vendors of each kind against the navmesh, nearest first, and reports the first with a route; 24 path checks per world update at most | none |

Module low marks: `SupplyHealthPotionLow` (1), `SupplyManaPotionLow` (1), `SupplyBandageLow` (3). Stops: ammunition, food and drink, potions and bandages
(`goods_vendor_*`), reagents (`reagent_vendor_*`).

## Notes and limits

- The reserve does not apply to ammunition: a hunter without ammunition cannot fight at range.
- A potion is a spell cast with the item attached; the game's potion cooldown applies and shows up as a refusal. A bandage is refused while the bot has
  "Recently Bandaged", which the module also avoids by waiting a minute.
- The path check treats "no route" as closed, and "partial route" as closed only for a vendor close enough (250 yards) that a whole route should
  fit; farther ones are accepted since the pathfinder looks a limited way ahead. No navmesh for the map: accepted. A vendor behind a closed gate
  can still pass; the walk times out after 3 minutes and the trip backs off, as before.
- A choice that ran out of path checks is made again in the next update; the bot is shown no vendor meanwhile.
- Reagents are learned from a failed cast, so the first cast after a level-up still fails once. Which vendors sell the item is looked up once per item.
- Nothing about the Auction House.
