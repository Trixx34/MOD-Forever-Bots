# Decide scheduler (2026-11-19)

Slice 5 of the 2026-11-19 plan (perf). Arm C on the sim: pump decide avg 7.9 ms at 350 bots, 11.2 ms at 400 bots, max 47 ms, growing over the run
(forever-ops `docs/sim-arm-c-20261119.md`). Prod target is 800 bots. `WalkDecideEvery` (docs/perf-bots-module-20261115.md) only throttles calm walkers;
idle, resting and dead bots still take their full turn (some 40 steps) every tick. The scheduler extends that to every bot by tier, with a time budget.

All keys default off: with `DecideScheduler = 0` the policy loop is the same as before (every bot, every tick, view order).

## What it does

`src/decide_scheduler.{h,cpp}` (pure logic, no core, no clock; unit tested) and `BotPolicy::Decide` / `SchedulerFactsOf` (`src/bot_policy.cpp`).

Tiers, from the snapshot and the policy's own state:

| tier | when | runs |
|---|---|---|
| every | in combat or a threat target or the policy's fight; a group, tank lead or follow order; a dungeon map, an active route, a staging hold, a level hold, a raid-sized group; a ghost within reclaim range of its corpse or with a resurrect offer; casting, on a transport, ground effects or telegraphs, a dead pet, a forced heal or LoS move; a living hostile within 30 yd | every tick, never deferred by the budget |
| walk | alive and moving, or standing with quests in the log, or on an errand, visit, supply, repair or travel trip | one tick in `DecideTierWalk` (3) |
| idle | resting (sat), idle standing with an empty log, dead waiting to release, a ghost on its corpse run | one tick in `DecideTierIdle` (6) |

Never skipped (run on the tick it happens, whatever the tier and the budget):

- a new action result (`last_action_sequence` changed since the bot's last turn), which covers every refusal;
- a change of life state since the last turn (death, release, resurrect);
- the end of a walk (moving on the last tick, not moving now; tracked every tick, so a walk that starts and ends between two turns is caught);
- a pending reaction: group invite, resurrect offer, fought units waiting to be looted, gear or bag-gear request (a trade finished), guild leave request,
  duel challenge, gift recipient, rest done (sitting with full health and mana);
- a bot seen for the first time.

Staggering: a bot of a tier with N ticks is due when `(tick + StaggerOf(guid, N)) % N == 0`, `StaggerOf` = splitmix64(guid) mod N. 800 sequential guids
land within 20% of the mean in every phase (test), so a tier's load is spread over its N ticks instead of landing on one.

Budget (`DecideBudgetUs`, 0 = off): the must-run bots run first (view order) and count against the budget; then the due bots run, the carried-over ones
first, then the longest waiting, then round-robin from the first bot that did not fit on the last tick, while the tick has used less than the budget.
A due bot that does not fit is carried over (stays due, first in line next tick). `DecideMaxSkipTicks` (12): a bot that has waited this many ticks is a
must-run, so nothing starves even when the every tier alone fills the budget.

A bot that does not run is still present for the policy (its state is kept) and sends no actions that tick. Its timers (wander, shopping, rest, release)
are time based (`nowMs`) and fire on its next turn, at most N-1 ticks late.

Order change with the scheduler on: must-run bots first, then the others. Within a tick the policy already does not depend on bot order for parties
(fight start is noted before the loop; party bots are all every-tier).

## Keys

| key | default | range | sim (perf arm) | test | prod |
|---|---|---|---|---|---|
| `DecideScheduler` | 0 | 0/1 | 1 (arm B of the parity run) | 0 | 0 |
| `DecideBudgetUs` | 0 | 0 = off, 500..200000 | 12000 | 0 | 0 |
| `DecideTierWalk` | 3 | 1..10 | 3 | 3 | 3 |
| `DecideTierIdle` | 6 | 1..20 | 6 | 6 | 6 |
| `DecideMaxSkipTicks` | 12 | 1..60 | 12 | 12 | 12 |

Files: `bot_settings.h/.cpp`, `main_options.cpp` (into `PolicyOptions::DecideScheduler`), README table, `config/modules.d/forever-bots.conf.example`.
Add-all-keys rule: every env gets all five keys with the values above when this build reaches it (prod: off; the drop-in is prepared in the window,
not applied outside it):

```
Module.forever-bots.DecideScheduler = 0
Module.forever-bots.DecideBudgetUs = 0
Module.forever-bots.DecideTierWalk = 3
Module.forever-bots.DecideTierIdle = 6
Module.forever-bots.DecideMaxSkipTicks = 12
```

`WalkDecideEvery` stays as it is; with the scheduler on it still applies inside a walking bot's turn (a scheduled turn of a calm walker can still be
skipped by the calm gate). For a clean measurement set `WalkDecideEvery = 1` in the scheduler arm only if the other arm also has 1.

## Pump line

