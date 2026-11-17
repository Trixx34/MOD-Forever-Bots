# fight_stalled on start-zone creatures (map 2991), 2026-11-16

The watchdog in `TurnFightWatch` (`src/bot_policy_turn_target.cpp`) gives a target up when neither its health nor the bot's went down for `kFightStallMs` (45 s). Prod (2026-11-16): about 120 per hour at 800 bots, clustered on map 2991 (Juvenile Vuldren, Pesky Cirrusfly, Skyhopper), the target 3-4 yards away with low health for the whole stall. The sim journal has the same shape on other maps (target entries 1708, 3538: `distance=3..4`, `target_hp` 74..289, two bots of one party stalled on the same target with the same health).

## What the code does at that range

- `turn.distance` is the ground distance (`rotation::DistanceYards`, x and y only). `MoveToTarget` compares it with the approach range (5 yards for melee) and, when it is inside, issues `kAttackTarget`; a ranged bot at 3-4 yards is inside its stand-off too. The vertical distance is never looked at there.
- The rotation does use the 3D distance (`DistanceYards3d`) for the row ranges, so a caster with a target above it skips rows whose range is exceeded, but it never walks nearer, because the approach test already said "in range".
- The `fight_stalled` event logs only the ground distance, so the stalls look like "target 3-4 yards away, nothing happens".

## Hypotheses (the telemetry does not separate them)

1. Target in the air or on a ledge (flying creatures: Cirrusfly, Skyhopper; Vuldren is a flyer too): ground distance 3-4, 3D distance well above melee reach. Melee bots swing at air; the creature cannot reach the bot either (no damage to the bot), and low health because a pet or a first spell hit it. Fits the cluster on flyers, "low health for the whole stall" and "bot health unchanged". Most likely.
2. Caster out of power with no wand: `outOfPower` makes the bot close to melee, so it should hit; a wand-less caster whose melee swing is refused would show a `kRefused` last action (that already avoids the unit).
3. Line of sight: bot and target 3-4 yards apart behind a wall or fence (start zones have fences). The core refuses the swing or cast; refused casts retry, a refused melee avoids the unit, but a cast row that keeps failing with another status does not.
4. Target fleeing or evading (low-health flee, leashed to its spawn): the target would move, the distance would not stay at 3-4.

## Changes (both default off)

- `FightStallDiag = 1`: the `fight_stalled` event also carries `class`, `level`, `power`, `moving`, `combat`, `last_action`, `last_status`, `at` (bot position), `dz`, `dist3d`, `target_victim`, `target_casting`, `target_level`. One look at 20 lines separates hypothesis 1 (`dz` large, `dist3d` above reach) from 2 (`power=0`), 3 (`dz` small, `last_status` of a cast or `moving=0` with no damage) and 4.
- `FightHeightAvoidMs = 3000..60000` (0 = off): a melee-range bot (not a healer, not on a raid boss) whose target is within its ground approach range but at least 5 yards above or below it for this long avoids the target (`fight_height_avoid: bot target_entry ground dz held_ms`) and takes another one. A slope or a step of 2 to 4 yards does not count. Casters are not affected: their rows use the 3D distance.

Tests: `tests/policy_test_11.cpp` (`AMeleeBotAvoidsATargetOutOfVerticalReach`, `TheStallEventCarriesDiagnosticsOnlyWithItsKey`), `tests/settings_test.cpp` (`FightStallSettings`).

## Next step

Turn `FightStallDiag` on in the sim drop-in (and `FightHeightAvoidMs = 8000` to measure the effect), run a leveling block that includes map 2991 creatures, and count `fight_height_avoid` against `fight_stalled` per hour. If the diagnostic shows another cause (power, line of sight), the lever is a different one: a stand point nearer the target for a caster at `dz` small, or an avoid after a few refused casts.
