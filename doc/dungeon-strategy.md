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
| `trigger.type` | `near` (with `x`, `y`, `z`, `radius`: when the leader comes within `radius` yards), `after_boss` (with `boss`: the creature entry, once it is dead), `after_bosses` (with `bosses`: a list of creature entries, once every one is dead; bots 0.42.0) or `at_start` |
| `steps[]` | at least one: `{ "type": "use", "object": <entry>, "x","y","z", "item": <item entry, optional> }` walks to within 4 yards and clicks the object (or uses the item on it); `{ "type": "talk", "creature": <entry>, "option": <n from 0>, "x","y","z" }` walks to within 4 yards of the place and talks to that creature, picking gossip option n (kGossip, needs `Bot.Actions.Gossip` in the core; bots 0.42.0); `{ "type": "follow", "creature": <entry>, "seconds": 1-600 }` follows that creature (an escort NPC) for that long, a fight holding the step; `{ "type": "teleport", "x","y","z" }` puts the leader at the place (a scenario tool, needs `DungeonTeleport`; for a place the navmesh does not join to the rest); `{ "type": "wait", "seconds": 0-600 }` stands still |
| `timeout_sec` | 5 to 3600, default 120: the event is given up after this long |

Events with the same trigger run one after another in file order.

### `kill_priority` (top level, used since bots 0.19.3)

