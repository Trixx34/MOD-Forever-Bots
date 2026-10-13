# MOD-Forever-Bots

Bot control as a ForeverBots module. The core creates and controls bots; this module decides what they do.

## Status

M2 (combat, in progress): the policy approaches a target beyond 5 yards, attacks within it, and casts the class rotation
once per global cooldown. The warrior rotation (`src/combat_rotation.cpp`) is ported from the forever branch's rules.
Refused casts are retried after a short wait. Not yet verified on a sim server.

Earlier status:

The module connects to the core, asks for one bot with `RequestControl`, tracks the bots it owns, and logs
revocations. It requests Tier 2 and drives its bots from snapshots: a pump thread reads each new snapshot and
publishes actions from `BotPolicy`. The policy is a placeholder: below 30% health a bot disengages (follows the closest
player, or stands still); otherwise it fights its threat target or the closest hostile, else follows the closest player,
else stands still. A dead bot gets no orders.

## Stored state

The bots the module controls are rows in the core's module store (doc 15): `bot/<guid>`. On start the module lists
these rows and takes its bots back first, up to `MaxBots`, spawning only for the rest. A row is removed only when the core
says the character does not exist, not on a revoke, because the core revokes every bot at shutdown.

## Configuration

The core sends the module the `Module.forever-bots.<Key>` entries from `worldserver.conf.d/modules.d/forever-bots.conf`
(under the config directory), with the prefix removed. See `config/modules.d/forever-bots.conf.example`:

- `MaxBots` (1..16, default 1): bots the module controls at once.
- `Race` and `Class` (default 1 and 1): the race and class to spawn bots with.

Invalid values fall back to the defaults, with a line in the module's log. The bots spawned are capped at the core's
Tier 2 grant, and none are requested when there is no Tier 2 region.

## Build

    cmake -S . -B build -DFOREVER_MODULE_SDK_SOURCE=/path/to/forever-module-sdk
    cmake --build build
    ctest --test-dir build

Without `FOREVER_MODULE_SDK_SOURCE`, CMake fetches `Trixx34/forever-module-sdk` (`main`).

## Layout

- `module.json`: manifest (name, protocol version, Tier 1 hooks).
- `src/bot_roster.{h,cpp}`: owned bots, pending requests, the max-bots cap. No SDK dependency.
- `src/bot_policy.{h,cpp}`: decides actions from snapshots: approach, attack, rotation casts, retries, disengage.
- `src/combat_rotation.{h,cpp}`: rotation rules (ported from the forever branch), warrior rows, facts from a snapshot.
- `src/bot_telemetry.{h,cpp}`: the CSV record of what each bot did (see Telemetry below). No SDK dependency beyond snapshot types.
- `src/bot_settings.{h,cpp}`: reads the module's config (MaxBots, Race, Class, RandomSeed, the Bots* ramp keys, TestSpell, telemetry keys).
- `src/bot_pick.{h,cpp}`, `src/bot_ramp.{h,cpp}`, `src/rotating_file.{h,cpp}`: the random pick, the ramp schedule, and the rotating telemetry file.
- `src/bot_store.{h,cpp}`: the store row keys (`bot/<guid>`) for the bots the module controls.
- `src/main.cpp`: handshake, config, control requests, revocations, and the Tier 2 pump thread.
- `tests/*_test.cpp`: unit tests for each library (roster, policy, rotation, settings, bot store, telemetry).

## Scale run (up to 1500 bots)

- `Race` and `Class` accept `random`: a seeded pick over the Forever valid combinations (`src/bot_pick.h`, from the Wowhead
  Forever grid, patch 1.60.1). The core validates each pick (`Player::Create`), so a stale entry costs one refused spawn.
- The ramp (`src/bot_ramp.h`) asks for `BotsInitial` bots, adds `BotsStep` every `BotsStepMinutes`, never above `BotsCap`,
  and stops adding at `BotsStopTime` local time. It runs on a 5 s SDK timer on the `Run()` thread, and spawns at most 10 per
  tick. A refused spawn waits 30 s.
- A config reload updates the ramp, the random picks and the cap. No bot is logged out.
- Telemetry rotates at `TelemetryMaxMB`, keeping `TelemetryKeep` older files (`src/rotating_file.h`).
- The pump logs its decide time and actions per tick once a minute.

