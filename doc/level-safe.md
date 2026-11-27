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
| `LevelSafeLogThrottleSec` | 60 | 0 to 3600; logging only (2026-11-19). `quest_walk_blocked reason=level_unsafe`, `level_unsafe_escape result=no_spot` and `level_safe_yard result=none_known` repeat for the same bot, quest and reason at most once per this; 0 = every line. The block itself is unchanged. The `deaths_top` line ends with `level_safe_lines=` (written) and `level_safe_held=` (held back) for the period. |
| `BelowMapRecover` | 0 | A living bot below every known place (quest givers and anchors, grind spots, hubs) within 60 yards by more than `BelowMapDropYards` (the lowest of them is the ground) for 20 s and five looks in a row (one look every 2 s), or with `BelowMapStormCount` no-route answers for walks of 6 to 100 yards within 5 minutes, is teleported up (z_below: that lowest place; storm: the nearest place of its side), at most once per `BelowMapCooldownSec`. Event `below_map_recover reason=z_below|no_route_storm`. Skipped (2026-11-19, see below): `below_map_skip reason=on_transport|swimming|on_path|near_ground`. |
| `BelowMapDropYards` | 60 | 20 to 1000. |
| `BelowMapStormCount` | 8 | 0 to 100; 0 = the storm sign off. |
| `BelowMapCooldownSec` | 600 | 60 to 86400. |
| `AuctionWalk3D` | 0 | The reach to the auctioneer is 4.5 yards in 3D, not 5 in 2D: the listing visit waits for it, the auction errand counts the bot as arrived only then and walks it to the auctioneer until then. The z-aware stand point (2026-11-19, below): the walk goes to the live auctioneer's own x, y, z; an auctioneer whose stored z lies more than 60 yards from its vendors, banker or graveyard is skipped. |
| `AuctionLogReach` | 1 | Logging only: `event auction_reach: bot= check=ok_3d|failed_3d|skip_bad_z|ignore_z walk3d= dist2d= dist3d= bot_z= auctioneer_z= ref_z= in_reach=` once per bot every 10 minutes for a bot in 2D reach of the auctioneer (before 2026-11-19 only `check=failed_3d`); the errand walk writes the same line with `where=errand` (no `in_reach=`); the `list_refused` and `buy_failed` lines end with the dist/z fields. |
| `LogRefusedReason` | 1 | Logging only: the `bot N <action> not done: <status>` lines (gather, craft, vendor, train, ...) end with `reason=N`, the ActionResult reason the core put in the result ring for that action (0 = none; the core classifies quest accepts, most other refusals give 0, so the text reason stays in the core log). |
| `DeathsTopEveryMin` | 60 | Logging only; 0 = off. `forever-bots: deaths_top: window_min= deaths= bots_online= bots_died= median= median_died= top10= top10_pct= top=guid:n,...` from the policy's own death count (`median` counts the bots in view that did not die as 0). |

Defaults: the level-safe keys are the fix and default on; the below-map teleport and the auction walk move bots and default off (the sim first); the rest is logging and defaults on. Every key is in `config/modules.d/forever-bots.conf.example`.

## Limits

