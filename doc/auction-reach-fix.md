# Auction reach fix (2026-11-20, branch `fix/auction-reach-20261120`)

## The problem

- Prod, 24 h: 98 `auction:` lines, almost all `list_refused ... reason=auctioneer_not_found_or_cannot_interact_ class=retry` (the core's "auctioneer not found or cannot interact with it", cut at 40 characters). Prod runs `Auction = 1`, `AuctionResolveNearby = 0`, `AuctionWalk3D = 0`.
- Sim arm C (`AuctionWalk3D = 1`): 194 `auction_reach check=failed_3d`, 0 `list_refused`. Samples: 2D 3.7 yards, 3D 14.2 (bot z -43.1, auctioneer z -56.8), and 2D 1.9 with bot z 7.4, auctioneer z 505.2.

## How the core decides (read only, `Trinity-Forever-Modules`)

- The snapshot's auctioneer (`ModuleBotSnapshot.cpp`, `FillNearestVendor`) is the auctioneer **spawn** nearest to the bot **in 2D**, among spawns whose faction is not hostile to the bot. It is looked up again only after the bot moves 100 yards. Its x, y, z are the spawn's (`creature` table). The height is ignored, so the chosen auctioneer can be on another floor. Phase and game-event spawns are not filtered out.
- A house call (`ModuleActions.cpp`, `AuctionHouseAction`, kList/kListCommodity/kBuyout/kCancel/browse by filter) resolves the reference with `ResolveCreatureRef`. That uses the guid first, then the spawn id (`Bot.Actions.SpawnRefFallback`, default 1, on prod since 2f3261fd76). If nothing is found, the core says "auctioneer not found on the player's map". If a creature is found, it calls `Player::GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_AUCTIONEER)` and refuses with "auctioneer not found or cannot interact with it" in any of these cases:
  - the bot is on a flight (`IsInFlight`);
  - the bot is dead, or the auctioneer is dead;
  - the auctioneer lacks the auctioneer flag or is charmed;
  - the auctioneer's reaction to the bot is unfriendly or worse;
  - the auctioneer is in combat;
  - the auctioneer is not in the bot's phase (`InSamePhase`);
  - the bot is beyond `IsWithinDistInMap(bot, auctioneer.CombatReach + 4)` **in 3D**. Both combat reaches are added on top, so the limit is 1.5 + 4 + 1.5 + 1.5 = **8.5 yards** for a player and a humanoid auctioneer.

## Root causes found

1. **The module's reach was 2D (5 yards); the core's is 3D (about 8.5).** Without `AuctionWalk3D`, the listing visit and the buy browse counted a bot as in reach when it was:
   - on another floor of the house (arm C sample 1: 13.7 yards of height);
   - under the city (sample 2);
   - on a taxi flight over the house;
   - walking past it.

   Every call from those places was refused with "cannot interact". This is the prod refusal: the creature is found (spawn-id fallback), and the interaction check fails.
2. **The errand walked into the auctioneer's own spot.** The auctioneer stands behind its counter, off the navmesh. The core's `kMoveTo` answers `kNoRoute` (`HasFullRouteTo`: path incomplete) and still moves the bot to the point the navmesh snaps it to. That point is often the floor above or below. A short walk with `kNoRoute` failed the errand at once (`errand_failed reason=no_route`). With `AuctionWalk3D`, the bot ended in 2D reach on the wrong floor, and after 60 s the errand failed (`auction_reach_3d`). The pump's own `auction_reach` line then said `failed_3d`, which explains the 194 arm C lines and the 0 listings.
3. **The live creature's position was never used.** The reach, the walk and the log all used the spawn's x, y, z, even when the bot could see the live auctioneer in its nearby units (`ResolveNearby` used only its reference, and only with `AuctionResolveNearby = 1`).
4. **Unclear lines.** The reason token is cut at 40 characters, and nothing said whether the bot was on the wrong floor, moving, on a flight, or really in reach.

**z 505 is not bad data.** Ironforge's auction house is at about z 501 to 505, and the snapshot's vendors near it agree (see doc/level-safe.md, arm C notes). The bot at z 7.4 was under the city. No data file holds auctioneer positions: they come from the core's creature spawns, and the bots repo has none (`data/` has no auctioneer table). So **no data entry was changed**. A truly wrong stored z is still caught by `auction::JudgeStandZ` (`skip_bad_z`, unchanged).

Not fixed here, needs the core: the snapshot's 2D-nearest pick (it could prefer an auctioneer on the bot's floor and skip spawns in another phase), and a ground-height query (`kProbePoint` has no sender) for a true "nearest walkable point".

## The fix

### Plain fix, no key (every config, prod included)

