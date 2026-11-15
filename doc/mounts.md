# Mounts, riding training and flight points (design 2026-11-14)

Status: built and unit-tested (`mount_plan_test`, `policy_test`, `settings_test`). **Not run on a sim.** Everything new is off by default (`MountBuy`, `RidingTraining`).

## What was already there

- **Mount and dismount** (`kMount`, policy step in `bot_policy_turn_town.cpp`): a bot at `MountMinLevel` (40) or above mounts for a walk of more than 100 yards and dismounts near the
  goal, in combat or when the walk ends. The core refuses it when the bot has no riding skill or no mount; the policy then pauses 2, 4, ... 60 minutes (`MountFails`).
- **Flight points** (`Taxi`, `doc/taxi.md`): the core gives every bot every flight node of its faction and charges nothing (`Bot.Taxi.*`), so "learn the flight master when passing it"
  is not needed for bots (the owner's rule; alts keep the player's rules). The module walks to a flight master on the way and sends `kTaxi`. Nothing was added here.
- `riding_training.*` (rank table, next rank due) and the keys `RidingTraining`, `RidingRankFile`: pure logic with an empty table, never wired to a bot.

## What the world DB has (read only, forever-test, 2026-11-14)

- Riding trainers (npcflag 17, subname "Riding Trainer"): Randal Hunter 4732, Ultham Ironhorn 4772, Jartsam 4753 (Alliance); Velma Warnam 4773, Kildar 4752, Xar'Ti 7953, Kar Stormsinger 3690 (Horde).
  They stand in the race villages beside their mount vendors, not in the capitals. Every one teaches **33388 Apprentice Riding** (level 40, 900000 copper = 90 gold) and **33391 Journeyman Riding**
  (level 60, 9000000 copper = 900 gold), to every race of the side (`trainer_spell`; Forever's own rows 10xxxxxx, 195 for Velma).
- Mount vendors: horse (Human), ram (Dwarf), mechanostrider (Gnome), saber (Night Elf), wolf (Orc), raptor (Troll), undead horse, kodo. Mounts are `item` class 15 subclass 5: level 40 for 200000
  copper (20 gold), level 60 (epic) for 2000000 (200 gold). `data/mount_vendors.txt` has 11 vendors and 82 mounts with their spawn positions.
- The item masks (`AllowableRace`) are faction wide, so the table takes the race from the mount type; whether the core refuses a Human buying a ram is unverified.

## The plan (`mount_plan.*`, `main_feeds.cpp`, `bot_policy_turn_move.cpp`)

The module sees neither a bot's riding skill nor its bags. Its sign is the core's refusal of a mount request: `BotPolicy::MountRefusals` (> 0 until a mount works). A bot with no profession
to train and no auction errand, **refused a mount**, gets one trip of the town-errand scheduler (`Errands = 1`; daily minutes, gap and per-city limits apply):

1. **Riding trainer** (`RidingTraining = 1`): when `riding::NextRank` finds a rank due (level, copper) and the bot can still pay the cheapest mount its race may buy after it. The errand
   walks to the nearest trainer of its side on its map (`data/riding_trainers.txt`, skill 762, same format as the capital trainers) and sends `kTrain` (the existing errand code). The module books the
   skill it should have learned (`SkillAfterTraining`) in memory.
2. **Mount vendor** (`MountBuy = 1`): with the riding skill booked (or assumed to be the first rank when `RidingTraining = 0`), the best mount of its race on its map that its level, skill and
   money allow (highest level, then cheaper, then nearer). The errand walks to the vendor; within 8 yards, with the snapshot's nearest vendor in reach, the policy hands a `BuyRequest`
   (vendor unit reference, item) to the Run() thread, which calls `ModuleClient::BuyItemForBot` and gives the answer back (`FinishBuy`); the errand ends with it (`mount_buy`, `mount_buy_done`,
   `mount_buy_refused` event lines, plus `errand_done` / `errand_failed`).
3. **Gold rules**: a trip uses only the copper above `MountReserveCopperPerLevel` (5000 = 50 silver) per level; the rank trip also keeps the price of the cheapest mount. A failed trip holds
   the bot off for 6 hours, a trip the scheduler ended (time limit, called away) for 1 hour, a bought mount for good. The core keeps its own supply reserve on top.
4. Mount and dismount on the road, and flights for the long walk to the trainer or vendor (`Taxi`), are the existing rules.

Limits: one mount per bot (no epic upgrade later); a bot of a race the table has no vendor for (Skyborne) never trips; the trainer and vendor must be on the bot's map; after a module restart the
book is empty, so a bot that has no mount is refused again and goes (at most once more).

## Needs from the core, unverified

- That `kTrain` at a riding trainer learns 33388 / 33391 for a bot with the gold (`kApplied` means "asked", not "learned"; nothing tells the module the skill).
- That `BuyItemForBot` works for a mount item with the snapshot's `vendor_guid` (the SDK's comment says that is the guid purchases use) and that the core's mount-item rules (race, skill) accept it.
- **That the bought mount is usable (read in the core, `ModuleBotActions.cpp` `MountAction`):** `kMount` casts the fastest mount *spell the bot has learned* (its spell map) and refuses with "bot has no mount"
  otherwise. A mount item bought from a vendor is only a bag item until it is used once (its "Use: teaches you how to summon this mount" spell), and neither `BuyItemForBot` nor any Tier 2 action uses an item.
  So, as the core stands, a bought mount does nothing. The core needs one of: (a) `MountAction`, when no mount spell is known, finds a mount item in the bags and learns/casts its spell (and the module's
  purchase then makes the next `kMount` work); (b) `BuyItemForBot` for a bot learns the mount spell of a mount item at once; (c) a Tier 2 "use item" action. (a) is the smallest and needs no module change.
  Until then keep `MountBuy` off; the riding training step (`RidingTraining` with the errands) works without it but is only useful together with it.
- A snapshot field for the riding skill and for whether the bot owns a mount would replace the refusal heuristic and the book (optional proto fields only).