## Class rotations

Every class the module spawns has a rotation (`src/combat_rotation.cpp`): warrior, paladin, hunter, rogue, priest, shaman,
mage, warlock and druid. Rows are rank 1 spell ids; the core casts the highest rank known. Ranged classes (hunter, priest,
shaman, mage, warlock, druid) approach to 25 yards instead of 5. Damage-over-time spells are not recast while the effect
runs on the same target, and self buffs (Seal of Righteousness, Power Word: Shield) are not recast while up
(`DotDurationMs` in `bot_policy.cpp`). A rogue counts its own combo points: each applied Sinister Strike on the target adds
one, and Eviscerate fires at five. A ranged bot below 15% power walks to melee range.

The spell ids and durations are from memory of the classic spell tables. The module logs once per spell id the core does
not know ("is not in the core's spell table"), so a wrong id shows up in the sim log. Auto Shot (75) is the hunter's last
row and its repeated casts are not yet checked on the sim.

## Milestones: M2 to M4

- M2 combat: rotation, approach, over-pull control, cooldown skips, Rend tracking, and rest (sit when idle and below 90%
  health, stand before anything moves or fights). Flee is the 30% disengage: the snapshot has no fleeing state, so a
  fleeing target is not detected.
- M3 vendor: `kVendor` (spell_id bits: 1 sell grey items, 2 repair, 4 equip the best gear by item level). Runs on a timer
  for an idle bot, when an NPC with the vendor or repair flag is in interaction range.
- M4 trainers: `kTrain` learns the trainer's spells the bot can take now, lowest level first, through the core's teach
  checks. Runs on a timer and at once after a level-up, when a trainer is in interaction range.
- Services are opt-in in the policy (zero interval is off). The module turns them on from `ServiceEverySec` and
  `TrainEverySec`. A refused service is logged at most once per bot every ten minutes.
- M8 parties: `kGroup` invites, accepts and leaves (spell_id 1, 2, 3; layout 5). With `PartySize` set, the first
  `PartyCount` parties of `PartySize` bots (lowest guids first) form: the leader (its first warrior, else its lowest-guid
  bot) invites each member not in the party, members accept, leave a foreign party, follow the leader when more than 8
  yards away and fight the leader's target when they have none. Party actions run only out of combat. Off by default.
- M6 travel: an idle bot due a vendor visit walks to the nearest vendor on its own map (`kMoveTo`), and visits once within
  4 yards. The core finds the vendor from the creature spawns and reports it in the snapshot (layout 6), since a bot
  only sees units within 40 yards. The lookup repeats after the bot moves 100 yards. A walk longer than 3 minutes is
  dropped until the next interval. A walking bot neither sits nor trains. Party members do not walk alone; they follow
  their leader. Trainers are not travelled to: bots learn their spells on level-up.

## Telemetry (overnight runs)

The pump writes one CSV line per event and one summary line per bot every `TelemetrySummarySec` seconds, appended to
`TelemetryFile` (relative to the module's working directory, default `forever-bots-telemetry.csv`). The header is written when the
file is new. Each line is self-contained: it carries the bot's running totals.

Columns: `time_ms` (module clock), `kind`, `bot` (guid), `level`, `health_pct`, `power_pct`, `in_fight`, `fights`,
`targets_gone`, `deaths`, `casts_applied`, `refusals`, `stuck_episodes`, `longest_fight_ms`, `health_lost`, `detail`.

Kinds: `summary`, `fight_start`, `fight_end` (detail: reason, target, duration, health lost), `target_switch`,
`death`, `alive`, `level`, `stuck`, `unstuck`.

- `fight_end` reason `target_gone` means the target was not in view when the threat dropped: usually a kill, but also a
  leash or a target that left. `no_threat` means the target was still alive and in view.
- `stuck` means a fight had no change in health, position or target for 60 seconds (`StuckAfterMs`).
- If the summary lines stop, the module has stopped.

## Open items

- Resting is not implemented: the protocol has no action to sit or regenerate.
- Rend's bleed is estimated (21 seconds per accepted cast) because the snapshot has no auras.
- The fleeing condition in the rotation is always false; the snapshot does not report it.
- Not yet tested against a running worldserver.