The minute line `pump: N bots, decide avg X us, max Y us over T ticks, ... S calm-walk turns skipped in all` gets a tail when the scheduler is on:

```
 sched=on walk=3 idle=6 budget_us=12000 tiers=every:S/R,walk:S/R,idle:S/R skipped=K deferred=D forced=F starved=X overruns=O max_turn_us=M max_tick_us=T
```

S/R = bot-ticks seen / turns run per tier in the period; `skipped` = turns not due; `deferred` = due turns carried over by the budget; `forced` =
reactions; `starved` = turns forced by `DecideMaxSkipTicks`; `overruns` = ticks over the budget; `max_turn_us` = the longest single bot turn (the
47 ms max of arm C: is it one bot or the sum?). Counts reset with each line.

## Tests

`tests/decide_scheduler_test.cpp` (ctest `decide_scheduler`): tier choice; off runs every bot in view order; cadence (idle 10 of 60 ticks, walk 20,
every 60); never-skip rules under a used-up budget (combat, refusal, new result, death, release, resurrect, end of walk, pending reaction, reviving
ghost, party, dungeon); budget carry-over round-robin (50 bots, 10 fit a tick, each runs exactly once in 5 ticks, max wait 4); starvation cap (one fight
eats the budget, every other bot still runs within cap + 1 ticks); stagger distribution (800 guids, N 3 and 6, within 20%, deterministic; 600 idle bots
run about 100 per tick); a simulated 800-bot load (20% every, 40% walk, 40% idle; turn cost from arm C, 28 us a bot on average: 40/25/18 us with +-50%
noise, 2% new results a tick): 19.8 ms a tick without tiers, 10.1 ms with tiers, 10.1 ms under a 12000 us budget (it does not bind), and 8.0 ms
under an 8000 us budget that binds (8010 us average, at most one turn past it; deferred > 0, starved 0, max wait 9 of the cap 12, every tier never
skipped); policy integration (off: no scheduler memory and no pump tail; on: a standing bot
waits but stays present, an invite runs it at once, a fighting bot runs every tick, the pump tail). `tests/settings_test.cpp` `DecideSchedulerSettings`.

The 800-bot numbers are a model, not a measurement: the real share of each tier and the real turn costs come from the sim.

## Sim plan

1. Parity A/B in one run, once the harness branch exists (slice 1: `ArmSplit = 1`, guid parity): arm A `DecideScheduler = 0`, arm B
   `Arm.B.DecideScheduler = 1`, `Arm.B.DecideBudgetUs = 12000`, tiers 3/6, cap 12. Same `WalkDecideEvery` in both arms. Before the harness: two runs one
   after the other with the same config except these keys (config drift is the known weakness, note it in the read-out).
2. Ramp 400 -> 800 bots with `scripts/sim-scale-ramp.sh` in forever-ops (the script does not exist yet on 2026-11-19; until it does, the ramp keys
   `BotsInitial = 400`, `BotsStep = 100`, `BotsStepMinutes = 30`, `BotsCap = 800` do the same). Hold 30 min at each step. Watch RAM (1.7 GB free at
   400; `HotSwap.Enabled = 0` on the sim for the run). No test pushes during the run (overnight push freeze).
3. Read: per step the pump lines (decide avg and max, the `sched=` tail per tier, deferred, starved, overruns, max_turn_us), the core world update
   interval, deaths per bot-hour and levels per bot-hour per arm, `no_route` per bot-hour, quest_reward per bot-hour.
4. Success: decide avg <= 6 ms at 400 bots and <= 12 ms at 800 bots (whole policy, both arms in one process: compare with arm C 11.2 ms at 400; with
   parity in one process the gain is about half the full gain, so also read it from a run with all bots on), deaths per bot-hour and levels per
   bot-hour of the scheduler arm within 5% of the other arm, `starved` near 0 (the budget is not starving anyone), no rise in `fight_stalled`,
   `death_loop_break`, `no_route` or quest refusals in the scheduler arm.
5. If the tiers alone reach the goal, try `DecideBudgetUs = 0` too (fewer moving parts); if decide avg is still high, try `DecideTierIdle = 10` before
   touching the walk tier.

## Risks and what is not verified

- Not run against the core: only the unit tests. The tier facts are read from the snapshot and the policy state; a fact the policy reacts to that is
  not in the list (for example a chat order that only changes state in `ApplyChat` and waits for the next turn) waits up to N-1 ticks. At a world
  update of 0.5..1.7 s and N = 6 that is up to 8 s for an idle bot.
- The hostile radius (30 yd) puts many questing bots in the every tier; the real savings depend on how many bots stand near hostiles. The pump tail
  shows the share.
- Must-run turns are not limited by the budget (by design); a combat-heavy tick still overruns. `overruns` counts it.
- The budget is measured with `std::chrono::steady_clock` around each turn (two reads per turn, cheap).