- The core refuses a teleport in combat, so a bot fighting in a high place cannot be moved; it is logged (`result=in_combat`), and when it dies the graveyard rule revives it elsewhere. That is the escape for a bot that never leaves combat.
- A graveyard is only known once a ghost was seen there. Right after a restart the first deaths in a high place keep today's behaviour (`level_safe_yard ... result=none_known`).
- The walk check uses the straight line from the bot to the destination, not the core's path.
- Grind spots are creature spawns: a quest point among guards or elites can count as high (about 3% of quest points at margin 5 for a bot at the quest's level).

## Arm C fixes (2026-11-19, branch `fix/arm-c-small-20261119`)

Arm C (400 bots, 7.9 h, `BelowMapRecover 1`, `AuctionWalk3D 1`) showed three small faults. Tests: `tests/arm_c_fixes_test.cpp` (with the arm C positions).

**Level-safe line spam.** Bot 225 logged `quest_walk_blocked reason=level_unsafe` 4,655 times (half of all 9,266): the planner gives the same step every turn and it is dropped every turn. The drop and `NoteLevelUnsafe` stay; only the line is throttled per bot, quest and reason (`LevelSafeLogThrottleSec` 60). Why the planner hands the same step out again (bot 225: a hand-in or a hunt area that the 30-minute block does not cover?) is not answered here.

**Auction reach fails on height.** All 194 `auction_reach` lines were `check=failed_3d`, by height: 2D 3.7 yards, 3D 14.2 (bot_z -43.1, auctioneer_z -56.8: another floor of the house), and 2D 1.9 with bot_z 7.4 against auctioneer_z 505.2. The second is most likely not bad data: Ironforge lies at about z 500 (the 11-18 sim saw the same house with a bot at z 19), so that bot stood under the city (a below-map case); the check below keeps such an auctioneer when its vendors stand at its height. With `AuctionWalk3D`:

- the errand walks to the snapshot's live auctioneer (its own x, y, z) when it stands within 30 yards of the errand's spot (the spot was taken when the errand started, from farther away);
- the stored z is judged first (`auction::JudgeStandZ`): more than 60 yards from the nearest reference within 200 yards of the auctioneer (the snapshot's vendor, food vendor, goods vendor, banker; the errand also a graveyard seen) is bad data: no listing, the errand fails (`errand_failed reason=auctioneer_bad_z`). With a ground height at the auctioneer's x, y, a z 40 yards or more off it would fall back to the 2D reach (`ignore_z`); the module has no such query today (`kProbePoint` exists in the core but has no sender, and a probe would take the walk's place as the last action the snapshot reports), so that branch is not reached and the z is only checked against the references;
- a bot in 2D reach but not in 3D reach for 60 s ends the errand (`errand_failed reason=auction_reach_3d`) instead of standing there (2026-11-20: replaced by the stand points of doc/auction-reach-fix.md, 15 s per point, 8 points);
- `auction_reach` is written once per bot per 10 minutes with the result (`ok_3d`, `failed_3d`, `skip_bad_z`).

**Below-map false positives.** `below_map_recover` fired 5,148 times on 376 of 400 bots (max 33 per bot; z_below 3,881: map 0 1,877, map 1 915, map 2991 1,094; no_route_storm 1,267). The ground it compared with was the z of the single nearest known place within 60 yards (a quest giver or ender, a start anchor, a grind spot made from creature spawns, a hub), at any height: a giver on a tower, a tree platform or a mesa, a grind spot on a hill above a valley, a place on another floor of a city. It was never a height query under the bot. Sample: map 1 pos 10329,848,1133 against 1326 (Teldrassil; not known whether that bot was really under the ground or on a lower level); map 2991 pos 4126,1953,976 with ground 980 was a storm recovery, the bot at the place's own height. Now:

- the ground is the lowest known place within 60 yards, not the nearest;
- the drop must hold for 20 s and five looks in a row (one look every 2 s; the scan of the map's places no longer runs every tick);
- skipped, not acted on: on a transport (`on_transport`), swimming (`swimming`), a walk the core applied in the last 60 s with the bot not 20 yards lower since and not falling (`on_path`: the core applies a `kMoveTo` only with a full navmesh route from where the bot stands), and for the storm a known place within 15 yards of the bot's height or a walk applied after the storm began (`near_ground`, `on_path`). A flight, combat, death or a held bot stay blocked as before;
- `below_map_skip: bot= reason= sign=z_below|no_route_storm map= pos= ground_z= movement_flags=` at most once per bot per 10 minutes, and the same count per reason in the `deaths_top` line (`below_map_recover=N below_map_skip=reason:n,...`); recoveries stay at most once per bot per 10 minutes (`BelowMapCooldownSec`).

Limits: on a map with no navmesh the core applies every walk (a straight line), so `on_path` excuses every bot there; a bot that fell and was never given a walk since is still judged by the known places only. Suggestion for the sim (not a code default): `BelowMapDropYards = 100` for the next run with `BelowMapRecover = 1`; the default stays 60 and `BelowMapRecover` stays 0 on prod.

What the next sim run should count: `below_map_recover` per hour and per bot (was 5,148 on 376 bots) against `below_map_skip` by reason (deaths_top line and the event), and for each recovery whether the bot's `no_route` count fell after it; `auction_reach` by `check=` (was 194 `failed_3d`) and `errand_failed reason=auctioneer_bad_z|auction_reach_3d`, `listed` lines per hour; `quest_walk_blocked reason=level_unsafe` lines (was 9,266) against `level_safe_held=`, and bot 225's pattern (same quest every turn?).

## What the sim read-out should measure

Deaths per bot and `deaths_top` (top10 share, median); repeat deaths at one spot (deaths within 60 yards of each other per bot); `quest_walk_blocked reason=level_unsafe` per hour and per quest; `level_safe_yard` (fitting vs `none_known`), `level_unsafe_escape` and `teleport_refused` counts; `no_route` per bot-hour (and with `BelowMapRecover = 1`, `below_map_recover` per hour and the short-walk share of `no_route`); levels gained per hour.

## Death loop breaker (2026-11-19, `DeathLoopBreaker`, default 0)

The sim arm C (2026-11-19) still had the Raven Hill loop: bots 235 and 66, level 8 to 10 Human hunters, died 260 and 221 times at the Duskwood graveyard (-10606, 294) to one Skeletal Horror (entry 202, level about 24) standing at the spirit healer. Why the level-safe code did not stop it:

- The graveyard swap never applied. The place minimum at Raven Hill is 18, not 23: a level 18-22 spot (-10630, 533) lies 240 yards off, inside the 250 yard radius, and the lowest minimum wins. 18 is not above level 8 plus the wide margin 10, so neither the graveyard nor the corpse counted as too high (no `level_safe_yard` line, `ghost_fallback ... reason=spirit_healer_far` and `ghost_fallback_teleport` without `graveyard=level_safe`), and `LevelSafeEscape` did not fire either (same margin).
- The escape came too late. The ghost fallback put the ghost back at Raven Hill, the spirit healer revived it beside the Horror, and `death_loop_escape` sent its teleport on the revive tick. The Horror had the bot in combat by then and the core refuses a teleport in combat (ModuleBotActions TeleportAction), so the bot never moved: `died ... pos=-10613,297` 5 s after `death_loop_escape ... from=-10606,294`. The escape then waits out its 120 s cooldown (bot 66: revived 16:07:06, dead 16:07:39, no escape line).

The breaker acts while the bot is a ghost, which the killer cannot follow and the core never refuses a teleport for. A solo bot (no group in the game, no dungeon route; a planned party it never joined does not count) that died 3 times to the same creature entry within 150 yards and 30 minutes:

1. gives up its corpse: no corpse run, no reclaim (the ghost fallback with reason `death_loop`);
2. marks the graveyard it appeared at unsafe for its level and below for 2 hours when that graveyard lies within 150 yards of the loop (`stage=yard_unsafe`); `LevelSafeGraveyard` treats an unsafe graveyard as too high for every bot of that level (`level_safe_yard ... reason=graveyard_unsafe`), gives up a corpse beside it as well, and never chooses it;
3. is teleported as a ghost to another graveyard of the same map and side (`stage=yard`): the nearest one seen that fits its level, else the nearest one at least 400 yards from the loop; a ghost that appeared away from the killer revives at its own graveyard (`stage=own_yard`);
4. asks the spirit healer there, but not while a live hostile 3 or more levels above it stands within 40 yards of the ghost: that graveyard is marked too and the next one is judged (`stage=yard_held`);
5. with no graveyard known, waits as a ghost (no corpse run) up to 5 minutes (`stage=wait`, `stage=wait_over`), revives at the spirit healer, and on revival is put at a grind spot for its level at least 400 yards from the loop, out of combat only, with a walk there when the core refuses the teleport (`stage=escape`, `stage=in_combat`, `stage=no_spot`; tried for 60 s). A bot that revived away from the loop logs `stage=revived_away`. The old `death_loop_escape` does not run for a death the breaker took.

Every move stays on the bot's map: graveyards and grind spots are chosen on the map the ghost is on, so a Skyborne bot never leaves map 2991. The teleports need `Bot.Actions.Teleport = 1`, as the ghost fallback already does. With the key off nothing changes (graveyards are remembered only when `LevelSafeGraveyard` or the breaker is on).

Sim read-out for the breaker: deaths per bot at one spot (within 150 yards, same killer), `death_loop_break` counts per stage (`detect` should be followed by `yard` or `own_yard`, rarely `wait`), repeat deaths at the same spot within 5 minutes after a `stage=yard` (a failure), `level_safe_yard reason=graveyard_unsafe` lines, and the `deaths_top` top-10 share (arm C peaked at 23.8%).
