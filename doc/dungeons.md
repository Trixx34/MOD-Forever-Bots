# Dungeon runs (M9)

See also `doc/dungeon-events.md`: the one events table for all dungeons (doors, levers, the Deadmines cannon), the `tank lead` chat command that runs a dungeon with the bots of a
player's group, and the routes planned over the navmesh (the section below describes how the older generated routes were made).

Status: the data file, its parser, the config merge and the dungeon-clear rule are built and unit-tested (`src/dungeon_data.*`,
`tests/dungeon_test.cpp`). **Nothing here has been run on the sim.** The sample route for Ragefire Chasm is hand-written and approximate.

## What it is, and what it reuses

A dungeon run is a **scenario** (README, "Scenarios"; `src/scenario.*`) whose settings come from a data file instead of a dozen config keys. The
scenario director and the party policy already do the work; this slice adds the data file and one rule:

| Need | Where it already lives |
|---|---|
| Spawn tank, healer and three dps, dress them, form the party | scenario phases Waiting, Dressing, Grouping |
| Enter the instance | scenario phase Entering: the core teleports the leader to `entry`, then the members (needs `Bot.Actions.Teleport = 1`). Walking to the portal is not implemented |
| The tank leads and pulls pack by pack | party leader walks the route (`PolicyOptions::Route`); it pulls only when every living member has 80% health, mana users 60% mana, and all are within 30 yards (it sits to recover, at most two minutes) |
| The healer prioritises the tank | a healer class heals the party member below 60% health; the tank is who loses health first. There is no explicit "tank first" ordering when several members are hurt (the lowest-health member is healed) |
| Dps assist the tank's target | members do not pull and join a fight 2.5 s after the tank, on the leader's target |
| Loot | the bot loots creatures it fought (core loot rules) |
| Wipe and corpse run | dead bots release, run back through `portal` to the instance, reclaim or accept a resurrect; `wipe_recovery` keeps the run going while a bot may come back |
| Give up | `duration` (timeout), `min_alive` (understrength), `leader_dead_abandon`, and the new `bosses_missing` |

New in this slice: **`ScenarioRequireBosses`**. With it on, the run is `complete` when every listed boss has been seen dead (a creature of that
entry with no health in some bot's view), not when the leader touches the last waypoint. If the leader reaches the last waypoint and bosses are still
alive, the run gets 120 s more and then ends as `bosses_missing`. A wipe, timeout and the other endings are unchanged and take effect first.
The result line shows `bosses=2/4 killed=...`.

## Using it

    Module.forever-bots.DungeonFile = data/dungeons.txt
    Module.forever-bots.Dungeon = rfc

`Dungeon` and `DungeonFile` must both be set. The module reads the file when the config arrives and adds the scenario keys the config does not set
itself (`Scenario`, `ScenarioRoster`, `ScenarioLevel` = the dungeon's top level, `ScenarioStaging`, `ScenarioPortal`, `ScenarioEntry`,
`ScenarioDurationSec`, `ScenarioWipeRecovery*`, `ScenarioMinAlive`, `ScenarioLeaderDeadAbandonSec`, `ScenarioBosses`, `ScenarioRequireBosses`,
`ScenarioRoute<n>`). Any key you set yourself wins; setting your own `ScenarioRoute<n>` replaces the whole file route. A problem (unknown dungeon,
unreadable file) is a line on the module's stderr and no dungeon run. Then the usual scenario rules apply (one result line in `ScenarioResultFile`,
`MaxBots >= 5`, the ramp and saved bots unused, `Bot.Actions.Teleport = 1` in the core). A config reload does not restart a run.

## File format

Line based. `#` starts a comment. A dungeon is a block from `dungeon <id>` to `end`:

| Line | Meaning |
|---|---|
| `dungeon <id>` | start of a block; `<id>` is what `Dungeon =` picks |
| `name <text>` | display name (optional) |
| `levels <min> <max>` | level range; the bots are made at `max` (optional, default 1 to 60) |
| `roster <race:class,...>` | one to five bots, race:class ids; **the party leader is the first warrior, so put the tank first**; the healer is any priest, paladin, shaman or druid |
| `staging <map> <x> <y> <z>` | where the bots log in (a city, so nothing attacks them) (optional) |
| `portal <map> <x> <y> <z>` | the instance door on the outside map; a ghost released outside walks there for a corpse run (optional; without it a bot released outside cannot return) |
| `entry <map> <x> <y> <z>` | **required**: the inside map and where the party is teleported to |
| `duration <sec>` | run time limit (default 1800) |
| `wipe_recovery <sec>` | how long an all-down party may take to get a bot back; 0 or absent: a wipe ends the run at once |
| `min_alive <n>` | give up (`understrength`) when fewer are alive for a while (optional) |
| `leader_dead_abandon <sec>` | give up when the leader stays down this long (optional) |
| `go <x> <y> <z>` | a route waypoint, in order |
| `boss <entry> <x> <y> <z> <name>` | a waypoint that is also a boss; `<entry>` is the creature template id; commas in the name are replaced by spaces |
| `end` | end of the block |

