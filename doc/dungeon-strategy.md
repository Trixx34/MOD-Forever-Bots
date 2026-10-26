# Dungeon strategy files (one JSON file per dungeon)

Since bots 0.19.0 everything the module knows about one dungeon lives in **one JSON file**: where to go in, the route, the doors and levers on the way, and a block per boss.
You can change a dungeon by editing its file, and you can keep your changes in a folder of your own so an update of the module never overwrites them. Nothing needs a rebuild.

    data/dungeons/36-the-deadmines.json        the Deadmines (map 36)
    data/dungeons/389-ragefire-chasm.json      Ragefire Chasm (map 389)
    ... one file for each of the nine dungeons the old text files had

The old text files (`data/dungeons.txt`, `data/dungeon_events.txt`) are still read, but only for a map that has no JSON file. Their content has been converted, line for line
(`tools/convert_dungeons.py`, and a unit test compares what the module reads from the JSON files with what it read from the text files).

## Comments

A dungeon file may hold comments: `// ...` to the end of the line and `/* ... */` anywhere between tokens (this is not standard JSON; the module's reader accepts it,
and text inside a string is never a comment). The shipped files use them to explain each section, the bosses and the doors. Comments are ignored and are not kept when a file
is written again by `tools/convert_dungeons.py --force`, which overwrites a hand-edited file: copy your edited file elsewhere (your `DungeonDir`) instead of regenerating it.
A tool that reads these files as plain JSON (`python3 -m json.tool`) will refuse the comments; strip them first.

## File names and where files are read from

The name is `<map id>-<name>.json`. The map id is the map of the dungeon's inside (36 for the Deadmines); the name after the dash is for people and can be anything
(`36-the-deadmines.json`, `36-dm-hard-mode.json`). The map id in the name must be the file's own `"map"`, so a copied file with a forgotten edit is caught.
A `.json` file without a map id in front of its name is ignored with a warning.

For one map the module uses the first of these that holds a good file:

| Priority | Setting | What |
|---|---|---|
| 1 | `Module.forever-bots.DungeonDir` | **your own folder** (an absolute path). A file here for a map **replaces** the shipped file for that map. Maps you have no file for keep the shipped one. A file for a map the shipped files do not have adds a dungeon |
| 2 | `Module.forever-bots.DungeonDataDir` | the shipped files. Default: the folder `dungeons` beside `DungeonFile`, so `data/dungeons` when `DungeonFile = data/dungeons.txt` (nothing to set) |
| 3 | `Module.forever-bots.DungeonFile` (+ `DungeonEventsFile`) | the old text files, for maps without a JSON file |

`Dungeon = <id>` (the `"id"` of a file, for example `dm`) still picks the dungeon of a scenario run, as before; the tank lead command works in every dungeon the module knows.

### Making your own version of a dungeon

1. Make a folder for your files, for example `/srv/forever/sim/dungeons-mine`, and set `Module.forever-bots.DungeonDir = /srv/forever/sim/dungeons-mine`.
2. Copy `data/dungeons/36-the-deadmines.json` into it and edit the copy (add a waypoint, move a door lever, change the roster of the test party).
3. Reload (below). Take the file out of the folder again and the shipped one is back.

A mistake is never fatal. A file with a problem is **not used**; the module logs the file, the place in it and the reason, and the next place down the list supplies that map:

    forever-bots: dungeons: /srv/forever/sim/dungeons-mine/36-the-deadmines.json: not used: route[3].y: must be a number
    forever-bots: dungeons: /srv/forever/sim/dungeons-mine/36-the-deadmines.json: bosses[0].strategy: unknown key 'pul'; ignored     (a warning: the file is used)

An unknown key is only a warning (so a typo in a key you meant to use shows up in the log, and a newer file still loads on an older module).

### Reloading

The files are read at start, and again every time the module's config is delivered again (a reload of the core's module configuration): the
log says `dungeon files reloaded: tank lead knows N dungeon(s)`. A reload changes what **tank lead** knows: the routes, boss points and events of the dungeons, from the next
`tank lead` order on. A tank that is leading now keeps the route it started with. The settings of a **scenario run** (`Dungeon = <id>`: roster, entry, duration, route) are taken
when the module starts, so change those and restart.

## The schema (`"schema": 1`)

All coordinates are game coordinates `x`, `y`, `z` as in the other data files. Numbers are plain JSON numbers (`-14.57`). Every key below is optional unless it says
required. "Used" says whether the bots act on it today.

