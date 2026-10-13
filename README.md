# MOD-Forever-Bots

Bot AI as a ForeverBots module. The core creates, logs in and moves the bots; this module decides what each bot does: it reads a
snapshot of every bot each tick and publishes actions (move, follow, attack, cast, rest, vendor, party, loot). Goal: bots that level
from 1 to 60 and, in a party of five, clear a dungeon.

## Status

Built and unit-tested (9 test suites). Run on a dedicated sim worldserver with 300, 500 and 1000 bots: spawn and login, the ramp,
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
| M5 quests 1 to 10, levelling 1 to 60, M9 dungeon | not started |

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
  without reaching, is avoided for 30 s, doubling on each repeat up to 2 minutes.
- **Approach and attack.** Melee classes close to 5 yards, ranged classes (hunter, priest, shaman, mage, warlock, druid) to 25. A ranged
  bot below 15% power walks in to melee. Below 30% health a bot disengages.
- **Rotation** (`src/combat_rotation.cpp`). Every spawned class has rank 1 rows; the core casts the highest rank known. A row is skipped
  until the bot reaches the level a trainer teaches it (`LearnLevel`, from the Forever trainer data). One cast per global cooldown; a
  refused cast is retried after a while. Damage-over-time spells and self buffs are not recast while their effect runs; a rogue counts its
  own combo points.
- **Rest.** An idle bot below 90% health sits and stays sat until healed, and stands before it moves or fights.
- **Vendor and travel.** An idle bot due a visit walks to the nearest vendor on its map (found by the core, since a bot sees only 40
  yards) and visits within 4 yards: sell grey items, repair, equip the best gear. A walk over 3 minutes is dropped until the next interval.
- **Hubs.** A bot idle for 30 s with nothing in view walks to the next hub of its map and waits a full interval there before walking on.
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
- If the summary lines stop, the module has stopped.

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
- `src/main.cpp`: handshake, config, control requests, revocations, the ramp timer and the Tier 2 pump thread.
- `tests/*_test.cpp`: unit tests for each library.

## Known gaps

- No quest, levelling or dungeon logic yet (M5, M7 levelling, M9). Bots fight what they meet and wander between hubs.
- The rotations follow the vanilla kits; Forever's changed abilities (for example Holy Strike, Demonic Brand) are not used yet.
- A hunter has no pet handling, and Auto Shot's repeated casts are unchecked. Rend's bleed is estimated (21 s per accepted cast)
  because the snapshot has no auras. A fleeing target is not detected.
- Hub positions come from the sim's quest-giver clusters and may sit in rock; a bot that cannot reach one gives up after 3 minutes.
- Classes 6 and 10 are not spawned.

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
attacks your selected target), `heal` (a healer heals your selected friend), `passive`, `aggressive`. Only the bots of the speaker's own group obey;
other chat is ignored. The core forwards only a group leader's lines in a group with a bot, and whispers to a bot (hook `PlayerScript.OnChat`).
`.bots <verb>` does the same as a dot command (`.bots autogear` dresses the group's bots); `goto here`, `pull` and `heal` are party chat only.

**Auto gear.** With `AutoGearing = 1` the core dresses each bot for its class and level (best usable item per slot, uncommon quality at
most) when it first appears and after every `AutoGearEveryLevels` levels. With it off, a player who leads a party can type `.autogear`
(a chat command the module claims) and the bots that player leads are dressed. It gears bots only, not the player's own characters.

**Scenarios.** A scenario is a repeatable test situation: `Scenario = <name>` with a roster, a staging place, an entry, a route and a
duration (see `config/modules.d/forever-bots.conf.example`). The module spawns the roster at the staging place, dresses it (auto gear,
or the class's `ScenarioGear<class>` list), waits for the party to form, teleports the leader to the entry and then the members, and
lets the leader walk the route (it waits for members more than 30 yards behind and while its health is under 80%). One result line is
written when it ends: `scenario <name>: outcome=<complete|wipe|timeout|enter_failed> furthest=3/7 deaths=2 alive=3/5 run=412s`.
`tests/scenario_test.cpp` covers the director, the route and the settings.
