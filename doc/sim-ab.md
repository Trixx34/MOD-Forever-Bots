# Sim A/B in one run (ArmSplit), bot traces and the read-out

Arms A, B and C of November 2026 ran one after another, so every comparison mixed the change under test with the drift between runs (other bots,
other levels, another config, another day). `ArmSplit` runs both configs in the same run on the same server: half the bots are arm A, half arm B.
`TraceBots` writes everything about a few bots to their own files, so a looper's whole death chain is in one place. `tools/sim_readout.sh` reads
the run back, per arm and per hour.

All of it is for the sim. The keys are off by default and stay off on test and prod.

## Keys

| Key | Default | What it does |
|---|---|---|
| `ArmSplit` | 0 | 1 = the bots with an odd guid are arm B, the even ones arm A (or per stratum, `ArmSplitMode`) |
| `ArmSplitMode` | guid | `guid` = the parity split above; `strata` = balanced per stratum (Skyborne or race, crossed with class), see below |
| `ArmBOverrides` | "" | Arm B's changes, `Key=Value,Key=Value` (for example `PathVariety=1,LevelSafeWalks=0`) |
| `Arm.B.<Key>` | unset | One override per key (`Module.forever-bots.Arm.B.PathVariety = 1`); wins over the same key in `ArmBOverrides` |
| `TraceBots` | "" | Bot guids to trace, comma list (`235,66`) |
| `TraceDir` | "" | Folder of the trace files (made when missing); empty = no trace |
| `TraceMaxMB` | 20 | Most disk per traced bot (1..1024): the file rotates at half of it, one old file is kept |

### Values per environment (add-all-keys rule)

Every environment gets the keys with explicit values once a build with them is deployed there (sim edits outside a sim run, prod in an owner
window):

```
# sim (until a calibration or A/B run sets them)        # test and prod
Module.forever-bots.ArmSplit = 0                        Module.forever-bots.ArmSplit = 0
Module.forever-bots.ArmSplitMode = guid                 Module.forever-bots.ArmSplitMode = guid
Module.forever-bots.ArmBOverrides = ""                  Module.forever-bots.ArmBOverrides = ""
Module.forever-bots.TraceBots = ""                      Module.forever-bots.TraceBots = ""
Module.forever-bots.TraceDir = ""                       Module.forever-bots.TraceDir = ""
Module.forever-bots.TraceMaxMB = 20                     Module.forever-bots.TraceMaxMB = 20
```

`Arm.B.<Key>` keys are written only for a run that uses them.

### What arm B changes, and what it does not

- Arm B reads the same config with its overrides applied, parsed by the same `ParseBotSettings`, so any key the module knows can be overridden
  with the same ranges and warnings (a bad arm B value warns as `arm B: Module.forever-bots.<Key>: ...`).
- The override takes effect in the bot's own turn (`BotPolicy::DecideBot`, everything from the last action's result to the rotation): death and
  ghost handling, level-safe walks and yards, the death-loop breaker, pulls, targets, quests, grind spots, path variety, supplies, rest, vendors.
- The steps shared by all bots run with arm A's values: party building and dynamic parties, the party planner, level dungeons and LFG, bot
  guilds, mail, chat answers. So do the process-level keys (`MaxBots`, `Bots*` ramp, `Telemetry*`, `Presence*`, `Auction*`, `Errand*`,
  `WorldEvents*`, `AmbientChat*`) and the tables the policy builds once (the quest planners per side, the known world, the level map). Do not
  A/B those with `ArmSplit`; use two runs.
- The split's own keys (`Arm*`, `Trace*`) cannot be overridden (warning).
- A scenario run (`Scenario = ...`) turns the split off with a warning.
- The data files arm B names the same as arm A are loaded once more at start and then shared (the arm B table lines in the log end with
  `arm B ready`); a different file (another `GrindFile`, for example) is arm B's own.

### What the split adds to the log

- Every event line that names a bot ends with ` arm=A` or ` arm=B` (`forever-bots: event died: bot=235 ... arm=B`), the module's
  `not done` lines too. With `ArmSplit = 0` nothing changes.
- After each `deaths_top:` line come two more, `deaths_top: arm=A window_min=...` and `deaths_top: arm=B ...`, with the same fields.
- After each `pump:` line come `pump_arm: arm=A bots=N turn_avg_us=T per_bot_us=P ticks=K` and the same for B: the bots in view (average per
  tick) and the time of their own turns. The shared steps are only in the `pump:` line. With `DecideScheduler` on in an arm, that arm's
  scheduler tail (` sched=on ...`) ends its `pump_arm` line (the scheduler is per arm, `doc/decide-scheduler.md`).
- Always (also without the split): `died` carries `level=` after `in_combat=`, and a bot seen at a higher level than at its last turn writes
  `level_up: bot= level= from= map=`. Both are for the read-out.

Parity is not random: guid order follows creation order. Bots made by the ramp get a random race and class, so both arms should get the same mix
in a large run, but the 2026-11-20 A=B calibration (200 bots per arm, parity) showed they do not: arm A 3.88 deaths per bot-hour, arm B 3.40 (a
12% gap, the pass rule is 10%), and 1,670 Skyborne (map 2991) deaths in arm A against 1,049 in arm B. Use `ArmSplitMode = strata` for
calibrations and A/B runs; check the "levels now ... classes" section of the read-out (bots per class per arm) before trusting a result.