| Key | Type | | Used |
|---|---|---|---|
| `schema` | number | required, must be `1` | yes |
| `id` | word | required: letters, digits, `-`, `_`, up to 32. What `Dungeon = <id>` picks. Unique across the dungeons | yes |
| `map` | number | required: the map id of the inside; must match the file name and `entrance.entry.map` | yes |
| `name` | text | free text for logs | yes |
| `levels` | `[min, max]` | 1 to 60 (default `[1, 60]`). A scenario spawns its bots at `max` | yes (scenario) |
| `roster` | text | required: `race:class` pairs for the tank, the healer and the dps, 1 to 5, as `ScenarioRoster` (`"2:1,5:5,8:8,2:9,5:4"`) | yes (scenario) |
| `entrance.entry` | `{map,x,y,z}` | required: where the party is put inside | yes |
| `entrance.staging` | `{map,x,y,z}` | where the test bots log in | yes (scenario) |
| `entrance.portal` | `{map,x,y,z}` | the instance door outside, for a corpse run | yes (scenario) |
| `run.duration_sec` | number | the run is given up after this long (default 1800) | yes (scenario) |
| `run.wipe_recovery_sec` | number | how long a wiped party may take to get a bot back (default 0 = a wipe ends the run) | yes (scenario) |
| `run.min_alive` | number | give up when fewer are alive for a while (0 = off) | yes (scenario) |
| `run.leader_dead_abandon_sec` | number | give up after the leader is dead this long (0 = off) | yes (scenario) |
| `route` | list | required, at least one waypoint, walked in order by the tank. A waypoint is `{ "x": .., "y": .., "z": .. }`, and `"boss": <creature entry>` makes it a boss point: the leader does not go past it, and does not skip it when it cannot route to it, before that boss is dead | yes |
| `bosses[]` | list | one block per boss: `entry` (creature entry, unique in the file), `name` (shown in logs; the route's boss points take their name from here), `strategy` | name: yes |
| `events[]` | list | the doors, levers and other things the leader must do on the way (below) | yes |
| `notes` | text | anything you want to remember; ignored | no |

### Events

Same behaviour as before (`doc/dungeon-events.md`): the leader runs an event as a short list of steps, once per run, paused while the party fights.

    { "id": "factory-lever", "name": "Factory Door (lever)",
      "trigger": { "type": "near", "x": -188.1, "y": -460.3, "z": 54.6, "radius": 35 },
      "steps": [ { "type": "use", "object": 101831, "x": -188.1, "y": -460.3, "z": 54.6 } ],
      "timeout_sec": 120 }

| Key | |
|---|---|
| `id` | required, one word, unique in the file |
| `name` | free text |
| `trigger.type` | `near` (with `x`, `y`, `z`, `radius`: when the leader comes within `radius` yards), `after_boss` (with `boss`: the creature entry, once it is dead) or `at_start` |
| `steps[]` | at least one: `{ "type": "use", "object": <entry>, "x","y","z", "item": <item entry, optional> }` walks to within 4 yards and clicks the object (or uses the item on it); `{ "type": "wait", "seconds": 0-600 }` stands still |
| `timeout_sec` | 5 to 3600, default 120: the event is given up after this long |

Events with the same trigger run one after another in file order.

### `kill_priority` (top level, used since bots 0.19.3)

A list of creature entries, the first to be killed first: `"kill_priority": [1729, 4418, 1731, 1732, 622, 634]` (Deadmines: Defias Evoker, Defias Wizard, Goblin Craftsman, Defias Squallshaper, Goblin Engineer, Defias Overseer: the creatures with cast abilities in the world database's smart scripts and spell lists).
The damage dealers of the party (not the tank and not the healer) fight the best-ranked live hostile within 12 yards of the tank's target, not only the tank's own target; with
nothing listed they assist the tank's target as before. The setting `ScenarioKillPriority` (a comma list) does the same from the config and wins over the file. It needs the
`focus` tactic (below).

### Dungeon tactics (`Module.forever-bots.DungeonTactics`)

A comma list of `focus`, `interrupt`, `heal`, `rest`, or `all` / `none` (default: none, until a sim run shows a gain). Each is a rule of a dungeon party that can be measured on its own:

| Tactic | What it does |
|---|---|
| `focus` | the damage dealers take the pack's `kill_priority` unit first (above) |
| `interrupt` | a paladin, rogue, shaman or mage turns on a unit of the tank's pack that is casting, so its rotation's interrupt (Hammer of Justice, Kick, Earth Shock, Counterspell) fires; one bot at a time |
| `heal` | a healer heals a party member below 75 % health (60 % without the tactic); the tank first, as before |
| `rest` | the leader pulls the next pack only when every member has 90 % health and 80 % mana (80 / 60 without the tactic) |

Always on, not a tactic: a dungeon party's dead wait 60 s for the healer before they release their spirit (20 s in other parties; 90 s before 0.31.0). The route leader also waits for every mana user of the party to have 80% mana (`route_hold why=mana`, 150 s at most).

### Parallel parties (`ScenarioParties`)

`Module.forever-bots.ScenarioParties = N` (1 to 20, default 1; 8 or fewer before 0.27.0) runs N parties of the dungeon's roster at once for a scenario. Each has its own director, so its own phases, boss kills and
result line (`scenario dm#2: ...`); the core makes one instance per group. The bots, in guid order, are cut into parties of the roster size, as the policy's party logic cuts them;
`MaxBots` is set to N x the roster size by the scenario. `tools/dungeon_report.py` reads the log and prints per party and in aggregate.

### Several dungeons in one run (`Scenario = dm:10,rfc:10`, bots 0.29.0)

`Module.forever-bots.Scenario` takes a list of dungeon ids with their number of parties: `Scenario = dm:10,rfc:10` runs 10 parties in the Deadmines and 10 in
Ragefire Chasm in the same hour. At most 20 parties in all: the entry that goes past 20 is shortened (a warning says so) and the ones after it are dropped. Each
listed dungeon takes its roster, level, staging, portal, entry, route, bosses, events and kill priority from its own dungeon file; `Dungeon` is not needed (and
is ignored). One key for one dungeon of the list is set with the dungeon's id after a dot:

    Module.forever-bots.Scenario = dm:10,rfc:10
    Module.forever-bots.ScenarioRoster.dm = 1:1,1:5,1:8,3:3,7:9      # an Alliance roster for the Deadmines
    Module.forever-bots.ScenarioLevel.dm = 18
    Module.forever-bots.ScenarioStaging.dm = 0 -8913 554 94          # Stormwind: the shipped files stage in Orgrimmar
    Module.forever-bots.ScenarioLevel.rfc = 17
    Module.forever-bots.ScenarioDurationSec = 3600                   # a plain shared key: every dungeon of the list

- Any `Scenario*` key works with `.<id>` (`ScenarioEntry.dm`, `ScenarioRoute3.rfc`, `ScenarioKillPriority.dm`, ...) and wins over that dungeon's file.
- The plain dungeon keys (`ScenarioRoster`, `ScenarioLevel`, `ScenarioStaging`, `ScenarioEntry`, `ScenarioPortal`, `ScenarioRoute<n>`, `ScenarioBosses`,
  `ScenarioBossAt`, `ScenarioKillPriority`, `ScenarioParties`) belong to the single form: with a list they are ignored, one warning each.
- The other plain keys (`ScenarioDurationSec`, `ScenarioGear<class>`, `ScenarioSupplies`, `ScenarioWipeRecovery*`, `ScenarioMinAlive`,
  `ScenarioLeaderDeadAbandonSec`, `ScenarioRequireBosses`, `ScenarioResultFile`) apply to every dungeon of the list.
- A roster is one faction (Alliance races 1, 3, 4, 7; Horde 2, 5, 6, 8): a roster with both is refused with a warning and that dungeon does not run (also in
  the single form, where it could never form its party). Every roster of the list must have the size of the first one's: the bots, in guid order, are cut into
  parties of one size; a dungeon with another size is dropped with a warning.
- The bots are spawned in party order, the first dungeon's parties first, each with the race, class, level and staging place of its own dungeon. Each party's
  leader walks its own dungeon's route, and a bot of a party that is outside walks back to its own dungeon's door.
- The result lines are numbered per dungeon: `scenario dm#1` .. `dm#10`, `scenario rfc#1` .. `rfc#10`. At start the module logs one line per dungeon (parties,
  bots, level, route length, duration) and the totals. `MaxBots` is set to the sum of roster size x parties.

`Scenario = rfc` (one name, no `:n`) is the single form, as before: `Dungeon`, the plain keys and `ScenarioParties` apply.

### Boss strategy (`bosses[].strategy`): `avoid` and `avoid_spells` are used, the rest is reserved

`avoid` and `avoid_spells` are acted on since bots 0.20.0 when `AvoidGroundEffects = 1` (see `doc/ground-effects.md`): a bot standing in one of those zones, or in the ground effect of one of
those spells, walks out of it. The other fields below are **read, checked and kept**, but **no bot behaviour uses them yet**. They exist so that you can write down how a fight is meant to go and so that a file written today keeps
working when the code starts to act on them. What the bots do in a boss fight today is: the tank leads and pulls pack by pack (and holds the pull until the party is healthy and together), the healer heals
the member with the lowest health, the dps assist the tank, and every class follows its own rotation (`data/rotations.txt`).

| Key | Meaning (for later) |
|---|---|
| `pull` | one word, for example `tank`, `ranged`: how the boss is pulled |
| `tank_position` | `{x,y,z}`: where the tank should hold the boss |
| `interrupt` | list of spell ids to interrupt |
| `dispel` | list of spell ids to dispel |
| `avoid` | **used** (AvoidGroundEffects): list of `{ "x","y","z","radius","note" }`: ground to stay out of (radius 0 to 500 yards); a bot inside walks out |
| `avoid_spells` | **used** (AvoidGroundEffects): list of spell ids whose ground effect (a dynamic object or area trigger of that spell) a bot steps out of, on top of `data/avoid_spells.txt` and the effects of hostile casters |
| `kill_order` | number 1 to 50: when this boss is killed among the others, if the route allows a choice |
| `notes` | free text, up to 500 characters |

Everything else the bots do in a dungeon (routes, boss points, doors and levers, the tank lead command, the scenario run) is driven by the keys marked "yes" above.

## A worked example

A small dungeon on map 7 with one boss and one door. Save it as `7-tiny-hold.json` in your `DungeonDir`.

    {
      "schema": 1,
      "id": "tiny",
      "map": 7,
      "name": "Tiny Hold",
      "levels": [10, 14],
      "roster": "2:1,5:5,8:8",
      "entrance": {
        "staging": { "map": 1, "x": 1629, "y": -4373, "z": 31 },
        "portal":  { "map": 0, "x": 4.5, "y": 5.5, "z": 6.5 },
        "entry":   { "map": 7, "x": -1.25, "y": -2.5, "z": 3.75 }
      },
      "run": { "duration_sec": 900, "wipe_recovery_sec": 120 },
      "route": [
        { "x": 10, "y": 20, "z": 30 },
        { "x": 11, "y": 21, "z": 31, "boss": 900 },
        { "x": 12, "y": 22, "z": 32 }
      ],
      "bosses": [
        { "entry": 900, "name": "Big Bad Boss",
          "strategy": {
            "pull": "tank",
            "tank_position": { "x": 11, "y": 21, "z": 31 },
            "interrupt": [8984],
            "avoid": [ { "x": 14, "y": 24, "z": 31, "radius": 8, "note": "fire on the floor" } ],
            "kill_order": 1,
            "notes": "pull to the door"
          } }
      ],
      "events": [
        { "id": "gate", "name": "Gate (lever)",
          "trigger": { "type": "near", "x": 9, "y": 9, "z": 9, "radius": 20 },
          "steps": [ { "type": "use", "object": 555, "x": 9, "y": 9, "z": 9 } ] }
      ]
    }

(The `strategy` block of this example is reserved: it is read and kept, and the bots do not act on it yet. The route, the boss point, the roster, the entrance and the gate are all in use.)

## Turning the old text files into JSON

`python3 -I tools/convert_dungeons.py` reads `data/dungeons.txt` and `data/dungeon_events.txt` and writes one JSON file per dungeon into `data/dungeons/`, copying every number as it was written. It refuses to
replace a file that exists unless `--force` is given (do not use that over files you edited), and `--map <id>` converts only that dungeon. `tools/make_dungeon.py` still writes text blocks; convert the new block with
`convert_dungeons.py --map <id>`. The boss list of a converted file has an empty `strategy` for you to fill in.

## Limits

A file is at most 1 MB, 300 route waypoints, 50 bosses, 100 events, 50 steps in an event, 64 avoid zones per boss. JSON has no comments: use the `notes` keys. The reader accepts standard JSON only (a trailing comma is an error that
names the line and column).