A list of creature entries, the first to be killed first: `"kill_priority": [1729, 4418, 1731, 1732, 622, 634]` (Deadmines: Defias Evoker, Defias Wizard, Goblin Craftsman, Defias Squallshaper, Goblin Engineer, Defias Overseer: the creatures with cast abilities in the world database's smart scripts and spell lists).
The damage dealers of the party (not the tank and not the healer) fight the best-ranked live hostile within 12 yards of the tank's target, not only the tank's own target; with
nothing listed they assist the tank's target as before. The setting `ScenarioKillPriority` (a comma list) does the same from the config and wins over the file. It needs the
`focus` tactic (below).

Blackfathom Deeps (2026-11-12) lists the Twilight casters of the room before Twilight Lord Kelris (`[4813, 4814, 4811, 4812]`: Shadowmage, Elementalist, Aquamancer, Loreseeker);
the 14 overnight parties all stopped there with 3 of 6 bosses and 5 deaths. It takes effect with the `focus` tactic; a sim run with `DungeonTactics = focus` is what shows whether it helps.

Two summoners are built in (they do not need the tactic): the Deadmines Goblin Engineer (622) once a Remote-Controlled Golem (2520) is in view, and, since the Ragefire fix,
the Searing Blade Warlock (11324) once its Voidwalker Minion (8996) is in view. Each is taken first by the damage dealers because the summoned creature goes away when its
owner dies. The Warlock is taken from up to 35 yards from the tank's target (it stands back and casts), the engineer from the usual 12.

### Dungeon tactics (`Module.forever-bots.DungeonTactics`)

A comma list of `focus`, `interrupt`, `heal`, `rest`, `cc`, or `all` / `none` (default: none, until a sim run shows a gain). Each is a rule of a dungeon party that can be measured on its own:

| Tactic | What it does |
|---|---|
| `focus` | the damage dealers take the pack's `kill_priority` unit first (above) |
| `interrupt` | a paladin, rogue, shaman or mage turns on a unit of the tank's pack that is casting, so its rotation's interrupt (Hammer of Justice, Kick, Earth Shock, Counterspell) fires; one bot at a time. A warrior has Shield Bash (Defensive Stance) and Pummel (Berserker Stance) rows in `rotations.txt`; it is not in the plan, so it kicks its own target only when no other kicker is planned for it. A cast with under 300 ms left (`cast_remaining_ms`, `Bot.Snapshot.CastInfo`) is not kicked |
| `heal` | a healer heals a party member below 55 % health (`DungeonHealBelowPct`; 60 % without the tactic, `kHealBelowPct`); the tank (the leader, with a target) is healed first, below 75 % (`DungeonTankHealBelowPct`) |
| `rest` | the leader pulls the next pack only when every member has 90 % health and 85 % mana (80 / 60 without the tactic); the hold lasts 45 s without recovery progress and 180 s at most |
| `cc` | a rogue (Sap), mage (Polymorph) or warlock (Banish) holds one extra member of the tank's pack that nobody has engaged yet and that is not the dungeon's own `kill_priority`; one bot, one unit at a time, the same rule `interrupt` uses. Sap and Polymorph only affect a Humanoid; Banish only a Demon or Elemental; a pack with nothing of a type the party's classes can hold does nothing, not an error. Sap and Polymorph are learned well under either shipped dungeon's spawn level (Deadmines 23, Ragefire Chasm 18); Banish is not (it needs level 28) and today has no Demon or Elemental to use it on in either dungeon either -- kept for a later, higher-level dungeon |

The party planner (`Module.forever-bots.PartyPlanner`, default 1; 0 turns it off): `party_planner.h` makes one plan per party every 400 ms from the merged view of all members. The nearest ready kicker is assigned to each casting unit, with a backup that may fire after 700 ms; with a plan only the assigned kicker turns on the caster. Crowd control: each ready caster is assigned a pack unit its spell affects (not a boss, not the kill priority, not engaged, not casting), two holds at a time, and a unit already held is not given to a second caster. It acts only through the `interrupt` and `cc` tactics, so with those off it changes nothing. The plan also holds the shared kill target (kill focus), the heal assignments (heal focus), one caster for each party buff and member (buff focus: a bot leaves a member's buff to the nearest other caster the plan names; the buff claims still guard a pair the plan does not cover), and, with `RaidRulesFile` (`doc/raid-rules.md`), the interrupt-first casts, the dispels and the tank swap (the main tank is read only for the swap taunt). The old per-bot elections (`_interruptClaims`, `_healClaims`, `_buffClaims`, the one-holder scan of crowd control, the focus scan's own tie-break) are still there as the fallback for a target or member the plan does not cover and for `PartyPlanner = 0`. `Module.forever-bots.PartyViewPlanner = 1` (default 0, needs the core's `Bot.Snapshot.PartyView`) lets the planner also read the core's shared party view: human members, tank roles and the loose mobs on non-tanks; without a view the bots-only merge is used.

Always on, not a tactic: a dead member of a dungeon party waits 20 s for a rezzer before it releases its spirit, the same as in any other party (`ReleaseDelayMs`; the owner's rule of 2026-10-27), and a release is held back while the party fights, for 60 s after the death at most (`ReleaseCombatHoldMs`). The route leader also waits for every mana user of the party to have 70% mana (`kRouteMinManaPct`; `route_hold why=mana`, 35 s at most, 180 s while the mana still rises).

### Parallel parties (`ScenarioParties`)

`Module.forever-bots.ScenarioParties = N` (1 to 20, default 1; 8 or fewer before 0.27.0) runs N parties of the dungeon's roster at once for a scenario. Each has its own director, so its own phases, boss kills and
result line (`scenario dm#2: ...`); the core makes one instance per group. The bots, in guid order, are cut into parties of the roster size, as the policy's party logic cuts them;
`MaxBots` is set to N x the roster size by the scenario. `tools/dungeon_report.py` reads the log and prints per party and in aggregate.

### Several dungeons in one run (`Scenario = dm:10,rfc:10`, bots 0.29.0)

`Module.forever-bots.Scenario` takes a list of dungeon ids with their number of parties: `Scenario = dm:10,rfc:10` runs 10 parties in the Deadmines and 10 in
Ragefire Chasm in the same hour. At most 200 parties in all: the entry that goes past 200 is shortened (a warning says so) and the ones after it are dropped. Each
listed dungeon takes its roster, level, staging, portal, entry, route, bosses, events and kill priority from its own dungeon file; `Dungeon` is not needed (and
is ignored). One key for one dungeon of the list is set with the dungeon's id after a dot:

    Module.forever-bots.Scenario = dm:10,rfc:10
    Module.forever-bots.ScenarioRoster.dm = 1:1,1:5,1:8,3:3,7:9      # an Alliance roster for the Deadmines
    Module.forever-bots.ScenarioLevel.dm = 18
    Module.forever-bots.ScenarioStaging.dm = 0 -8913 554 94          # Stormwind: the shipped files stage in Orgrimmar (gnomer: Ironforge)
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

## Party planner A/B protocol (before the old self-election code is deleted)

The fallbacks go only when a sim block shows the plan does not make a run worse. The unit tests prove the rules, not the outcome; this is the run that decides.

1. **Two blocks, same everything else.** Block A: `Module.forever-bots.PartyPlanner = 1` (default). Block B: `PartyPlanner = 0`. Same build, same drop-in otherwise (`DungeonTactics = all`, `PartyViewPlanner = 0`), same rosters. Do not set `RaidRulesFile` in either block (it needs the planner; it is measured separately, `doc/raid-rules.md`).
2. **Loop.** `sim-dungeon-loop.sh` with `DUNGEONS="dm"` and `PARTIES=14` (the 2026-11-07 Deadmines block had 14 parties), `ROTATION="base"` so the roster is the same every cycle, one deadline long enough for at least two cycles of each block (about 3 h per block). Run A and B back to back, in alternating halves if the sim is shared (A, B, A, B) so a drift of the machine shows up as a difference between the two A halves.
3. **Metrics, from the journal (`scripts/dungeon-study/ds_journal.py`) and the result lines**: per party `deaths`, `outcome` (`complete` share), `run` seconds, `bosses=N/M`, `time_s=fight:`; per run the counts of `fight_stalled`, `interrupt_target:` and `cc_target:` events. Also the number of `heal:` events per party and the `buff` casts per member (a buff cast twice on one member is the old double-cast).
4. **Reading.** The plan is at parity when, over the same number of parties: deaths per party are not higher by more than the spread between the two A halves; `fight_stalled` is not higher; `interrupt_target:` is at least as high (a kick lands on a casting unit) and `cc_target:` is not lower; the `complete` share is not lower. A difference inside the spread of A against A is no difference.
5. **Then** delete, in one change with the tests that cover them: `_interruptClaims` and `InterruptTaken`, the one-holder scan in `TurnCrowdControl`, `_healClaims`, `_buffClaims`, the focus scan's own fallback, and the `PartyPlanner` switch. Until then nothing is deleted.

What is still self-election after this change: leader election in `BuildParties`; the loose-mob pickup (needs the core's party view); the claims named above, which guard a target or member the plan does not list.

## Findings of 2026-11-12: SFK roof floor, BFD healer lag, ghost teleports

**SFK leader on the roof floor above Rethilgore (z about 100 at -258, 2123).** Read from the sim journal of 11-12 02:55 to 03:05 (map 33, party of bot 116):
- The party fought on the upper walkway at (-134, 2181, z 112) (killer entry 5097 there; leader died twice at 02:55), the leader rejoined its quiet party by teleport (`leader_rejoin`) to a member who stood on that level, then held `route_hold why=mana` at z 109 to 113 and walked point 4 (-233, 2148, z 90.8) "from=-128,2182,113" in 10 to 20 s steps that crept 15 yards in 3 minutes.
- At 03:01 the leader stood at (-258, 2117 to 2123, z 100): directly above point 7, Rethilgore's point (-253.3, 2121.5, z 81.3), 19 yards over it. The log shows `route_passed` for points 4, 5, 6 from z 100: the old X/Y-only reach and skip rules. The z-aware rules (commit 6100235 in this branch: reach 10, skip 12 yards of height) stop the leader holding under the boss and passing his points from the roof; the sim ran the older build.
- Why the walk ends on the roof (not fixed, needs a sim probe): the leader's `kMoveTo` to a lower-floor point is answered by the core's path to the nearest navmesh polygon of the destination, and the upper walkway has polygons 9 to 19 yards over the route points, so the walk can end at the roof polygon at the right X/Y. Point 4 (z 90.8) is only 9.2 yards under z 100, which the new reach rule (10 yards) still accepts as arrived; point 5 (z 87.3) is then 12.7 under and is not reached, and the walk repeats. Suggested next steps, in order: (1) a `kProbePoint` for the waypoint (ground_z, path_yards) before the walk, log it as `route_probe`; (2) lower `kRouteReachMaxZYards` to 6 so point 4 is not accepted from the roof; (3) when the leader's z is more than 8 yards above the waypoint and it has not moved for 30 s, teleport it to the last route point it reached on the right floor (the `back` teleport of the route code already does that for lost leaders); (4) add stair points to `data/dungeons/33-shadowfang-keep.json` between the courtyard (-143, 2183) and point 3 so that the path leg is short and on one floor. Nothing was changed here: without a sim run any of these can break the other floors of the keep.

**BFD healer lag (`route_hold why=health` for minutes).** Journal count since 11-12 00:00: 1,915 `route_hold` lines with `why=health`, many with `farthest_yards` of 200 to 350 (for example bot 371 at 73 % health, farthest member 251 yards; bot 221 at 50 %, 350 yards). The leader holds until its own health is 80 %; only a healer heals it, and a healer 250 yards behind never comes (it follows at its own pace, resting or drinking). Implemented (small, off the walking path): **healer fetch** in `RouteWalkOn` (`bot_policy_turn_move.cpp`): the leader out of combat, below `kRouteMinHealthPct`, no dead member to wait for, holds `kHealerFetchAfterMs` (45 s); then, if no healer (member with a heal spell, not a dps or tank role) is within `kHealerFetchYards` (45 yards, 2D), it walks to the nearest healer (`move_why healer_fetch`, log `healer_fetch: bot= healer= yards= hp_pct= held_ms=`). Within 45 yards the existing healer top-up (`kRouteTopUpPct`) takes over and the leader goes on. The walk goes back along ground the party has cleared. Unit test `ALeaderWalksBackToAFarHealer`. What the sim must check: `healer_fetch` lines appear, the matching `route_hold why=health` runs shrink from 10 minutes to about a minute, and no leader dies on the way back (`died:` with `last_decision` a move shortly after `healer_fetch`).

**Ghost teleport stagger.** `GhostTeleportPerSecond` (default 2, 0 = off): the module sends at most that many ghost teleports (corpse run limit, four-minute timeout, other-map corpse, dungeon entrance, to leader or member) in one second for the whole policy; a bot that does not get a slot tries again on the next tick. A ghost teleport the core refuses (`Bot.Teleport.GhostNeedsInstance`) clears `RunTimeoutTeleported`, restarts the run clock and is tried again after the usual 60 s (`corpse_run_teleport_refused:` in the log); before, the flag stayed set and the ghost only walked until `corpse_run_failed`.

## Telemetry

A scenario run (dungeon or raid) writes three kinds of event lines, so a run can be analysed and replayed from a file and not only from the journal. They are in the journal
(`forever-bots: event party_tl: ...`) in every case. With `Module.forever-bots.RunTelemetryDir = /absolute/dir` the same lines, with an ISO time in front
(`2026-11-12T10:00:30Z party_tl: ...`), are also appended to `<dir>/run-telemetry.log` (empty = off; a relative path is refused with a warning; a folder that cannot be opened
gives one warning and the file stays off). The file is opened once, written buffered and flushed every 30 s; it is never rotated, so point it at a folder you clean.
(`TelemetryFile` is the bots' CSV, a different file.)

All lines start with `scenario=<name> party=<n> variant= t_s=<seconds since the party's run start>`. `variant` is reserved for a later strategy variant and is empty today.
Fields come in a fixed order; new fields are only added at the end.

| Line | When | Fields after the common ones |
|---|---|---|
| `party_tl:` | one per party every 30 s of scenario time (never per bot) | `leader=<guid> map= pos=x,y,z step=<fight\|walk\|dead\|rest\|far\|idle> route=<point>/<total> target=<creature entry of the leader's target, 0 none> alive=<n>/<n> fight=<members in a fight> last=<marker>` |
| `party_stalled:` | no progress (boss kill or a route point advanced) for 300 s, then again every further 300 s | `cause= no_progress_s= pulls= route_refusals= rest_s= dead_s= fight_s= stalls=<n so far> route= last=` |
| `boss_attempt:` | when a member fights a boss of the dungeon file (`stage=start`), and when the boss dies or the fight is over (`stage=end`) | `boss=<name, spaces as _> stage=start\|end dur_s= deaths=<deaths during the attempt> outcome=open\|killed\|aborted` |

`last=` is the last progress marker: `boss:<name>@<t_s>` or `route:<point>@<t_s>` (`route:0@0` before the first point). `step` is the time state of the result line's `time_s=`.

The stall cause is one of `fight_churn` (10 or more fight starts since the last progress: the pull churn of `pull_unreachable`), `route_refusals` (5 or more moves of the leader refused
or without a route), `resting` (a member below the ready health or mana for 40 % of the window or more: the regroup wait), `dead_members` (members down for 40 % or more),
`unknown`. The counters of the window since the last progress (or the last verdict) are in the line. The highest ratio over its threshold wins. A boss fight that lasts over
300 s counts as no progress and shows as `unknown` or `fight_churn`.

The scenario's result line gets `stalled=<n> last_stall=<cause>@<t_s>` after `time_s=` when the party stalled at least once; it is left out otherwise.

**Damage by spell (protocol 29).** With a core that sends the damage event (`PlayerScript.OnDamaged`) and `DamageEvents = 1`, a `party_tl:` line gets these fields at its end when a bot of the party
took a killing blow or a big hit in the 30 s it stands for, and the `boss_attempt:` end line gets them for the whole attempt (never on the start line, never when empty):

    dmg_by_spell=<spell>:<damage>:<hits>:<deaths>,...   dmg_by_attacker=<creature entry>:<damage>:<hits>:<deaths>,...   (the top 5 by damage; spell 0 = melee or auto-attack; entry 0 = a player or no creature)
    death_by_spell=<spell>:<deaths>:<overkill>,...      death_by_attacker=<entry>:<deaths>:<overkill>,...              (the top 5 by deaths; only when there was a death)

The core sends only killing blows and hits of at least `Modules.DamageEvent.MinPct` (default 20 %) of the bot's maximum health (the module's `DamageEventMinPct` can raise that floor), and at most
`Modules.DamageEvent.MaxPerSecond` (200) a second, so these totals are the big hits and the deaths, not a damage meter; small damage over time does not show. The `party_tl:` window restarts every
line (30 s, not cumulative); the attempt total runs from the start line to the end line.

The `died:` line gets, after its old fields (`bot life map pos last_decision killer_entry in_combat`), `killer_spell=<id|none> killer_src=<event|guess> overkill=<n|none> hit_amount=<n|none> hit_hp_pct=<n|none>
last_hits=<spell:amount,...>`. With `killer_src=event` the `killer_entry` is the real attacker (`none` for a hit with no creature: a player, fall damage), `killer_spell` its spell (0 = melee), `hit_amount` the killing
blow, `hit_hp_pct` the bot's health before it, and `last_hits` its last five big hits, newest first. With `killer_src=guess` (an older core, `DamageEvents = 0`, or no event for this death within 15 s) `killer_entry` is
the old guess (the bot's threat target, else the closest live hostile) and the other fields are `none`.

Read a run with `tools/run_telemetry.py run-telemetry.log` (or a journal file, or `-` for standard input; `--party dm#2` for one party): per party the time per step, the route
point reached in 5 minute buckets, the stalls with their cause and the boss attempts. Or by hand:

    grep -h 'party_stalled:' run-telemetry.log                                   # every stall
    grep -h 'party_tl: scenario=sfk party=3 ' run-telemetry.log | awk '{print $6, $10, $11}'   # t_s, step, route of one party (field 1 is the ISO time)
    awk '/boss_attempt:/ && /stage=end/ {print $7, $9, $10, $11}' run-telemetry.log   # boss, duration, deaths, outcome

`python3 tools/run_telemetry.py deaths <journal|-> [--spells spells.txt] [--top 30]` reads the `died:` lines and prints a table: one row per (attacker entry, spell id) with the deaths, the damage and overkill of the killing blows,
and the average health percent before the blow; guesses are one row per guessed entry, marked. Spell names come from the optional `--spells` file (`<id> <name>` per line); the world DB is not read.
