# Level-safe places, below-map recovery, auction reach, death and refusal logging (2026-11-18)

Branch `feature/level-safe-zone-move-20261118`. Code: `src/level_safe.*`, `src/below_map.*`, `src/bot_policy_level_safe.cpp`; tests: `tests/level_safe_test.cpp`, `tests/main_util_test.cpp`.

## What the sim showed (9 h, 400 bots)

- About 15% of the deaths came from a few low bots in high places: bot 13 (Human warrior, level 6) died 941 times and bot 154 (priest, level ~6) 505 times, both to Skeletal Horror (entry 202, level ~24) at the Raven Hill graveyard in Duskwood. Bot 13 died in western Elwynn (places of level 5), its ghost appeared at Raven Hill, the ghost fallback put it back there and the spirit healer revived it among level 24 creatures; the death loop and death area escapes skip a bot in combat, so it never left.
- Bot 242 (level 5) ended in Feralas, bot 27 (level 6) in the Wetlands. Quest start walks go up to 2000 yards (`kWorldStartRadiusYards`); `kRedQuestMargin` limits the quest's level, not the ground or the giver's zone.
- About 101k `no_route` events in 9.7 h, 30k of them for walks under 100 yards (bots under the map: z=19 under the Ironforge auction house at z=505).

## How the level of a place is known

There is no zone level table in the module. The grind spots (`data/grind_spots.txt`, creature level ranges with positions, made from the world database) give it: the level of a place is the lowest minimum level of the spots within `LevelSafeRadiusYards`. With fewer than two spots that near the place is unknown and never judged (today's behaviour); a town whose only spot is its guards (Stormwind 45, Darkshire 29) is not "high". No new data file.

A place is too high for a bot when that level is above the bot's level plus `LevelSafeMarginLevels` (a walk to something new: a quest giver, a hunting area) or plus `LevelSafeEscapeMarginLevels` (moving a bot that is already there: the escape, the graveyard, a hand-in walk). The wide margin is never below the narrow one, so a bot is never moved out of a place a walk would take it to.

Checked against the data: Northshire and Goldshire are 1, the Elwynn places where bot 13 died are 5, Raven Hill is 18. For a bot at a quest's own level, 309 of 9354 quest giver and ender points are too high with margin 5 (212 with margin 10): those quests are skipped (often a giver among high creatures or guards).

Graveyards: the module has no graveyard table either. Every place where a ghost is seen to rise from its corpse is remembered (per map and side) as a graveyard. A restart forgets them; the sim's deaths refill the list within minutes.

## Keys

| key | default | effect |
|---|---|---|
| `LevelSafeWalks` | 1 | A quest walk whose end, or a point of its straight line past the first 300 yards (checked every 100 yards), lies too high is dropped: a hunt tries the quest's next area, otherwise the quest is left alone for 30 minutes and the planner gives another step. Event `quest_walk_blocked ... reason=level_unsafe kind=giver|hand_in|hunt level= margin= place_min= at= dest= dist=`. |
| `LevelSafeGraveyard` | 1 | Once per death: when the corpse or the graveyard lies too high (wide margin), the nearest remembered graveyard of the bot's map and side that fits its level (narrow margin) becomes the ghost fallback's graveyard. A corpse too high: the ghost is teleported there at once and asks the spirit healer (`ghost_fallback reason=level_unsafe`, `ghost_fallback_teleport ... graveyard=level_safe`); a graveyard too high only changes where a later fallback goes. Event `level_safe_yard` (`result=none_known` when no fitting graveyard is known yet: today's behaviour). |
| `LevelSafeEscape` | 1 | A solo bot, alive and out of combat, standing in a place too high (wide margin), is teleported to the nearest grind spot for its level whose own place is not too high, at most once per `LevelSafeEscapeCooldownSec`. Not for party members, dungeon routes, scenarios, chat orders, flights, transports. Event `level_unsafe_escape`. |
| `LevelSafeMarginLevels` | 5 | 1 to 30. |
| `LevelSafeEscapeMarginLevels` | 10 | 1 to 40. |
| `LevelSafeRadiusYards` | 250 | 50 to 1000. |
| `LevelSafeEscapeCooldownSec` | 600 | 60 to 86400. |
| `BelowMapRecover` | 0 | A living bot whose nearest known place (quest givers and anchors, grind spots, hubs) within 60 yards lies more than `BelowMapDropYards` above it for 20 s, or with `BelowMapStormCount` no-route answers for walks of 6 to 100 yards within 5 minutes, is teleported to the nearest known place (z_below: that place; storm: the nearest place of its side), at most once per `BelowMapCooldownSec`. Event `below_map_recover reason=z_below|no_route_storm`. |
| `BelowMapDropYards` | 60 | 20 to 1000. |
| `BelowMapStormCount` | 8 | 0 to 100; 0 = the storm sign off. |
| `BelowMapCooldownSec` | 600 | 60 to 86400. |
| `AuctionWalk3D` | 0 | The reach to the auctioneer is 4.5 yards in 3D, not 5 in 2D: the listing visit waits for it, the auction errand counts the bot as arrived only then and walks it to the auctioneer until then. |
| `AuctionLogReach` | 1 | Logging only: `event auction_reach: bot= check=failed_3d walk3d= dist2d= dist3d= bot_z= auctioneer_z=` (once per bot every 10 minutes) when the 2D check passes and the 3D one does not; the `list_refused` and `buy_failed` lines end with the same fields. |
| `LogRefusedReason` | 1 | Logging only: the `bot N <action> not done: <status>` lines (gather, craft, vendor, train, ...) end with `reason=N`, the ActionResult reason the core put in the result ring for that action (0 = none; the core classifies quest accepts, most other refusals give 0, so the text reason stays in the core log). |
| `DeathsTopEveryMin` | 60 | Logging only; 0 = off. `forever-bots: deaths_top: window_min= deaths= bots_online= bots_died= median= median_died= top10= top10_pct= top=guid:n,...` from the policy's own death count (`median` counts the bots in view that did not die as 0). |

Defaults: the level-safe keys are the fix and default on; the below-map teleport and the auction walk move bots and default off (the sim first); the rest is logging and defaults on. Every key is in `config/modules.d/forever-bots.conf.example`.

## Limits

- The core refuses a teleport in combat, so a bot fighting in a high place cannot be moved; it is logged (`result=in_combat`), and when it dies the graveyard rule revives it elsewhere. That is the escape for a bot that never leaves combat.
- A graveyard is only known once a ghost was seen there. Right after a restart the first deaths in a high place keep today's behaviour (`level_safe_yard ... result=none_known`).
- The walk check uses the straight line from the bot to the destination, not the core's path.
- Grind spots are creature spawns: a quest point among guards or elites can count as high (about 3% of quest points at margin 5 for a bot at the quest's level).

## What the sim read-out should measure

Deaths per bot and `deaths_top` (top10 share, median); repeat deaths at one spot (deaths within 60 yards of each other per bot); `quest_walk_blocked reason=level_unsafe` per hour and per quest; `level_safe_yard` (fitting vs `none_known`), `level_unsafe_escape` and `teleport_refused` counts; `no_route` per bot-hour (and with `BelowMapRecover = 1`, `below_map_recover` per hour and the short-walk share of `no_route`); levels gained per hour.
