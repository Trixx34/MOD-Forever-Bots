# Mounts: what the core must still provide (2026-11-16)

Read in `ModuleBotActions.cpp`, `ModuleTier2Layout.h` of the core at the time of writing. The module side is finished and waits on these. Nothing here is a core edit made by the module team.

1. **A mount item must become a mount spell (blocking).** `MountAction` (`kMount`) casts the fastest *learned* mount spell and answers "bot has no mount" otherwise. A vendor mount is an item with the effect
   Learning (55884) that teaches the mount spell when used; `BuyItemForBot` only puts it in the bag and no Tier 2 action uses an item. Smallest fix: in `MountAction`, when no mount spell is known, look for a mount
   item in the bags (class 15, subclass 5), learn its taught spell (the item's second effect, e.g. 6777 for 5864) and continue. Alternatives: `BuyItemForBot` learns the spell at once for a mount item; or a new
   action `kUseItem` (item entry or bag slot) that the module could also use for other items.
2. **Riding trainers must teach the riding skill to a bot.** Confirm `kTrain` at 4732 / 4752 / 4753 / 4772 / 7953 / 7954 / 3690 learns 33388 (level 40, 90 gold) and 33391 (level 60, 900 gold), spends the money,
   and sets the riding skill (75 / 150) so `MountAction` and the mount's skill check pass. Velma Warnam (4773) teaches 33389 / 33392 through trainer 195 and her `1004773` rows have no `creature_trainer` row: either
   add the row (a world SQL change) or confirm 33389 gives the riding skill and 33392 does not need 33388.
3. **Optional snapshot fields** (new optional proto fields, no layout break): `riding_skill` (0, 75, 150), `has_mount` (a learned ground mount spell exists), and the spell id of the best mount. They replace the module's
   refusal heuristic and its in-memory book (which is lost on a restart: a mountless bot tries once more).
4. **Race rule for mount items.** `AllowableRace` is faction wide in the item data; confirm whether `BuyItemForBot` refuses a Human buying a ram (module assumes it is allowed but buys only its own race's mounts).
5. **Flight masters need nothing new**: `kTaxi`, `taxi_master_*`, `in_flight` and `Bot.Taxi.*` exist. A snapshot field for the flight's landing node would let the module skip flights the core refuses for too little gain
   (today it walks to the master, is refused, and holds off `TaxiBlockedSec`).