### ArmSplitMode = strata

Each bot gets its arm the first time the policy sees it (`ab_split::Strata`, at the top of `BotPolicy::Decide`):

- The stratum is the race group crossed with the class. Skyborne (races 95 and 96) is one group, every other race its own; a bot whose race or
  class is not known yet (0) falls in a stratum of its own.
- The bots seen first in one tick are one batch, sorted by stratum and guid, so the login order inside the batch does not matter. In a stratum
  that leans one way the next bot goes to the other arm, the pairs go A then B, and the one left over of each stratum goes to the arm with fewer
  bots overall. Every stratum stays within one bot of 50/50; the bots of one batch also overall (with bots coming in over many batches each
  stratum still does, overall within a bot or two).
- The arm is kept for the life of the process: a bot that goes offline and comes back keeps its arm. A restart assigns afresh.
- Everything that reads the arm (arm B's options in the bot's turn, the scheduler per arm, ` arm=A|B`, `deaths_top` and `pump_arm` per arm,
  `tools/sim_readout.sh`) reads the assigned arm. An event line from another thread about a bot the policy has not seen yet gets its parity
  arm (rare: the policy sees a bot in its first tick online).
- The start-up line reads `arm split on: strata, each (Skyborne|race) x class stratum alternates A/B at first sight; arm B overrides: ...`.

## Tracing a bot

`TraceBots = "235,66"` and `TraceDir = "/var/log/forever-sim/bot-trace"` write `bot-235.trace` and `bot-66.trace`, each line with a local
timestamp (milliseconds):

- `event ...`: every policy event line of the bot (the same text as in the journal, without the arm field)
- `action type=N spell=S target=E dest=x,y,z`: every action the policy decided for it this tick (before the move gate)
- `result seq=N type=T spell=S status=... reason=R` / `refused seq=N ...`: the core's answer to each action, with the reason code
- `state life= hp=% level= map= pos=x,y,z combat= target= moving= casting= group= hostiles= hostile_max= above= elites= nearest=entry:level@yards`:
  when anything in it changes and at least every 5 s; `hostiles` counts the live hostile units within 40 yards, `above` those 3 or more levels
  above the bot

Nothing of the trace goes to the journal. Delete the folder after the run. Find the death chain with
`grep -n "event died" bot-235.trace` and read the lines before it.

## Running the read-out

`tools/sim_readout.sh` runs on the sim as root and only reads (journal, `SELECT` on the characters table, `free`, `df`); its temp files go away
when it ends. From the ops host:

```
ssh forever-test 'sudo -n bash -s -- "2026-11-20 06:30" "2026-11-20 08:30"' < tools/sim_readout.sh
```

The first argument is the start (`journalctl --since`), the second the optional end. It prints, per arm and per hour: died, bot-hours,
deaths per bot-hour and no_route per bot-hour; per arm for the window: levels gained per bot-hour (from `level_up` and every `level=` field),
deaths by map, by level band (the died line's `level=`, else the characters table) and top killers; the per-bot spread (median, top-10 share)
and the `deaths_top` lines; repeat deaths of one bot in one 100-yard cell; the level-safe, death-loop, below-map, quest_walk_blocked and
level_up counts; levels now by band and class; errors; pump decide ms; RAM and disk. `READOUT_LOG` / `READOUT_CHARS` read files instead of
the journal and the database (the test, `tests/sim_readout_test.sh`, does).

## A=B calibration (do this first)

Before any real A/B, run both arms with the same config to see how far apart two equal arms land. If equal arms differ by more than the
effect you want to measure, a real A/B cannot show it.

1. Sim config (outside a sim run; it needs a restart): `ArmSplit = 1`, `ArmSplitMode = strata`, `ArmBOverrides = ""`, no `Arm.B.*` keys. Everything else as for the
   real run (bot count, ramp, keys). Optionally `TraceBots` for two bots of each arm.
2. Purge or keep bots as the run needs (standing rule: stay under the bot limit), restart the sim, note the start time once the ramp is done.
3. Run 2 hours, no pushes or config changes meanwhile.
4. Read out: `ssh forever-test 'sudo -n bash -s -- "<start>" "<start + 2h>"' < tools/sim_readout.sh`.
5. Pass: in "per arm, whole window", arm A and arm B are within 10% of each other on `deaths_per_bot_hour` and on `levels_per_bot_hour`
   (|A - B| / mean(A, B) <= 0.10), and the class mix per arm is close. Also look at the per-hour lines: one hour far off with the others
   close is noise from a few loopers (see the repeat-death cells); a steady gap is a bias of the split.
6. On a fail: look at the top-10 share and the repeat cells per arm (a single looping bot can carry 10% of an arm's deaths), and the class and
   level mix. Run longer (4 h) or with more bots before trusting a real A/B.

## A real A/B

1. `ArmSplit = 1`, `ArmSplitMode = strata`, `ArmBOverrides = "<the change>"` (for example `"LevelSafeWalks=0"`, or `"DeathLoopBreaker=1"`). Arm A is the config as
   written, the baseline.
2. One change per run where possible; several keys are fine when they are one feature.
3. Run as long as the effect needs (8 h for deaths per bot-hour by band; the overnight window), read out with the same command.
4. Judge against the calibration: a difference inside the A=B gap is no result. Write the numbers into the ops read-out doc with the config of
   both arms (the start-up line `arm split on: ... arm B overrides: Key=new (A: old)` has it).
