# Route filter (RouteFilter, off by default)

Status: built and unit-tested (`src/route_filter.*`, `tests/route_filter_test.cpp`). Nothing here has run on the sim. With `RouteFilter = 0` no bot changes (the explore code takes one candidate, the grind code does not look at the memory).

## Problem
The core answers a walk it cannot route with `BotActionStatus::kNoRoute` (journal: `event no_route`, with `cause=`). The module only ended the walk; the next pick failed the same way. Sim 3 h: 10,119 `no_route` events to 8,690 distinct destinations; prod 90 min: 20,246 events (about 1 grind pick in 4 ends as a failed walk).

## What the states do now
- Grind (`bot_policy_turn_town.cpp`): a spot from `GrindSpotsFor` (level, side, nearest first, minus the two just visited and the shared 3-bot/10-min/1-h block of `GrindBlocklist`), jitter of up to 6 yards. A refusal ends the walk; a walk of 150 yards or more that gets `no route` is chained (`NotePartialLeg`) and only a chain that makes no progress counts as a refusal.
- Explore: a point 50 to 100 yards from the bot at the bot's own height in a golden-angle direction; a refused point is retried at half and quarter radius, then the bot waits 10 s and does it again.

## What the filter does
1. Refused grind spot: remembered per bot (spot index, TTL `RouteFilterTtlSec` 300 s, doubling per repeat to `RouteFilterMaxTtlSec`) and shared per spot and start cell (`RouteFilterStartCellYards`, 200) after `RouteFilterSharedBots` (3) different bots were refused. `GrindSpotsForBot` leaves those spots out; with none left the bot explores on foot for a minute as before and looks again.
2. Refused explore point: its cell (`RouteFilterCellYards`, 8) is remembered the same way; a new walk looks at up to `RouteFilterExploreTries` (6) directions of its own sequence and takes the first free one. The pause between explore walks doubles every 3 refusals in a row up to `RouteFilterExploreBackoffMaxSec` (600), reset by an arrival.
3. Arrival: a refusal within `RouteFilterArrivedYards` (8) of the destination of a grind or explore walk is an arrival. The place is marked reached (its entries are erased), the walk ends as an arrival, nothing is reported to `GrindBlocklist` (before, three bots arriving at one spot blocked it for everybody for an hour).
4. A bot forgets its own entries when its level or map changes.
5. A walk that began as exploring and still holds `Wandering` when the grind timer expires is ended when its stale `GrindTarget` is a refused spot (the two states share `Wandering`).

Events: `route_filter_skip` (grind spots left out or explore skipped, at most one per bot a minute), `route_filter_shared` (a key became shared), `route_filter_arrived`.

## Keys
See the README table and `config/modules.d/forever-bots.conf.example` (`RouteFilter*`, nine keys, all explicit).

## Not covered
Quest, vendor, hub and gather walks, kite and follow moves are not filtered; they have their own pause (`NoRouteUntilMs`, `MoveGate`). On prod they are most of the `no_route` events (see the ops doc `docs/route-filter-20261117.md`).
