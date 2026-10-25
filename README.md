# MOD-Forever-Bots

Bot AI as a ForeverBots module. The core creates, logs in and moves the bots; this module decides what each bot does: it reads a
snapshot of every bot each tick and publishes actions (move, follow, attack, cast, rest, vendor, party, loot). Goal: bots that level
from 1 to 60 and, in a party of five, clear a dungeon.

## Status

Built and unit-tested (16 test suites). Run on a dedicated sim worldserver with 300, 500 and 1000 bots: spawn and login, the ramp,
the safety valves, telemetry and the pump all work. What is **not yet verified** on the sim: sustained combat and kills (the last run
found bots could not see neutral creatures, now fixed), experience and levelling, vendor and hub walks, parties and looting.

| Milestone | State |
|---|---|
| M1 facts and feedback | done |
| M2 combat: rotation, approach, over-pull control, rest | built, not verified |
| M3 vendor, M4 trainer | built; the trainer check is off by default (bots learn spells on level-up) |
| M6 travel to a vendor | built, not verified |
| M7 hubs, looting, safe pulls | built, not verified |
| M8 parties | built, not verified |
| M9 dungeons: data file of routes, `Dungeon` config, boss-clear rule | first slice built (unit-tested), not run on the sim; see `doc/dungeons.md` |
| M5 quests 1 to 10, levelling 1 to 60 | partly built (quest, whole-world quest and grind-spot data), not verified |

## Needs

- The SDK at layout 7 with the loot action (`forever-module-sdk`, branch `m7-quest-share-sdk` or later).
- The core with bot support (`BotMgr`), the Tier 2 snapshot at layout 7 and the loot action (branch `m7-quest-share-core` or later).
  The core keeps a byte-identical copy of the SDK's `tier2_layout.h`; a mismatch is refused at region attach.

## Stored state

The bots the module controls are rows in the core's module store (doc 15): `bot/<guid>`, holding the race and class. On start the
module lists these rows and takes its bots back first, up to `MaxBots`, spawning only for the rest. A row is removed only when the
core says the character does not exist, not on a revoke, because the core revokes every bot at shutdown. Clear the module's store rows
whenever the bot characters are deleted, or the module tries to reclaim characters that no longer exist.

## Configuration

The core sends the module the `Module.forever-bots.<Key>` entries from `worldserver.conf.d/modules.d/forever-bots.conf` (under the
config directory), with the prefix removed. See `config/modules.d/forever-bots.conf.example`. A bad value keeps the default and writes a
line to the module's stderr. A config reload updates the ramp, the random picks and the cap without logging out any bot.

