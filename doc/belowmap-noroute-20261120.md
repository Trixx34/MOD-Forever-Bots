# Below-map false positives and the no-route backoff (2026-11-20)

Branch `fix/belowmap-noroute-20261120` (on `fix/arm-split-balance-20261120`, 3eba59a). Code: `src/below_map.*`, `src/no_route_backoff.*`,
`src/bot_policy_level_safe.cpp` (TurnBelowMap), `src/bot_policy_result.cpp` (no_route line, refusals), `src/bot_policy_turn_town.cpp` (TurnSendMove),
`src/route_triage.h` (caller groups). Tests: `tests/belowmap_noroute_test.cpp` (new), `tests/arm_c_fixes_test.cpp`, `tests/level_safe_test.cpp`.

## The problems

- `below_map_recover` fired 5,148 times on 376 of 400 bots in arm C (max 33 per bot; z_below 3,881, no_route_storm 1,267) and 2,655 times in one arm of
  the 11-20 strata run. Too common to be real falls.
- `no_route`: about 29 per bot-hour on every bot (91,234 in 7.9 h), unchanged by the earlier fixes.

## Causes found (from the code; not measured)

### How the ground is found

There is **no height query** anywhere: not in the module, not in the snapshot (SDK layout 24 has no ground z). The "ground" is the set of known places of
the map (`off_map::KnownWorld`: quest givers, enders and anchors, grind spots, hubs) within 60 yards in 2D. Arm C took the nearest of them, the arm C fix
(4c5eea2) the lowest. Both are an estimate: a bot in a valley, a cave or on the lower floor of a city with every known place on a hill or upper floor
above it looks "below the map".

### Why it fired so often

1. **The core walks a bot in a straight 3D line when the navmesh has no path.** `ModuleBotActions` kMoveTo answers `kNoRoute` but still calls
   `MovePoint`; with `PATHFIND_NOPATH` (start or end off the mesh), `MoveSplineInit::MoveTo` builds a two-point spline from the bot to the destination.
   Server-side bots have no client to keep them on the terrain, so a bot crossing a hill this way is *inside* the hill for the whole walk, and z_below
   fires on a bot that is walking. The walk ends at the destination's height, so the bot recovers by itself. This ties problem A to problem B: each
   refused walk can be such a straight line.
2. **The storm counted retries and explore walks.** Eight short refused walks in 5 minutes made a storm, but the same refused point retried counted each
   time, and explore walks (a point 50 to 100 yards away *at the bot's own height*, which the core often refuses on slopes) counted too. Neither says
   anything about the bot being under the ground.
3. **One place is no floor.** A single giver on a tower or a tree platform within 60 yards was enough to judge the drop.
4. Flying (movement flag bit 1) and short falls were not excused for z_below.

### Why no_route did not drop

Two older gates exist: the policy's `NoRouteUntilMs` and the pump's `move_gate`. Both remember **one** place per bot, and the pause (5 s, doubling up to
about 2.7 min) only grows while the *same* place is refused again in a row. A bot that goes round two or three places it cannot reach starts at 5 s for
each of them every time. Also any `state.Last.reset()` makes the policy send the same walk again.

Callers of a point walk (`BotTurn::moveWhy`), grouped as the new `caller=` field does:

| caller | move reasons | why it is refused (code reading) |
|---|---|---|
| grind | `state.GrindTarget` | the spot's point is jittered in x/y but keeps the spot's z (a creature-spawn average): on a slope the end is more than 7 yards off the mesh (`PATHFIND_FARFROMPOLY` + `INCOMPLETE`); spots in rock or water |
| explore | `state.ExploreTarget` | the point keeps the bot's own z by design ("the core often has no route to it"); refusals are retried nearer twice |
| hub | `hub`, `list[...]` | hubs on another floor or in rock; after 2 failures the bot explores |
| quest | `quest_walk`, `quest_hunt` | giver z on another floor, hunt areas off the mesh; the planner blocks a start for 30 min or a quest after 3 refusals |
| errand / vendor | `order.Dest`, `vendor`, `rep_vendor`, `repair_wait`, `gather_walk`, `point_walk` | NPC z on another floor (auction house: 13.6 yards of z) |
| goto / event | `state.GotoPoint`, `visit.Dest`, `event_*` | chat orders, death-loop spots, world events |
| combat / dungeon / death | kite, flee, approach, tank spots, waypoints, corpse walks | chases and route points; these have their own handling |

The biggest share by this reading is the idle loop (grind, explore, hub): it runs all session on every bot, picks new points all the time, and two of the
three keep a z that is not the ground's. That would fit "spread evenly over all bots". This is a reading of the code; the new fields measure it.

## Fixes

### Below-map (all behind `BelowMapRecover`, default 0)

- **Closest floor.** `below_map::FindGround` takes every known place within 60 yards and picks the one whose z is closest to the bot's (the lower one on a
  tie): a bot on the lower floor of a city stands on that floor. When all lie above, it is the lowest of them (never the highest). The teleport goes there.
- **The bot must stand still.** z_below counts only checks of a bot that is not moving (`BelowMapRequireStill`), not flying, not falling (a fall is
  excused for 30 s; a longer fall is a fall into the void and counts), with at least `BelowMapMinPlaces` known places near, on a map the known world
  covers (10 places or more: instances and battlegrounds are never judged). Any of these resets the count: the drop must persist again.