- `auction::JudgeCall` is the call gate. A listing, a goods listing or a buy browse is made only when all of these hold:
  - the bot is within 5 yards in 2D;
  - the bot is within the core's **8.5 yards in 3D**;
  - the bot is not on a flight;
  - the bot is not moving.

  It measures from the **live auctioneer** (`auction::FindLive`: the same type and entry within 8 yards of the spawn, in the bot's nearby units) when the bot sees it, else from the spawn. The gate only removes calls the core would refuse. A bot within 5 yards in 2D and 8.5 yards in 3D, standing still, is gated exactly as before.
- `list_refused` and `buy_failed` keep their text and end with ` code=<code> cause=<cause>`:
  - `code` is one of: `cannot_interact`, `auctioneer_missing`, `no_money`, `item_gone`, `item_mismatch`, `no_house`, `not_listed`, `auction_gone`, `own_auction`, `no_player`, `other`.
  - `cause` (only for `cannot_interact` and `auctioneer_missing`) is one of: `far_height`, `far_2d`, `moving`, `in_flight`, `not_seen` (the bot did not see the live auctioneer: another phase, or not loaded), or `in_reach_other` (in reach as far as the module can tell: phase, faction standing, the auctioneer in combat, or the bot moved between the snapshot and the call).
- "auctioneer has no auction house" is now `class=retry`. It is a fault of the place, not of the item, so the item is no longer dropped.
- The pump's `auction_reach` line gains `live=0|1` and `gate=<JudgeCall>`, and is measured from the live auctioneer when one is seen.

### Behind `AuctionWalk3D` (default 0; sim only so far)

- The errand's anchor is the live auctioneer's own x, y, z when the bot sees it. Its height is real, so `JudgeStandZ` is not applied. Otherwise the anchor is the snapshot's spawn, judged as before.
- The walk goes to a **stand point**, not to the anchor. There are 8 points on a ring of 2.5 yards around the anchor, at its height (`auction::StandPoint`). The first is on the side the bot came from, then ±45°, ±90°, ±135°, and 180° last. Each point is within 4.5 yards in 3D of the anchor.
- A `kNoRoute` answer for the stand point no longer fails the errand. The walk moves on to the next point, and so does a bot that has stood in 2D reach without the 3D reach for 15 s (`auction::JudgeStandTry`). The pause for the refused place is cleared, so the next point is sent at once. After the 8th point the errand fails: `reason=auction_stand_no_route` for no route, `reason=auction_reach_3d` for the wrong floor.
- Arrival is unchanged: within 4.5 yards in 3D of the anchor, then a 25 s dwell for the listing visit.

## Tests

- `tests/auction_reach_test.cpp` (new) covers:
  - the gate against the arm C samples and the 8.5-yard edge, with in-flight and moving bots;
  - the codes and causes, and the prod line;
  - `FindLive` and its z;
  - the ring (8 distinct points, each in reach, the bot's side first, wrapping);
  - `JudgeStandTry`;
  - the policy errand: it walks to a stand point; `kNoRoute` moves through all 8 points before failing; without the key `no_route` fails as before; the live auctioneer's height is used.
- `tests/arm_c_fixes_test.cpp` was updated: the walk goes to the ring, the wrong floor fails after 8 points, and there are 7 retry lines.

## How to verify on the sim

Config for an arm: `Auction = 1`, `Errands = 1`, `AuctionWalk3D = 1`, `AuctionLogReach = 1`. Compare with an arm that has `AuctionWalk3D = 0`: that arm still gets the plain fix.

Grep the journal from the run start:

```
journalctl -u <sim unit> --since "<start>" | grep -c "forever-bots: auction: bot .* listed"        # should rise from 0
journalctl ... | grep "list_refused" | grep -o "code=[a-z_]* cause=[a-z_-]*" | sort | uniq -c       # what is left, and why
journalctl ... | grep -o "auction_reach: .*check=[a-z_0-9]*" | grep -o "check=.*" | sort | uniq -c  # ok_3d should dominate where=errand
journalctl ... | grep -o "auction_reach: .* gate=[a-z_0-9]*" | grep -o "gate=.*" | sort | uniq -c   # far_height / moving / in_flight: calls not made
journalctl ... | grep "auction_stand_retry" | grep -o "why=[a-z_0-9]*" | sort | uniq -c            # no_route vs near_2d_only
journalctl ... | grep -o "errand_failed: bot=[0-9]* reason=[a-z_0-9]*" | grep -o "reason=.*" | sort | uniq -c
journalctl ... | grep -c "errand_done: bot=[0-9]* auction"
```

Expect:

- `listed` lines > 0;
- `list_refused` rare, mostly `cause=in_reach_other` or `not_seen`, with no `far_height`, `moving` or `in_flight` (those calls are no longer made);
- `auction_reach ... where=errand check=ok_3d live=1` for most errands;
- `errand_failed reason=auction_reach_3d` or `auction_stand_no_route` far below arm C's 194 `failed_3d`.

On prod (after a deploy with the key still 0), `list_refused ... cause=` shows what remains. `auctioneer_not_found_or_cannot_interact_` with `cause=far_height`, `moving` or `in_flight` must be absent.
