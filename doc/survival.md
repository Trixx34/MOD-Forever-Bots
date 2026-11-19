# Survival system (2026-11-19)

Branch `feature/survival-20261119`, built on `integrate/arm-d-20261119` (DeathLoopBreaker, level-safe, below-map, Skyborne exit quests, level dungeons).
Three parts, each behind its own key, all off by default (today's behaviour). The core part is `Bot.Actions.TeleportInCombat` on the core branch
`feature/bot-combat-rescue-20261119` (Trinity-Forever-Modules).

Why: the sim arms A, B and C all had about 3.6 deaths per bot-hour. Arm C (2026-11-19, 400 bots, 7.9 h): the Raven Hill loop (bots 235 and 66, level 8 to 10,
481 deaths to the level 24 Skeletal Horror, entry 202, at the Duskwood graveyard -10606,294), four Skyborne killers with 1,692 deaths on map 2991 (251707,
254626, 251245, 256092), one pull in twelve ending in death, warriors 38.5 and rogues 29 deaths per bot.

## Keys

| key | default | sim (survival arm) | prod | what |
|---|---|---|---|---|
| `SafeRevive` | 0 | 1 | 0 | ghosts never revive beside a high hostile or at a dangerous place |
| `SafeReviveLevelMargin` | 3 | 3 | 3 | "high": this many levels above the bot (1..20) |
| `FightRiskRetreat` | 0 | 1 | 0 | potion or bandage, then walk away from a fight the bot is losing |
| `FightRiskMarginPct` | 100 | 100 | 100 | 50..400; losing when time to die < time to kill x pct / 100 (above 100 the bot leaves earlier) |
| `FightRiskTeleport` | 0 | 1 (with the core key) | 0 | the rescue teleport after 8 s of walking away |
| `DangerMap` | 0 | 1 | 0 | grind spots, quest walks and explore walks skip dangerous cells |
| `DangerMapDeathsPerHour` | 6 | 6 | 6 | 1..1000; the rate that makes a cell dangerous for a band |
| `DangerMapDecayMin` | 120 | 120 | 120 | 10..1440; a death's weight falls to 0 over this |
| core `Bot.Actions.TeleportInCombat` | 0 | 1 | 0 | the core applies kTeleport in combat for a solo bot on an open-world map, off a transport |

All module keys take the prefix `Module.forever-bots.`; add them to every environment with explicit values (add-all-keys rule). AutoGearing stays 0 on prod.
`FightRiskTeleport = 1` without the core key only logs `fight_retreat stage=teleport_refused` (the walk away goes on).

## SafeRevive

A solo ghost in the open world (no group, no dungeon route, not a dungeon map) is held before every revive:

- the spirit healer at its graveyard (the ghost fallback, `DeadGhostFallback`), and a spirit healer asked from elsewhere;
- the spirit healer asked from the corpse (the core walks to a healer near the corpse or reclaims there);
- the corpse reclaim.

Held when a live hostile `SafeReviveLevelMargin` or more levels above the bot stands within 40 yards of the revive point, or when the danger map counts that
point as dangerous for the bot's level band. A held graveyard is marked unsafe for the bot's level (10 minutes for a hostile, 30 for the danger map; the
level-safe choice of every bot skips it too), and the ghost is moved, as a ghost (the core never refuses a ghost's teleport, nothing can follow it), to another
graveyard of the same map and side: one that fits its level, else the nearest known one that is not unsafe and not dangerous. A held reclaim turns the ghost to
the spirit healer of a safe graveyard (`ghost_fallback reason=safe_revive`). No other graveyard known: the ghost waits, at most 10 minutes, then revives as
before (`stage=give_up`), so no ghost stays a ghost for good. Never another map: a Skyborne ghost stays on 2991.

Events: `safe_revive: bot= level= map= stage=held what=spirit_healer|spirit_healer_far|spirit_healer_near_corpse|reclaim reason=hostile|danger at=x,y
[entry= hostile_level=] deaths_h=` (at most once per 30 s per bot), `stage=yard from= to= dist= known=`, `stage=wait known= max_wait_s=600`,
`stage=give_up what= at= waited_s=`.

SafeRevive keeps the danger map (deaths are noted) even with `DangerMap = 0`; only the skips of walks and spots need `DangerMap = 1`.

## FightRiskRetreat

A solo bot in the open world (no group, no dungeon route, not a dungeon or raid map, not on a transport, not in flight) samples its own health and its target's
every turn of a fight. After 3 s, at 70% health or less: incoming damage per second is the larger of its health loss over the last 6 s and the big hits the core
reported in that window (damage events, the `died` line's `last_hits`); outgoing is its target's health loss. Time to die = health / incoming; time to kill =
target health / outgoing (never when the target loses nothing). Losing: time to die < time to kill x `FightRiskMarginPct` / 100 (above 100 the bot leaves earlier). Then, in order:

1. a health potion it carries, when the shared potion cooldown is over (`stage=potion`; the fight goes on);
2. a bandage it carries, once per fight, when nothing hit it for 3 s (`stage=bandage`);
3. walk away (`stage=disengage`): to the last place it stood out of combat at 80% health or more with no live hostile within 30 yards (10 to 300 yards off,
   same map), else 40 yards straight away from its target; it does not attack meanwhile;
4. still losing after 8 s of walking and `FightRiskTeleport = 1`: a rescue teleport (kTeleport, map = the bot's own map) to the nearest grind spot for its
   level that is at least 100 yards away, not too high for it (level-safe margin) and not dangerous; once per fight and per 10 minutes
   (`stage=teleport`, `stage=no_spot` when none fits). Never another map: a bot on 2991 stays on 2991.

It ends when the bot is out of combat (`stage=end result=out_of_combat`) or nothing hits it any more (`result=not_losing`); a death during it logs
`stage=died last=<step> entry= killer_entry=`. A refused rescue teleport logs `stage=teleport_refused status=` once.

Every line: `fight_retreat: bot= level= map= stage= hp= in_dps= out_dps= ttd_s= ttk_s= entry=` (entry: the target when the retreat began).

## DangerMap

One map for all bots, owned by the policy (`src/danger_map.{h,cpp}`). Every open-world death (not a dungeon route, not a dungeon or raid map) is noted in a
40 yard cell of its map with the killer entry and the dead bot's level band (1-5, 6-10, 11-15, ...), the newest 64 per cell, at most 20,000 cells. A death's
weight falls linearly from 1 to 0 over `DangerMapDecayMin`; the rate of a cell for a bot is the weighted deaths of bots of its band or a higher one, as deaths
per hour (a steady rate r adds up to r x decay hours / 2). A point is judged by its cell and the eight around it (the highest). Dangerous: rate >=
`DangerMapDeathsPerHour` (6: about six fresh deaths in one cell).

With `DangerMap = 1`:

- grind spots (every user of the spot list: grinding, the death area and level-safe escapes, the loop breaker's move) leave out dangerous spots, unless none
  would be left (`danger_skips=grind_all_hot`);
- a quest walk (giver, ender, hunting area) whose end or straight way (past its first 60 yards) passes a dangerous cell is dropped the way the level-safe
  check drops one (the planner moves to the next area or blocks the quest a while): `quest_walk_blocked reason=danger kind= level= deaths_h= at= dest=
  top_entry=` (throttled like the level-safe lines); a Skyborne exit step is never dropped;
- an explore walk picks another direction (up to 4 tries).

Events: `danger_cell: map= cell=x,y band=6-10 deaths_h= deaths= top_entry= top_share= bot= level=` when a death makes a cell dangerous for that band (once
per cell and band per hour). The hourly `deaths_top` line ends with ` danger_cells= danger_hot= danger_deaths= danger_skips=grind:a,quest:b,explore:c,revive:d
danger_top=map:x,y:entry:rate,...` (the three hottest cells).

## Sim test (parity arm, 8 h)

Arm B with `SafeRevive`, `FightRiskRetreat`, `FightRiskTeleport`, `DangerMap` = 1 and the core's `Bot.Actions.TeleportInCombat` = 1; arm A without. Count per arm:

- deaths per bot-hour (median and trimmed) against arm C's 3.6; target 2.0 or less;
- `deaths_top` top-10 share (arm C 11 to 24%); target under 8%;
- repeat deaths in one cell: deaths per bot within 40 yards to the same killer (arm C: 260 and 221 at Raven Hill); target no bot over 30 at one spot;
- `fight_retreat` per `stage=` (potion, bandage, disengage, teleport, teleport_refused, no_spot) and how each fight ends: `stage=end` vs `stage=died`
  (saved share);
- `danger_cell` lines (which cells, which killers; the four Skyborne entries and 202 should appear) and the `danger_*` fields per hour;
- `safe_revive` per `stage=` (held reasons, yard, wait, give_up);
- levels per bot-hour not lower than the other arm (arm C about 1.1), and `quest_walk_blocked reason=danger` per hour (a high count means the threshold is too
  low).

Not to read as a result: a lower death count with a much lower level rate (bots that only walk away).
