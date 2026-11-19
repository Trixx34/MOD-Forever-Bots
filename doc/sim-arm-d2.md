# Sim arm D2: A=B calibration, survival A/B, scheduler A/B (2026-11-19)

Branch `integrate/arm-d2-20261119` = `integrate/arm-d-20261119` (04a49c1, keys in `doc/sim-arm-d.md`) + three slices, in this order:

1. `feature/sim-ab-harness-20261119` (ArmSplit, ArmBOverrides, TraceBots, `tools/sim_readout.sh`; `doc/sim-ab.md`)
2. `feature/decide-scheduler-20261119` (DecideScheduler and co; `doc/decide-scheduler.md`)
3. `feature/survival-20261119` (SafeRevive, FightRiskRetreat, DangerMap; `doc/survival.md`)

Every new key defaults off (0 or empty) except `LevelSafeLogThrottleSec` 60, `TraceMaxMB` 20 and the scheduler tiers `DecideTierWalk` 3,
`DecideTierIdle` 6, `DecideMaxSkipTicks` 12 (read only with `DecideScheduler = 1`). Nothing here is for prod; prod gets every key with its default
(add-all-keys rule, `doc/sim-arm-d.md`, `doc/sim-ab.md`, `doc/decide-scheduler.md`, `doc/survival.md`).

## What can be A/B'd in one run (per arm) and what cannot

Per arm (`ArmBOverrides` / `Arm.B.<Key>`; read through `Opt()` in the bot's own turn, `BotPolicy::DecideBot`):

- every arm D key that acts in a bot's turn: `DeathLoopBreaker`, `LevelSafe*`, `BelowMap*`, `SkyborneExit*`, and the rest of the policy options;
- the survival keys: `SafeRevive`, `SafeReviveLevelMargin`, `FightRiskRetreat`, `FightRiskMarginPct`, `FightRiskTeleport`, `DangerMap`;
- the scheduler keys: `DecideScheduler`, `DecideBudgetUs`, `DecideTierWalk`, `DecideTierIdle`, `DecideMaxSkipTicks`. With `ArmSplit = 1` each arm has
  its own scheduler over its own bots (arm A's over the even guids, arm B's over the odd ones), with its own keys and its own budget. An arm with
  `DecideScheduler = 0` runs its bots every tick. With the scheduler on in either arm, arm A's bots take their turns before arm B's in a tick (with
  both off the order is the view order, as before).

Not per arm (one value for the process):

- the danger map itself: one map for both arms. While `DangerMap` or `SafeRevive` is on in either arm, every open-world death of either arm goes into
  it (arm B sees the map it would see with the keys on for all bots); only the bots of the arm with the key on act on it. `DangerMapDeathsPerHour`
  and `DangerMapDecayMin` are the map's: arm A's when arm A uses the map, arm B's when only arm B does (the A/B below: arm B's, which are the
  defaults 6 and 120).
- `DecideBudgetUs` is per arm but counts only that arm's turns (about half the bots): a budget meant for all bots is halved for one arm.
- the shared steps and process keys listed in `doc/sim-ab.md` (parties, level dungeons, LFG, guilds, mail, chat, `MaxBots`, `Bots*`, `Telemetry*`,
  `Presence*`, `Auction*`, `Errand*`, `WorldEvents*`, `AmbientChat*`, `LevelDungeon*`, the trace keys).
- core keys (`Bot.Actions.Teleport`, `Bot.Actions.TeleportInCombat`): the core applies them to every bot. With `TeleportInCombat = 1` only the bots
  that send a teleport in combat are affected; in arm A nothing does (FightRiskTeleport off), so the key is safe for both arms. It also lets an
  in-combat below-map recovery through in both arms (equal for both).