- **Ten checks in a row** (`BelowMapChecks`, about 20 s at one check every 2 s) and 20 s.
- **Storm:** a refused short walk counts only when its destination lies `BelowMapStormRiseYards` or more above the bot, and each destination (12 yards)
  once (`BelowMapStormDistinct`).
- Unchanged: on a transport, swimming, on_path (a walk applied recently), near_ground (storm), blocked when dead, fighting, on a flight or held; the
  10-minute cooldown.

### No-route backoff (`NoRouteBackoff`, default 0)

Per bot, up to 16 refused places (12 yards wide, per map). A refused point walk arms the place: not sent before `NoRouteBackoffSec` (60 s), then 120,
240, ... up to 30 minutes while it keeps being refused. An applied walk there clears it; a place not refused for an hour is forgotten. A held walk is not
sent (no core call: no no_route line, no straight-line walk) and ends as a refusal would (`WalkBlockedAtMs`), so the grind, hub, explore, gather and
craft walks pick something else. Not for walks to a unit, in combat, while dead (corpse walks) or on a dungeon route; arrivals (under 6 yards) do not arm.

## Keys

| Key | Default | Range | Meaning |
|---|---|---|---|
| `BelowMapRecover` | 0 | | unchanged; keep 0 on prod |
| `BelowMapChecks` | 10 | 1 to 100 | checks in a row (one every 2 s) the drop must hold |
| `BelowMapMinPlaces` | 2 | 1 to 20 | known places within 60 yards needed to judge z_below |
| `BelowMapRequireStill` | 1 | | a moving bot is not judged |
| `BelowMapStormRiseYards` | 30 | 0 to 500 | the storm counts refused walks to places this much above the bot (0 = any) |
| `BelowMapStormDistinct` | 1 | | each refused destination counts once |
| `NoRouteBackoff` | 0 | | the per-(bot, destination) backoff |
| `NoRouteBackoffSec` | 60 | 5 to 3600 | the first wait; doubles per refusal of the place, at most 30 min |

Every default is the safe side (fewer teleports; the backoff off). All are in `config/modules.d/forever-bots.conf.example` and work as
`ArmBOverrides` items. Per the add-all-keys rule, add them to each env's drop-in (prod: `BelowMapRecover = 0`, `NoRouteBackoff = 0`).

## Events

- `below_map_recover: bot= reason=z_below|no_route_storm map= pos=x,y,z ground_z= z= drop= places= floors= low_z= high_z= checks= moving= to=x,y,z
  movement_flags=` (ground_z is the closest floor; drop = ground_z - z; floors = distinct heights in 10-yard bands among the places).
- `below_map_skip: bot= reason=on_transport|swimming|on_path|near_ground|moving|flying|falling|few_places sign=z_below|no_route_storm` with the same
  fields; once per bot per 10 minutes, counted in the hourly `deaths_top` line (`below_map_skip=reason:n,...`).
- `no_route:` now also carries `why=` (the move reason), `caller=` (grind, explore, hub, quest, errand, vendor, goto, event, combat, dungeon, death,
  other), `dist=` (2D to the destination), `dz=` (destination z minus bot z) and `map=`. Still at most once per bot per 10 s.
- `no_route_backoff: bot= stage=armed why= caller= map= dest=x,y,z dist= fails= wait_s= places=` on every refusal that arms a place;
  `no_route_backoff: bot= stage=held why= caller= map= dest= fails= wait_s=` once per wait when a walk is held back.

## How to verify on the sim

No test push during an overnight run (push freeze). In the next window, as an A/B with `ArmSplitMode = strata`:

1. Calibration first is already done for strata; then `ArmBOverrides = "NoRouteBackoff=1"`, both arms with `BelowMapRecover = 1` (the new below-map
   defaults apply to both arms).
2. Count per arm and per bot-hour: `no_route` (was 29), its split by `caller=` and by `dz` (|dz| > 7 is the off-mesh z), `no_route_backoff stage=armed`
   and `stage=held`, `walk_chained`, `grind_spot_blocked`, levels and deaths per bot-hour (the backoff must not cost levels: arm B within the A=B gap).
3. Below-map: `below_map_recover` per hour and per bot (was 5,148 on 376 bots, and 2,655 in one strata arm) against `below_map_skip` by reason;
   expect `moving` to dominate the skips if cause 1 is right. For each recovery read `places=`, `floors=`, `drop=`: recoveries with floors >= 2 and a
   drop near the floor gap are suspects; check a few positions by hand (`.gps` on a GM at the spot).
4. Pass: `below_map_recover` down by an order of magnitude with no rise of bots stuck (same position for an hour) and no rise in deaths; `no_route` per
   bot-hour down in arm B with levels per bot-hour not worse.

## Not verified

- Nothing here ran on the sim; the caller breakdown is from reading the code, the new `caller=` field is what measures it.
- Whether the Teldrassil sample (map 1, 10329,848,1133 against 1326) was a real fall: Shadowglen's ground is about 1326, so it may well have been.
- The core's straight-line walk under the terrain is read from the core source (`MoveSplineInit::MoveTo`, `PathGenerator::BuildPolyPath`), not seen
  in a trace. The real cure for it is in the core (a z-corrected destination or no move on `PATHFIND_NOPATH`), which this branch does not touch.
