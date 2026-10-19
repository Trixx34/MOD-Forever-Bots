# Dungeon events and "tank lead"

Status: built and unit-tested (`src/dungeon_events.*`, `tests/dungeon_events_test.cpp`, the policy tests for tank lead). The core side is `ActionType::kUseObject`
(layout 19 / protocol 14, `Bot.Actions.UseObject`). **Not run on a sim yet.**

## One table for every dungeon

`data/dungeon_events.txt` holds the things a party has to do to get on, for all dungeons, keyed by map id. It is data, not code per dungeon: a new dungeon is new lines in the
file. Format (`src/dungeon_events.h`):

    event <map> <id> <name...>
    after_boss <entry>                  trigger: once that boss is seen dead      (or)  near <x> <y> <z> <radius>   (or)  at_start
    use <object entry> <x> <y> <z> [item <item entry>]    walk to within 4 yards of the place, then click the object, or use the item on it
    wait <seconds>
    timeout <seconds>                   given up after this long (default 120)
    end

The leader of a dungeon party (the scenario's, or the tank under "tank lead") runs the first event whose trigger holds, one after the other, once per run. While an event runs
it does only what the event asks and the route waits; an event is paused while the party fights. A click on a door or lever that is open already does nothing (the core checks), so
`near` events can fire whenever the leader comes by. Three refusals in a row, or the timeout, end an event with a log line (`event_failed`, `event_timeout`).

The Deadmines are in the file: five doors with their levers (read from the world database) and the Defias Cannon that opens the Iron Clad Door (a charge of Defias Gunpowder,
item 5397, used on the cannon; the core gives the bot one). Other dungeons need their lines added; the doors and levers of a map can be listed from `gameobject` and
`gameobject_template` (types 0, 1 and 10), as was done for the Deadmines.

Core: `ActionType::kUseObject` (spell_id = the object's entry, target_guid = an item entry to use on it or 0, dest = where it stands). The nearest object of that entry within 30 yards is
used, the bot must be within 8 yards (status "out of range" otherwise: the runner walks closer and tries again). Off unless `Bot.Actions.UseObject = 1` in the core.

## tank lead

In party chat inside a dungeon the module has a route for (`DungeonFile`): **`tank lead`**. The tank (the selector `tank` = the warrior; `role tank` changes who) walks the group through the
dungeon, boss by boss, and the other bots follow the tank instead of the speaker. The dungeon party rules apply: the tank pulls only when the group is healthy and has its mana, the
others never pull and join the fight, a healer keeps the tank healed. The tank waits for the speaker (a player out of its view counts as far) and for members more than 30 yards behind.
It holds at each boss point until that boss is seen dead (a creature of that entry with no health, seen by the tank or any member), never skips a boss for lack of a route, and gives
a boss up after 3 minutes if it never shows.

- Started in the middle of a dungeon it takes the route from the point nearest to where it stands.
- `tank lead off`, `stop`, or leaving the dungeon (a map with no route) ends it; it does not start again by itself.
- Not in a dungeon with a known route: the bot says so ("not in a dungeon with a known route").
- Needs `ChatOrders = 1` and `DungeonFile` (and `DungeonEventsFile` for the events) in the module config.

## Routes planned over the navmesh

`data/dungeons.txt` is made in two steps: `tools/make_dungeons_from_db.py` (doors, spawns, bosses) and `tools/replan_dungeons.py` with `tools/mmap_check` (build with `tools/mmap_check/build.sh`),
which replaces each route by one that goes boss to boss over the navmesh files the worldserver uses: the nearest boss first by walking distance, the corners of the walkable path between
two bosses as waypoints, so the leader follows the corridors and every point is on the mesh. Trash is left to the party, which fights what it meets (and the tank pulls what is near the
route). A boss that cannot be walked to from the one before is left out and the tool says so (Wailing Caverns 4, Shadowfang Keep 3, Blackfathom Deeps 2, Gnomeregan 1): those dungeons
need an event or a door opened first. `mmap_check <mmaps> <dungeons.txt> <id>` checks any route leg by leg.

## What the repo mod-dungeon-clear (AzerothCore) does that this does not yet

It routes the same way, from the live navmesh boss to boss, and has hand-written event tables per dungeon in code, a choice between running into packs and pulling them to a camp (by how many
creatures would wake), resurrection of the fallen by a living healer before going on, and a test harness that runs random parties many times for a success rate. Of these, per-dungeon code
was not copied (this table is data), and the success-rate harness is the useful thing left to build.