Events: every event line that names a bot carries ` arm=A|B` (the survival lines `safe_revive`, `fight_retreat`, `danger_cell` (the dying bot's arm),
`quest_walk_blocked reason=danger` included). The `pump:` line gets the scheduler tail only without the split; with the split each
`pump_arm: arm=A|B ...` line ends with that arm's tail (` sched=on walk= idle= budget_us= tiers=... skipped= deferred= forced= starved= overruns=
max_turn_us= max_tick_us=`, nothing when that arm's scheduler is off). `deaths_top` (without `arm=`) gets the `danger_*` fields when either arm
uses the map.

## Drop-ins

All in `/srv/forever/sim/conf/worldserver.conf.d/modules.d/`, outside a sim run (overnight push freeze), sim restart after. Arm D's `zz-arm-d.conf`
(the drop-in text in `doc/sim-arm-d.md`) stays as it is in every run below: it is the base both arms read. One key in one file only: the core refuses a
duplicate key name (the 2026-11-12 crash loop). If the 2026-11-15 add-all-keys drop-in or a later one already has any of the keys below (`ArmSplit`,
`ArmBOverrides`, `Trace*`, `Decide*`, `SafeRevive*`, `FightRisk*`, `DangerMap*`), change them there instead or take them out there first.
Use one `zz-arm-d2.conf` and replace its content between the runs.

### (1) A=B calibration, 2 h (first)

Both arms the arm D config; the survival and scheduler keys off in both; no trace.

```
# zz-arm-d2.conf: A=B calibration (arm D keys from zz-arm-d.conf apply to both arms)
Module.forever-bots.ArmSplit = 1
Module.forever-bots.ArmBOverrides = ""
Module.forever-bots.TraceBots = ""
Module.forever-bots.TraceDir = ""
Module.forever-bots.TraceMaxMB = 20
Module.forever-bots.SafeRevive = 0
Module.forever-bots.SafeReviveLevelMargin = 3
Module.forever-bots.FightRiskRetreat = 0
Module.forever-bots.FightRiskMarginPct = 100
Module.forever-bots.FightRiskTeleport = 0
Module.forever-bots.DangerMap = 0
Module.forever-bots.DangerMapDeathsPerHour = 6
Module.forever-bots.DangerMapDecayMin = 120
Module.forever-bots.DecideScheduler = 0
Module.forever-bots.DecideBudgetUs = 0
Module.forever-bots.DecideTierWalk = 3
Module.forever-bots.DecideTierIdle = 6
Module.forever-bots.DecideMaxSkipTicks = 12
```

No `Module.forever-bots.Arm.B.*` key in any drop-in. Check the start-up line: `arm split on: even guids are arm A, odd guids arm B; arm B overrides:
none (an A=B run)`. Run 2 h after the ramp is done; pass when arm A and arm B are within 10% on `deaths_per_bot_hour` and `levels_per_bot_hour`
(`doc/sim-ab.md`, "A=B calibration"). Write the gap down: the real A/B below must beat it.

### (2) Real A/B: survival (8 h, overnight)

Arm A = arm D (the base, survival off); arm B = arm D plus the survival keys with the rescue teleport. Only the two lines differ from (1):

```
# zz-arm-d2.conf: survival A/B (arm A = arm D, arm B = arm D + survival)
Module.forever-bots.ArmSplit = 1
Module.forever-bots.ArmBOverrides = "SafeRevive=1,FightRiskRetreat=1,DangerMap=1,FightRiskTeleport=1"
Module.forever-bots.TraceBots = ""
Module.forever-bots.TraceDir = ""
Module.forever-bots.TraceMaxMB = 20
Module.forever-bots.SafeRevive = 0
Module.forever-bots.SafeReviveLevelMargin = 3
Module.forever-bots.FightRiskRetreat = 0
Module.forever-bots.FightRiskMarginPct = 100
Module.forever-bots.FightRiskTeleport = 0
Module.forever-bots.DangerMap = 0
Module.forever-bots.DangerMapDeathsPerHour = 6
Module.forever-bots.DangerMapDecayMin = 120
Module.forever-bots.DecideScheduler = 0
Module.forever-bots.DecideBudgetUs = 0
Module.forever-bots.DecideTierWalk = 3
Module.forever-bots.DecideTierIdle = 6
Module.forever-bots.DecideMaxSkipTicks = 12
```

Core key (core config, not a module key; one file only): `Bot.Actions.TeleportInCombat = 1`. It needs the core with
`feature/bot-combat-rescue-20261119` (Trinity-Forever-Modules) on the sim; without it the rescue teleport is refused and logged as
`fight_retreat stage=teleport_refused` (the walk away still runs), so the A/B then tests SafeRevive, the retreat walk and the danger map only. Keep
`Bot.Actions.Teleport = 1` (arm D needs it). Prod: neither changes.

Start-up line to check: `arm B overrides: SafeRevive=1 (A: 0), FightRiskRetreat=1 (A: 0), DangerMap=1 (A: 0), FightRiskTeleport=1 (A: 0)`.
What to count per arm: `doc/survival.md`, "Sim test" (deaths per bot-hour against the calibration gap, top-10 share, repeat-death cells,
`fight_retreat` per `stage=`, `safe_revive` per `stage=`, `danger_cell`, `quest_walk_blocked reason=danger`, levels per bot-hour not lower).

### (3) Real A/B: decide scheduler (separate run, after (2))

Arm A = arm D (every bot every tick); arm B = arm D with the scheduler. Survival off in both (one feature per run). The budget is arm B's own and covers
only arm B's turns (about half the bots), so it is half the 12000 us of `doc/decide-scheduler.md`:

```
# zz-arm-d2.conf: scheduler A/B (arm A = arm D, arm B = arm D + DecideScheduler)
Module.forever-bots.ArmSplit = 1
Module.forever-bots.ArmBOverrides = "DecideScheduler=1,DecideBudgetUs=6000"
Module.forever-bots.TraceBots = ""
Module.forever-bots.TraceDir = ""
Module.forever-bots.TraceMaxMB = 20
Module.forever-bots.SafeRevive = 0
Module.forever-bots.SafeReviveLevelMargin = 3
Module.forever-bots.FightRiskRetreat = 0
Module.forever-bots.FightRiskMarginPct = 100
Module.forever-bots.FightRiskTeleport = 0
Module.forever-bots.DangerMap = 0
Module.forever-bots.DangerMapDeathsPerHour = 6
Module.forever-bots.DangerMapDecayMin = 120
Module.forever-bots.DecideScheduler = 0
Module.forever-bots.DecideBudgetUs = 0
Module.forever-bots.DecideTierWalk = 3
Module.forever-bots.DecideTierIdle = 6
Module.forever-bots.DecideMaxSkipTicks = 12
```

`WalkDecideEvery` stays the same for both arms (it is not overridden). Read: the `pump_arm` lines per arm (`per_bot_us`, `turn_avg_us`, arm B's `sched=`
tail: tiers seen/run, `deferred`, `starved` near 0, `overruns`, `max_turn_us`), and deaths, levels and `no_route` per bot-hour per arm (arm B within 5%
of arm A, `doc/decide-scheduler.md` "Sim plan" step 4). The `pump:` decide average is both arms together: the saving there is about half the full one.
The 400 to 800 bot ramp of that plan can run in the same configuration once this run is read.

## (4) The sim dungeon folder fix

Unchanged from `doc/sim-arm-d.md` ("The sim dungeon folder warning"): in `zz-sim-new-keys.conf` (dated `.bak-<date>` copy first, outside a sim run)

```
# was: Module.forever-bots.DungeonDir = /srv/forever/sim/dungeons-mine   (the example path; the folder does not exist)
Module.forever-bots.DungeonDir =
Module.forever-bots.DungeonDataDir = /srv/forever/sim/modules_runtime/forever-bots/data/dungeons
```

(`DungeonDataDir` only if no sim drop-in sets it or `DungeonFile` yet; one file only.) Check after the restart: no `dungeon folder ... cannot be read`
line, and `forever-bots: dungeons: N from /srv/forever/sim/modules_runtime/forever-bots/data/dungeons` and `tank lead knows N dungeon(s)` with N > 0.
Do it before (1), so all three runs have the same dungeons.

## (5) Read-out

From the ops host, in a checkout of this branch (the script only reads: journal, `SELECT` on characters, `free`, `df`):

```
ssh -o BatchMode=yes forever-test 'sudo -n bash -s -- "<start>" "<end>"' < tools/sim_readout.sh
```

`<start>` and `<end>` as `journalctl` takes them (`"2026-11-20 06:30" "2026-11-20 08:30"`): the start once the ramp is done, the end 2 h later for
(1), 8 h for (2). Output per arm and per hour; judge (2) and (3) against the A=B gap of (1).

## Not verified

- Only unit tests ran (all ctest targets, local build against the SDK at `origin/test`); nothing ran against the core or the sim.
- Which sim drop-ins hold which of the keys above today (no host access in this slice): check for duplicates before adding `zz-arm-d2.conf`.
- Whether the sim core has `Bot.Actions.TeleportInCombat` (core branch `feature/bot-combat-rescue-20261119`).