| Key | Default | Meaning |
|---|---|---|
| `MaxBots` | 1 | Bots controlled at once, 1 to 1500 (capped by the core's Tier 2 grant) |
| `Race`, `Class` | 1, 1 | A race and class id, or `random` (a seeded pick over the Forever valid combinations, including the Skyborne races 95 and 96) |
| `RandomSeed` | 1 | Seed for the random picks |
| `BotsInitial`, `BotsStep`, `BotsStepMinutes`, `BotsCap`, `BotsStopTime` | 300, 100, 15, 1500, 06:00 | The ramp: start count, bots added per step, minutes per step, the cap, and the local time it stops adding |
| `ServiceEverySec` | 600 | Seconds between vendor visits for an idle bot (60 to 86400) |
| `ChatSay`, `ChatSayMax` | 0 (off), 3 | A bot that takes a chat order answers with a short /say line (0 to 50 bots speak for one order; a whispered bot always answers) |
| `Supplies` | 0 (off) | Bots buy ammunition, food and drink at vendors that sell what they can use, and eat and drink while resting (needs `Bot.Actions.Supplies` in the core) |
| `SupplyAmmoLow`, `SupplyAmmoTarget` | 100, 400 | A bot with fewer pieces of ammunition goes shopping, and buys up to the target |
| `SupplyFoodLow`, `SupplyFoodTarget`, `SupplyDrinkLow`, `SupplyDrinkTarget` | 3, 10, 3, 10 | The same for food, and for drink (bots with mana) |
| `SupplyTripSec` | 120 | A bot that needs supplies makes a trip at most this often (30 to 3600); a trip that bought nothing waits 10 minutes, doubling each time (an hour at most) |
| `QuestTeleport` | 0 | A quest walk stuck for two minutes teleports to its giver (needs `Bot.Actions.Teleport = 1`); 0 drops the start and grinds instead |
| `TrainEverySec` | 0 (off) | Seconds between trainer checks, also after a level-up (0, or 60 to 86400) |
| `PartySize`, `PartyCount` | 0 (off), 1 | Bots per party (at most 5) and number of parties |
| `Hub<map>_<n>` | none | Hub positions `"x y z"` per map, in order |
| `TestSpell` | 0 (off) | A spell cast at the target every few ticks instead of the rotation, to test the cast path |
| `TelemetryFile`, `TelemetryMaxMB`, `TelemetryKeep`, `TelemetrySummarySec` | `forever-bots-telemetry.csv`, 64, 5, 60 | The telemetry file (empty turns it off), its rotation, and the summary interval |

The core has its own bot limits in `worldserver.conf`: `Bot.Login.MaxPerSecond`, `Bot.Limit.MaxBots`, `Bot.Limit.MaxWorldDiffMs`,
`Bot.Limit.MaxDbQueue`, `Bot.Run.StopTime`, `Bot.Spread.SpacingYards`, `Bot.Spread.MaxRadiusYards` and the `Modules.Tier2.*` keys.

## How a bot behaves

Each tick the policy (`src/bot_policy.cpp`) reads the bot's snapshot and sends an action only when the decision changes.

- **Target.** The bot's own threat target is always fought. Otherwise it picks the closest creature the core marks as fightable
  (hostile, or a neutral "yellow" creature that is not a critter or an NPC with a role; never one a player cannot attack, such as a
  trainer). It skips a creature more than two levels above it and elites, and does not pull a group of more than three within 10 yards;
  but a bot that is in combat with no target of its own is being attacked, and fights back against the closest creature whatever its level.
- **Refused targets.** A unit whose attack is refused, that a cast reports with no valid target, or that the bot walks toward for 30 s
  without reaching, is avoided for 30 s, doubling on each repeat up to 2 minutes. A fight in which neither side's health goes down for 45 s
  (a target out of reach) is given up, and that target is avoided for 5 minutes, then 10, 20 and 40.
- **Approach and attack.** Melee classes close to 5 yards, ranged classes (hunter, priest, shaman, mage, warlock, druid) to 25. A ranged
  bot below 15% power walks in to melee. Below 30% health a bot disengages.
- **Rotation** (`src/combat_rotation.cpp`). Every spawned class has rank 1 rows; the core casts the highest rank known. A row is skipped
  until the bot reaches the level a trainer teaches it (`LearnLevel`, from the Forever trainer data). One cast per global cooldown, and none while the bot has any spell in progress (a Fireball in flight counts); a
  ranged row is used two yards inside its range, measured in three dimensions; a refused cast is retried after a while, an out-of-range
  one makes the bot stand closer for 5 s, and three line-of-sight refusals in a row drop the target. Damage-over-time spells and self buffs are not recast while their effect runs; a rogue counts its
  own combo points.
- **Rest.** An idle bot below 90% health sits and stays sat until healed, and stands before it moves or fights. An idle mana user below 40% mana
  sits too, until 90% (at most 2 minutes at a time, then not again for 5), and does not walk off while it drinks.
- **Grey creatures.** A creature far enough below the bot to give no experience (the classic zero-experience difference: 5 levels up to level 7,
  more above) is not pulled and does not keep an idle bot from walking on; one that attacks the bot, or that a quest wants, is still fought.
- **Vendor and travel.** An idle bot due a visit walks to the nearest vendor on its map (found by the core, since a bot sees only 40
  yards) and visits within 4 yards: sell grey items, repair, equip the best gear. A walk over 3 minutes is dropped until the next interval.
- **Hubs.** A bot idle for 30 s with nothing in view walks to the next hub of its map and waits a full interval there before walking on.
- **Talents.** At level 10 and up, out of combat, and again at each level and on a role change, the module sends `SpendBotTalents` with the
  bot's role (`Auto` gives a warrior in a party the tank role, a class with a heal spell the healer role, the rest dps). The core spends the
  points by its profile for the (class, role), when `Bot.Talents.Profiles` names a profile file; otherwise, or for a class with no profile,
  its greedy allocator. A refusal (in combat) is asked again after 5 s. Points already spent are not taken back, so a role change affects
  only new points (no respec yet). Protocol 17.
- **Loot.** A creature the bot fought, once dead and within 25 yards, is looted once when the bot is out of combat. The core runs the
  bot's own loot handlers, so loot rights, group rules and bag space are the core's. The item-looted event fires, so
  auto-vendor-trash sells a bot's grey drops.
- **Parties.** With `PartySize` set, the lowest-guid bots are cut into parties. The leader (the first warrior, else the lowest guid)
  invites; members accept, leave a foreign party, follow the leader when more than 8 yards away and fight the leader's target.

The spell ids are rank 1 ids from the classic spell tables. They were checked against the Forever trainer data: every trainable one has
its vanilla level. Sunder Armor (7386) has no trainer row there. The module logs once per spell id the core does not know.

## Telemetry

The pump writes one CSV line per event and one summary line per bot every `TelemetrySummarySec`, appended to `TelemetryFile`
(relative to the module's working directory). The header is written when the file is new. Each line carries the bot's running totals.

Columns: `time_ms`, `kind`, `bot`, `level`, `health_pct`, `power_pct`, `in_fight`, `fights`, `targets_gone`, `deaths`,
`casts_applied`, `refusals`, `stuck_episodes`, `longest_fight_ms`, `health_lost`, `detail`.

Kinds: `summary`, `fight_start`, `fight_end` (detail: reason, target, duration, health lost), `target_switch`, `death`, `alive`,
`level`, `stuck`, `unstuck`.

- `casts_applied` counts every applied action (move, follow, stop, rest and the rest included), not only spells.
- A fight starts when the bot's own attack target is set. `fight_end` reason `target_gone` means the target was not in view when the
  threat dropped (usually a kill, also a leash or a target that left); `no_threat` means it was still alive and in view.
- `stuck` is a fight with no change in health, position or target for 60 seconds.
- `time_ms` is the machine's monotonic clock: it starts again after a reboot, and the counters in each line start again when the module
  restarts. `tools/soak_report.py` splits the file into runs where the clock steps back or no line was written for 5 minutes, leaves the
  gaps out of the hours, and adds the counters of each stretch.
- Quest hand-ins: a hand-in the core refuses (bags full) sends the bot to the nearest vendor at once, even in the middle of quest work, and
  the hand-in is tried again after the visit.
- If the summary lines stop, the module has stopped.

Two tools read a telemetry file (both are Python 3, run with `-I`, and both read the rotated files beside it):

- `tools/soak_report.py --telemetry <csv> [--stderr <file>] [--server-log <file>] [--hours 5]`: one report over the whole file
  or the last hours, with the runs split up.
- `tools/snapshot_replay.py --telemetry <csv> [--stderr <file>]`: one section per open question — the bots still on level 1 and
  their last quest step, deaths per level, stuck episodes beside fights, the longest fight with the rows around it, applied casts
  against refusals with the reason from the stderr, and fights that ended with the target gone by level. It is a diagnosis aid, not
  a summary tool: run it on the runs of one soak, not on a mixed chain.

## Tests

`tests/*_test.cpp` are the unit tests; `tests/*_test.py` are the Python ones, run with `-I`. Beyond the per-library suites:

- `policy_test.cpp` covers the whole decision path: pulls, over-pull control, the class rotations, kiting, fleeing,
  mounting for long travel, parties, heals, resurrections, loot, hubs, quests, death recovery, and the coordination
  the policy does across bots (one interrupt per party, one heal per member, one party buff per member).
- `scale_test.cpp` runs `Decide` at the 1500-bot ceiling with 300 parties and a full nearby list, and checks that the
  tick stays bounded in time and that the actions published stay within the two per bot the core granted. Timing is
  printed; the ceiling is wide (override it with `FOREVER_BOTS_TICK_MS`) so a loaded machine does not fail a build.
- `tools/snapshot_replay.py` replays a telemetry run and answers the open questions of a soak; `soak_report.py`
  summarises a whole file.

## Build

    cmake -S . -B build -DFOREVER_MODULE_SDK_SOURCE=/path/to/forever-module-sdk
    cmake --build build
    ctest --test-dir build

Without `FOREVER_MODULE_SDK_SOURCE`, CMake fetches `Trixx34/forever-module-sdk` (`main`), which does not yet have layout 7.

## Layout

- `module.json`: manifest (name, protocol version, Tier 1 hooks).
- `src/bot_policy.{h,cpp}`: the decisions from snapshots (target, approach, rotation, rest, services, travel, hubs, loot, parties).
- `src/combat_rotation.{h,cpp}`: rotation rules per class, learn levels, facts from a snapshot.
- `src/bot_roster.{h,cpp}`: owned bots, pending requests, the cap. No SDK dependency.
- `src/bot_settings.{h,cpp}`: the module's config. `src/bot_pick.{h,cpp}`: valid race/class combinations and the seeded pick.
- `src/bot_ramp.{h,cpp}`: the ramp schedule. `src/bot_store.{h,cpp}`: the store row keys.
- `src/bot_telemetry.{h,cpp}`, `src/rotating_file.{h,cpp}`: the telemetry CSV and its rotating file.
- `src/dungeon_data.{h,cpp}`: the dungeon structures, the old text file (routes, bosses, entry) and its merge into the scenario config. `data/dungeons.txt`, `tools/make_dungeon.py`, `doc/dungeons.md`.
- `src/dungeon_files.{h,cpp}`, `src/mini_json.{h,cpp}`: the per-dungeon JSON files (`data/dungeons/<map id>-<name>.json`: entrance, route, events, a block per boss), the override folder `DungeonDir` and the reload. `tools/convert_dungeons.py`, `doc/dungeon-strategy.md`.
- `src/main.cpp`: handshake, config, control requests, revocations, the ramp timer and the Tier 2 pump thread.
- `tests/*_test.cpp`: unit tests for each library.

## Known gaps

- Quests and levelling are built from data files but not verified on the sim. Dungeons (M9) are a first slice: a data-driven route and a boss-clear rule on top of the scenario director; the sample Ragefire Chasm route is approximate, no run has been done on the sim, there is no crowd control, boss mechanics or walk to the portal (`doc/dungeons.md`).
- Layout 13 (protocol 7) is what the module is built against: the bot's auras, the pet, `casting_target`, vendor/repair/auctioneer guids, money and free bag slots, and `last_target_lost_reason`. A core that still runs layout 12 refuses the module at attach, so the core has to move first. What the module does with them is in `src/bot_policy.cpp` (`AuraRunning`, `PetOf`, the `kPetCommand` actions, the avoid of a leashed creature); a target's DOTs are still a coarse `aura_mask`, whose bit assignment the core has not published, so a DOT on a target is still estimated.
- The rotation spec uses the Forever abilities that could be verified in the client data (Holy Strike, Divine Storm, Chaos Bolt and others; `doc/rotations.md`). Abilities that need special handling or whose effect could not be verified (Demonic Brand, rogue finishers, forms, channels) are not used yet.
- A hunter's pet is handled by spell casts only (Call Pet 883, Revive Pet 982, Mend Pet 136, its state guessed from accepted and
  refused casts): the snapshot has no pet and the core has no pet command action, so the pet's health and presence are not seen and
  the pet is not sent to attack the bot's target (it acts on its own AI). See doc/rotations.md, Limits, for the core/SDK change
  needed. Auto Shot is tracked and not re-cast while active. Rend's bleed is estimated (21 s per accepted cast) because the
  snapshot has no auras. A fleeing target is not detected.
- Hub positions come from the sim's quest-giver clusters and may sit in rock; a bot that cannot reach one gives up after 3 minutes.
- Classes 6 and 10 are not spawned.
- **Not yet built on layout 13:** vendor buying (`client.BuyItem`, needs an item list to buy from and the Tier 1 action sent from the `Run()` thread), the auction house (`client.AuctionHouse` plus `PlayerScript.OnAuctionResult`), and crafting and gathering (`client.Craft`, `client.Gather`, which need recipe and node data). The SDK has all of it; the module does not use it yet.

## Quests (M5)

With `QuestFile` set (see `data/starter_quests.txt`), a bot that is near the anchor of one of the file's starts does the start's quests
in order: it walks to the giver and accepts the quest (core action `kQuest`), hunts the quest's targets (it pulls only those creatures,
walking between the quest's hunting areas when none is near, and still fights back when attacked), walks to the ender and hands the
quest in (the core picks the reward). A quest with no progress for 10 minutes is abandoned and left alone for an hour; a refused accept
or hand-in blocks that quest for a minute. Party members follow their leader and do no quests. The data format is documented in
`src/quest_data.h`; `tests/quest_test.cpp` covers it.

## Auto gear and scenarios

**Chat commands.** With `ChatCommands = 1` the leader of a group commands its bots by typing in party or raid chat, or by whispering one bot:
`[selector] verb`. Selectors: `all` (default), `tank`, `healer`, `dps`, a class name, or a comma list. Verbs: `follow [off]`, `stay [off]`, `goto here`,
`release` (a dead bot releases its spirit now), `status` (one line per bot, sent back to you). `ChatOrders = 1` adds `stop`, `pull` (the tank
attacks your selected target), `heal` (a healer heals your selected friend), `passive`, `aggressive`, `summon` (the bots come to you), `revive`
(a dead bot is resurrected at 35% health and brought to you), `mount` and `dismount` (summon, revive and mount also need `Bot.Chat.Orders.Enabled = 1` in
the core). `ChatControl = 1` adds the control panel: `role tank|healer|dps|auto`, `stance aggressive|defensive|passive`, `focus [off]` (your selected
target is attacked first in a fight), `distance <yards>|default` (how far from you a following bot stops). Only the bots of the speaker's own group obey;
other chat is ignored. The core forwards only a group leader's lines in a group with a bot, and whispers to a bot (hook `PlayerScript.OnChat`).
A bot accepts a party invite from a player when `ChatCommands = 1`, so the player invites bots with the normal /invite and then commands them.
With `ChatSay = 1` a bot that takes an order says a short line out loud (`On my way.`, `Holding here.`), at most `ChatSayMax` bots per order so a big group does not
drown the chat; a bot you whisper always answers. Refused orders and `status` (which already whispers) get no line.

**Supplies.** With `Supplies = 1` the core tells the module what each bot carries (ammunition for its ranged weapon, food, drink) and where the nearest vendor is
that sells what the bot can use. A bot with fewer than `SupplyAmmoLow` pieces of ammunition (a hunter, or anyone with a bow, crossbow or gun), or fewer than
`SupplyFoodLow` food or `SupplyDrinkLow` drink, and with some money and a free bag slot, walks to that vendor when it is idle and buys up to the target; the core chooses
the items from the vendor's stock (ammunition of the right kind, the best food or drink the bot may use at its level). Worn gear (the repairman) comes first. A trip is
judged a few seconds after the purchase: if the bot carries no more than before, the next trip waits 10 minutes, doubling per failure in a row. A resting bot below 70%
health eats, one below 40% mana drinks (`kConsume`). A hunter that carries no ammunition does not shoot (the core would refuse it) and fights in melee until it has some.
More dot commands (they need the module-command permission): `.bots spawn <race id> <class id> [level]`, `.bots list`, and alts: `.bots alt add <name>`
logs a character of your own account in as a bot (the core needs `Bot.Alt.Enabled = 1`; you must be playing another character, the alt must be
offline), `.bots alt remove <name>` logs it out, `.bots alt list`. An alt is your own character: it gets no auto gear, no equip service and no free
trainer spells or talents, and it cannot be played while it is a bot. Alts are not remembered across a restart.
`.bots <verb>` does the same as a dot command (`.bots autogear` dresses the group's bots); `goto here`, `pull` and `heal` are party chat only.

**Whole-world quests.** A quest file made for the whole world (`tools/make_world_quests.py`, `doc/world-quests.md`) lets bots level by quests from 1 to 60: they take quests that suit their level, race and class from the nearest area that has some, and walk on to the next area when it is used up.

**Rotations.** With `RotationFile` set, the classes use rows from a file (levels 1 to 60, damage-over-time lengths, and upkeep buffs such as Power Word: Fortitude, armor spells and Battle Shout cast out of combat, on the bot or its whole group). The file is made from a spec of spell names and the core's ability dump by `tools/make_rotations.py`; see `doc/rotations.md`.

**Grind spots.** With `GrindFile` set, an idle bot walks to a place that suits its level (nearest first, not back to the last two) instead of the hubs of its map. The file is made from the world database with `tools/make_grind_spots.py`; see `doc/grind-spots.md`.

**Auto gear.** With `AutoGearing = 1` the core dresses each bot for its class and level (best usable item per slot, uncommon quality at
most) when it first appears and after every `AutoGearEveryLevels` levels. With it off, a player who leads a party can type `.autogear`
(a chat command the module claims) and the bots that player leads are dressed. It gears bots only, not the player's own characters.

**Scenarios.** A scenario is a repeatable test situation: `Scenario = <name>` with a roster, a staging place, an entry, a route and a
duration (see `config/modules.d/forever-bots.conf.example`). The module spawns the roster at the staging place, dresses it (auto gear,
or the class's `ScenarioGear<class>` list), waits for the party to form, teleports the leader to the entry and then the members, and
lets the leader walk the route (it waits for members more than 30 yards behind and while its health is under 80%). One result line is
written when it ends: `scenario <name>: outcome=<complete|wipe|timeout|enter_failed> furthest=3/7 deaths=2 alive=3/5 run=412s`.
`tests/scenario_test.cpp` covers the director, the route and the settings.

**Dungeon runs (M9).** `DungeonFile = data/dungeons.txt` and `Dungeon = rfc` run a dungeon as a scenario: the file gives the roster (tank first, healer, three dps), the door and the entry, the boss list and the route, and `ScenarioRequireBosses` makes the run complete when every boss has been seen dead (`bosses_missing` if the leader reaches the end without). Keys you set yourself win over the file. The tank leads and pulls pack by pack, the healer heals, rest and corpse runs are the existing party and scenario logic. First slice, unit-tested only; the Ragefire Chasm route in the sample file is approximate. See `doc/dungeons.md`. **Since 0.19.0 each dungeon is one JSON file** (`data/dungeons/<map id>-<name>.json`) that you can edit, or replace from your own folder (`DungeonDir`) without touching the shipped files; a file with a mistake is logged and not used, and a config reload reads the files again. Format: `doc/dungeon-strategy.md`.