A block with a bad number, no `entry`, no `roster`, or no waypoint is dropped with a warning; the other blocks stay usable. The route is what the
leader walks: put a waypoint at every corridor turn and at the position of every pack you want pulled, because the party fights what it meets on
the way and the leader holds each pull until the party has recovered. The party does not search for a pack off the path.

## Making a route from the world database

The module needs the creature spawns of the dungeon's map. Nothing in this repository reads a database; `tools/make_dungeon.py` turns an export
into a block. The export is a tab-separated file `entry x y z name kind` (`kind` is `boss` or `pack`), in the order the party should meet them.
Adapt the column names to the fork (`SHOW COLUMNS FROM creature;` first, as in `doc/grind-spots.md`):

    SELECT c.id, ROUND(c.position_x,1), ROUND(c.position_y,1), ROUND(c.position_z,1), ct.name,
           IF(ct.`rank` = 3 OR c.id IN (11517,11520,11518,11519), 'boss', 'pack')
    FROM creature c JOIN creature_template ct ON ct.entry = c.id
    WHERE c.map = 389 AND ct.faction NOT IN (35)   -- hostile only; map 389 = Ragefire Chasm
    ORDER BY ...;

Order matters and is the hard part. Spawn data has no order, so sort by hand or by distance from the entry along the dungeon (or record the real path
by walking it with a GM character and copy the coordinates into `go` lines). Then:

    python3 -I tools/make_dungeon.py --id rfc --name "Ragefire Chasm" --levels 13 18 --roster 2:1,5:5,8:8,2:9,5:4 \
        --entry 389 3.81 -14.82 -17.84 --portal 1 1811 -4410 -18 --staging 1 1629 -4373 31 spawns.tsv >> data/dungeons.txt

`--merge 25` folds packs closer than 25 yards to the previous waypoint. Straight lines between waypoints are not path-checked: the core moves bots
along its own navigation mesh, so a waypoint in rock or on the wrong floor is a stuck leader (it then runs into the timeout).

## The data file

`data/dungeons.txt` is made by `tools/make_dungeons_from_db.py` from the world database (`dump_to_sqlite.py` reads it from a `mysqldump` file, see
`doc/world-quests.md`): Ragefire Chasm (`rfc`), Wailing Caverns (`wc`), Deadmines (`dm`), Shadowfang Keep (`sfk`), Stormwind Stockades (`stockade`),
Blackfathom Deeps (`bfd`), Gnomeregan (`gnomer`), Razorfen Kraul (`rfk`) and Razorfen Downs (`rfd`). What is real: the entrance (where the party is teleported)
and the exit (where a ghost walks to, `portal`) are the teleport targets of the database ("Classic: <name> - Entrance" and "- Exit"), the hostile spawns,
and the bosses (found by name; a boss with no spawn on the map is left out and the tool says so: Mutanus, Baron Aquanis, Grubbis and Tuten'kash are summoned
or event spawns, so those dungeons are cleared without them). What is a guess: **the order of every route** (the pack or boss nearest to where the party
stands next, starting at the entrance, with a penalty for another floor), so a route may walk past a door into a dead end and back, and the levels (the usual
vanilla ranges) and the roster (an orc warrior, undead priest, troll mage, orc warlock and undead rogue from Orgrimmar for all of them). Zul'Farrak is left out
(most of its bosses are event spawns) and the Temple of Atal'Hakkar has no "Classic:" door in this database. None of it has run on the sim: start with `rfc`,
whose route has 42 waypoints, and edit the `go` lines that lead the party wrong.

The hand-written sample that was here before (Ragefire Chasm) had the entrance at 3.81 -14.82 -17.84; the database says 0.80 -8.23 -15.53, and the boss positions were off.

### The old sample note

`data/dungeons.txt` held Ragefire Chasm (`rfc`). The entry (map 389, 3.81 -14.82 -17.84), the door (map 1, 1811 -4410 -18) and the four boss entries
(Oggleflint 11517, Taragaman the Hungerer 11520, Jergosh the Invoker 11518, Bazzalan 11519) are the vanilla values. **The waypoints and the boss
positions are approximate and were not read from a database or walked.** Regenerate the route before trusting it.

## Not verified, not built

- Nothing has run on the sim: entry by teleport, the route walk through an instance, pulls, heals, loot, wipe recovery in a dungeon, the corpse run
  through the door are all covered by unit tests with hand-built snapshots only.
- A boss counts as dead when a bot sees its corpse (health 0). A boss killed out of every bot's sight, or despawned, is missed.
- Walking from the city to the portal (instead of teleporting to the entry) is not implemented; `portal` is used only for ghosts.
- Pack-by-pack control is the policy's: it pulls what is near the route, not a list of packs. There is no line-of-sight pulling, crowd control,
  boss mechanics, interrupt logic or add handling beyond the normal rotation. A tank that cannot hold threat is not detected.
- Healing uses the existing 60% rule, with no mana or cooldown planning beyond the party's rest rule before a pull.
- No re-entry after a `wipe` outcome (the run ends); a second attempt means a new run.
- Only one dungeon at a time, with one party.
