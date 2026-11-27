# TravelGraph: detours along known ways

Key `TravelGraph` (default 0). Code: `src/travel_graph.{h,cpp}`, `src/bot_policy_travel.cpp`.

## Why

The core's navmesh path is capped, and a long walk is chained leg by leg only while each straight attempt brings the bot nearer. Where the way goes
around a mountain or through a pass the chain ends, the walk is refused and an errand runs into its time limit (sim 2026-11-26: more than half of the
Auction House trips; prod 2026-11-27: 6k ended chains a day).

## What it does

- Every look at a living bot adds a step to a graph of 40 yard cells per map. Steps longer than 100 yards or faster than 20 yards per second (flights,
  boats, teleports) are dropped. A step that climbs or drops more than 12 yards is kept one way only.
- When a chain ends, the graph is asked for the next leg (about 200 yards) towards the destination. With one, the walk is not refused: it is sent again to
  the leg, and the destination is tried again from there.
- A detour stops when the leg itself has no route, when the known way is not at least 40 yards shorter than at the leg before, after 60 legs, or when
  no known cell lies within 60 yards of the bot. The walk is then refused as before.
- The graph is held in memory (at most 200,000 cells) and is empty after a restart.

## Events

`travel_detour: bot= map= dest= dist= result=leg|no_way|no_progress ...` and `travel_detour: bot= result=leg_failed`.
