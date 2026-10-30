# Dungeon events and "tank lead"

**Since bots 0.19.0 the events of a dungeon are in its JSON file** (`events[]` in `data/dungeons/<map id>-<name>.json`, format in `doc/dungeon-strategy.md`), next to its
route, and can be overridden from your own folder (`DungeonDir`). The text format below is the old one: `data/dungeon_events.txt` is read only for a map that has no JSON
file. The behaviour (triggers, steps, timeouts, the runner) is the same.

Status: built and unit-tested (`src/dungeon_events.*`, `tests/dungeon_events_test.cpp`, the policy tests for tank lead). The core side is `ActionType::kUseObject`
(layout 19 / protocol 14, `Bot.Actions.UseObject`). **Not run on a sim yet.**

## One table for every dungeon

`data/dungeon_events.txt` holds the things a party has to do to get on, for all dungeons, keyed by map id. It is data, not code per dungeon: a new dungeon is new lines in the
file. Format (`src/dungeon_events.h`):

    event <map> <id> <name...>
    after_boss <entry>                  trigger: once that boss is seen dead      (or)  near <x> <y> <z> <radius>   (or)  at_start
    use <object entry> <x> <y> <z> [item <item entry>]    walk to within 4 yards of the place, then click the object, or use the item on it
    talk <creature entry> <option> <x> <y> <z>   (bots 0.42.0) walk to within 4 yards, then talk to the creature and pick gossip option <option> (from 0): kGossip
    follow <creature entry> <seconds>   (bots 0.42.0) follow that creature (an escort) for that long
    teleport <x> <y> <z>                (bots 0.42.0) put the leader at the place (DungeonTeleport)
    after_bosses <entry> <entry> ...    trigger (bots 0.42.0): once every one of those bosses is dead
    wait <seconds>
    timeout <seconds>                   given up after this long (default 120)
    end

The leader of a dungeon party (the scenario's, or the tank under "tank lead") runs the first event whose trigger holds, one after the other, once per run. While an event runs
it does only what the event asks and the route waits; an event is paused while the party fights. A click on a door or lever that is open already does nothing (the core checks), so
`near` events can fire whenever the leader comes by. Three refusals in a row, or the timeout, end an event with a log line (`event_failed`, `event_timeout`).

The Deadmines are in the file: the Factory, Mast Room and two Heavy Doors with their levers or click (read from the world database), and the Iron Clad Door, which the same party member
opens by clicking the Defias Gunpowder chest in the room before it and then firing the Defias Cannon with a charge of Defias Gunpowder (item 5397; the core gives the bot one).
The Foundry Door needs no event: it opens on its own. Other dungeons need their lines added; the doors and levers of a map can be listed from `gameobject` and
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

## Recovery by teleport (DungeonTeleport)

Bots that wander onto another level of an instance (a ledge above the route, a pit) have no path back, and the dungeon stalls. With `DungeonTeleport = 1` (and `Bot.Actions.Teleport` in the core, so for tests and
scenarios, never in normal play):

- A leader whose move to a route point is refused three times for lack of a route is put back on the route once, on the point before (`route_recover` in the log). If the same point fails
  again, it is skipped (a boss point is never skipped: `route_blocked`). Since 0.28.0 a walk that does not move the leader 3 yards in 30 seconds, out of combat, is sent again and counts as
  one of those refusals (`route_stall`).
- A leader revived at the instance entrance while a living member is more than 30 yards further in is put next to that member (`leader_recover`, since 0.28.0), and the members are not
  put back at the entrance. Before, the members were teleported to the leader and the party walked the cleared way again.
  Since the next version the leader first rests at the entrance until it has 80% health (`leader_recover_wait`, at most 120 seconds, then it goes in anyway), and the members deep in the dungeon do not walk
  back to it: a leader revived at about half health and teleported into the party died again within 120 seconds in 24 of 60 cases (Stockade sim, 2026-10-28).
- A leader that does not walk on, out of combat, says why every 30 seconds (`route_hold`, since 0.28.0): `why=health` (below 80%), `why=mana` (a mana user of the party below 80%; the members sit and drink, and the leader goes on after 150 seconds whatever the mana, since 0.31.0), `dead_member`, `member_far` (with the member farthest
  behind and how far) or `player_far`. These three need no `DungeonTeleport` for the log lines; the teleport needs it.
- A member more than 30 yards from its leader (the distance the leader waits at; 60 before 0.27.1), or more than 25 yards above or below it, for 20 seconds is put next to the leader (`member_recover`), and not again for 30 seconds.
