# Path variety: bots stop sharing one leveling path

Status: built and unit-tested (`src/path_variety.*`, `tests/path_variety_test.cpp`). **Nothing here has run on the sim.** Off by default (`PathVariety = 0`); with it off no bot changes.
Measurements and the sim plan: `docs/path-variety-20261117.md` in forever-ops (branch `docs/path-variety`).

## The problem (prod 2026-11-17, 800 bots, levels 3 to 15)

Same race and class means the same path: the same spot (14.6 percent of all same race+class pairs share their nearest grind spot; one spot held 72 bots, 5 spots 24 to 40
each), the same zone (zone 40 held 86 of the 268 bots at levels 10 to 12; the map 2991 start zone 119 of the 474 at levels 13 to 15), the same quest order, the same vendor at
the same moment. Crowds mean fights shared by many bots, deaths in crowds and `no_route` bursts to the same spot.

## What decides the path today

* **Spot**: `GrindSpotsFor` (`grind_data.cpp`) lists the level-fit spots of the bot's map and side by `distance / (1 + 0.15 ln(1 + weight))`, minus the last two visited and the blocked ones.
  `turn_town.cpp` takes one of the nearest `kGrindPickAmong = 4` by a draw (RandomPct > 0) and stands within 6 yards of it. So every bot of a place chooses among the same
  4 spots, and a bot that stays picks the nearest ones again: bots of a level share a handful of spots.
* **Zone**: none. The list holds only spots of the bot's own map, ranked by distance, so a bot never leaves the zone it stands in until the level band of every spot nearby ends.
  The spot file has no zone column (`creature.zoneId` is 0 in this fork).
* **Quests**: `QuestPlanner::Next` (`quest_planner.cpp`) takes the first eligible quest in file order of the nearest start; the hunting area index starts at 0; starts are ranked by
  distance. All bots of a start do the same quests in the same order and walk to the same area first.
* **Trips**: the vendor trip is due at login (`NextServiceMs = 0`), then every `ServiceEveryMs`; the trainer is visited the moment a level is gained.
* **Personalities** (`personality.h`) vary rest, idle dwell, pulls and chat; none of them varies where a bot goes.

## Design

All draws are `bot_random::Draw(RandomSeed, guid, salt, counter)`: stable per bot, nothing stored, tests can pin them. Keys: `Module.forever-bots.` (all in the example conf and README).

| Lever | What | Bound | Keys |
|---|---|---|---|
| Home zone | the level-fit spots are grouped by zone; the bot has a home zone chosen by a seeded draw weighted by fit (more spots), distance and a soft cap on bots per zone `1 / (1 + (bots / cap)^2)`; its own zone counts double (stay) | zones farther than `PathZoneMaxWalkYards` are not chosen while another is in reach | `PathZoneSpread`, `PathZoneSoftCap`, `PathZoneMaxWalkYards` |
| Migration | the choice is looked at again only at a level step (every `PathZoneReviewLevels` levels, at a per-bot phase), when the home zone has no spot for the level, or per spot pick with `PathZoneMigratePct` percent when the zone holds more than the cap. No teleport: the move is a walk of the normal grind walk, one bot at a time | a bot that changes zone walks at most `PathZoneMaxWalkYards` | `PathZoneReviewLevels`, `PathZoneMigratePct` |
| Spot | inside the zone: spots farther than `PathSpotReachYards` beyond the nearest are dropped, full spots (`PathSpotMax` bots) are skipped while another is free, the rest sorted by `distance * (1 + taste) + PathCrowdYards * bots there`; taste is a stable +-`PathSpotAffinityPct` per bot and spot | extra walk per pick at most `PathSpotReachYards` | `PathSpotMax`, `PathCrowdYards`, `PathSpotAffinityPct`, `PathSpotReachYards` |
| Quest mix | the next quest is the first of a per-bot order among the first `PathQuestOrderWindow` eligible ones; a per-bot appetite makes the bot leave up to `PathQuestSkipMaxPct` percent of the free-standing quests (not a `prev` of another quest) and grind; the hunting area starts at a per-bot index; the ranking of nearby quest starts is wobbled +-`PathStartOrderPct` | chain heads are never skipped; the window is 8 at most; skipped quests are grind time, not idle time | `PathQuestOrderWindow`, `PathQuestSkipMaxPct`, `PathStartOrderPct` |
| Offsets | the first grind walk and the first vendor trip after login wait a per-bot 0 to `PathStartOffsetMaxSec` | 15 min at most | `PathStartOffsetMaxSec` |
| Trips | vendor interval +-`PathTripSpreadPct`; trainer visit after a level-up waits 0 to `PathTrainDelayMaxSec` | 60 percent, 10 min | `PathTripSpreadPct`, `PathTrainDelayMaxSec` |

Counts come from two `path_variety::Crowd` tables in the policy: bots by home zone and bots by the spot they were last sent to. After a restart they start empty and fill within the first
spot picks; a bot that logged off keeps its entry until its next pick (the table is bounded by the number of bots).

Zones: the spot file has no zone, so `PathZoneFile` (`data/grind_zones.txt`, made by `tools/make_zone_samples.py` from characters' `map zone x y`) gives sample points; a spot takes the zone of the nearest
sample on its map within 1500 yards. Without the file a spot falls into a 1500-yard square region of its map, so the lever works but the zones are not the game's. The shipped file has 335 samples
from the prod bots and sim characters (31 zones; Elwynn, Westfall, Duskwood have only a few). More samples sharpen the borders; regenerate with the tool.

### What it cannot do

* A bot never changes map (no walking between maps; the map 2991 start zone has one zone). The spread is over the zones of the bot's map and side: map 0 for Alliance Eastern Kingdoms
  and Horde undead, map 1 for Kalimdor races. Races in separate start maps stay in them until the content for 2991 grows (zones: 1 on prod).
* Quests: the zone lever acts on grinding; quests follow the nearest quest start, so a bot that moved zone takes the quests there, but no quest data decides the zone.
* Detours (professions, cities) are not part of this lane: gathering and auction trips belong to the gather lane. Trips are spread, not added.
* The death-loop and death-area escapes call `GrindSpotsForBot` and teleport to its first spot away from the corpse; with variety on that spot can lie in the bot's (possibly new) home zone.

## Overlap with other lanes

Touches `bot_policy_features.cpp` (`GrindSpotsForBot`, new `PathVarySpots`), `bot_policy_turn_town.cpp` (one line at the grind pick, vendor interval, train delay),
`bot_policy.cpp` (`WanderIdleFor`), `quest_planner.*`, settings, README, example conf, CMakeLists (one source and one test added). Likely merge conflicts only in CMakeLists and README rows.
The death-fix lane changes pull and rest rules, the gather lane `gather_nodes` and the CMake list, the gear lane gear files; none edits these functions.

## Tests

`path_variety_test`: off changes nothing; every draw stable and within its key; each key 0 switches its lever off; spots ordered by crowd and distance, reach limit, full spots dropped only while another
is left; the soft cap moves the choice away from a crowded zone; reviews happen about once per `PathZoneReviewLevels` levels and not at all in an uncrowded zone with the same level; the crowd table;
zone samples and regions; a synthetic world (600 bots clustered at one start, 6 zones of 12 spots) off versus on (numbers printed by the test); the planner: the first quest varies over bots,
is stable, and the file order is unchanged without variety.
