# Zul'Gurub: why the raid dies on the entry ledge, a new route, and a plan (2026-11-15)

Branch `feature/zg-route-plan` (from `origin/test` a192123). This is a proposal. The shipped route `data/dungeons/309-zulgurub.json` is unchanged, and so are all key defaults.

Sources:
- the forever-ops study `docs/dungeon-studies/zulgurub_route_study.html`, `docs/dungeon-studies.md`, and its raw data: `analysis.json`, `db.txt` (sim world DB), `navpolys.txt`;
- the sim journal extract from 2026-11-12 on (`journal.txt`; only the event types that `ds_journal_fetch.sh` fetches);
- the map-309 navmesh, queried through `navtool`;
- the module code on `origin/test`.

Nothing was run on the sim.

## 1. Diagnosis

The 30 full runs had 3 raids of 20 bots each, 3600 s per run. The leader reached route point 8 to 24 of 224 (median 11). No boss was attempted. There were 16526 deaths.

ZG_ROSTER (`race:class`) is 4 orc warriors, 4 undead priests, 4 troll mages, 4 orc warlocks and 4 undead rogues. It has no hunter and no shaman.

The ledge has several problems at once. Some are causes, some are symptoms.

| # | Cause (C) or symptom (S) | Evidence |
|---|---|---|
| 1 | **C: the route walks through a thrower camp.** Legs 6-8 of the shipped route pass 9 yards from the Axe Thrower and 15 yards from the Hakkari Priest at the bottom of the stairs (pack 72). That is inside their 13.5-yard aggro radius, so the raid walks into them; nobody pulls them. Axe Flurry has a 30-yard radius and covers the whole flight of stairs. | 5377 deaths at (-11838,-1308) (killers: thrower 4037, priest 1264), plus 1254 at (-11858,-1297). This camp has been the main killer since the 11-13 12:38 runs: the top 10-yard cells were (-11840,-1310) and (-11840,-1300). |
| 2 | C, now mostly fixed: **the shelf packs were pulled with the ledge pack.** These are pack 61 (2 throwers, 30 yards from the ledge legs, 7 yards lower) and pack 57 (Serpent + Adder, 20-23 yards). The ledge is walkable to the shelf (navtool: 13.9 yards), so this is not an unreachable kite. | 4332 + 645 deaths at (-11882,-1302) and (-11885,-1283), nearly all from the 11-12 and 11-13 morning runs (cell (-11880,-1300): 952 and 1015 deaths). After the leash 15 and pullpos rows (bots 99206b0, 6a5ab75) the cell drops out of the top list. The 11-13 morning run had 393 pulls on 11371/11372 at route_yards=28. |
| 3 | **C: the revive-in-place loop.** A ghost is teleported to the instance entrance, and the core revives it there at 49-50% health. It then walks back into the fight that is still going on. | 11-14 window (bots c92b0fa): 2209 deaths and 2130 `revived:` (2097 how=unknown). 685 of 2042 revived bots died again within 30 s (34%), and 895 within 60 s (44%). The runs averaged 682 revives per 3600 s. Dead time per run was 1004-2710 s (`time_s dead:`). `GhostTeleportHold` checks only for a hostile within 30 yards of the destination. The entrance has none, so the teleport is never held. |
| 4 | **C (new): `pullpos` rows apply to the whole map.** `raid_rules::Table::PullPositionOf(map, entry)` has no position. The two shelf rows for 11371/11372 therefore hold for all 13 snake packs on the routes (the first is at shipped point 14, 130 yards on). Before every later snake pull the tank walks back toward the ledge for up to 30 s (`kPullPosWalkMs`), and after the pull for up to 15 s (`pull_back`). This drags the pull across the plateau. | Code: `bot_policy_turn_move.cpp` `MoveToTarget` and `bot_policy_turn_target.cpp` (positioned hold). The leader's index sits at point 9 for 10 of the 30 runs (study). Points 9-14 are where the snake packs start. |
| 5 | **C: Jin'do is the first boss.** The shipped route's first boss stands inside a 15-mob chain (pack 55, which includes Jin'do himself). A 10-mob pack (53) and three 4-packs sit at route points 21-26. | Point 24 is the second hot spot: the one run that got there wiped (553 deaths, killers 14883/14882/14825 and a summoned 14668). 11-14 quick re-deaths cluster at (-11580,-1325). |
| 6 | S: **pull churn on the thrower.** | 11-14 window: 1191 of 2651 `pull:` events on map 309 targeted 11350. 6008 `no_route` lines (2686 cause=combat). 248 `regroup_stuck`, 837 `member_recover`. |
| 7 | Contributing: **no line-of-sight handling.** The module has no vmaps, so no out-of-LOS pull is possible. The throwers throw from 20-30 yards, and the raid of 20 follows the leader as a ball. | `ds_data` uses a navmesh ray only as a stand-in for LOS. The raid rules note this for every pullpos row. |
| 8 | Contributing: **mana and health gates add rest without progress.** | `time_s rest:` was 255-1737 s per 11-14 run. The 11-13 run 2 had 2895 `pull_hold reason=low_hp`. The gates work, but each pull still meets the thrower. |
| 9 | Latent: **the shipped route cannot finish.** Legs 216 and 223 (Venoxis, Jeklik) are not walkable. Boss point 61 is Zanza the Restless (15042), who is *neutral* (faction 1574) and never dies. | navtool; `db.txt` faction. |

**Summary.** Fixes 99206b0 and 6a5ab75 mostly cured the ledge-specific failure (pulling the shelf with the ledge). What is left is the stairs camp (72), which the route walks into, and the revive loop that feeds the raid back into it. Pullpos rows that hold map-wide stall the plateau, and the first boss after that is a 25-mob court.

## 2. Proposed route: Venoxis first

The proposed route is in `data/dungeons-proposed/309-zulgurub.json`. To try it, set `Module.forever-bots.DungeonDir = <runtime>/data/dungeons-proposed` (an existing key; `doc/dungeon-strategy.md`). The same route in `dungeons.txt` block format is in `doc/zulgurub-route-proposal.txt`. It was made with the forever-ops `ds_plan.py` and `DS_PLAN_KEY=plan_zg_20261115`.

Navmesh check:
- All 140 legs from the entry are `path ok` in navtool, 5504 yards in total.
- All 140 points snap to the main mesh island (island 0). The shipped route has 2 blocked legs.
- Two legs are long and straight (172 and 180 yards).

Bosses that exist (spawned in `creature`): Jin'do 11380, Hakkar 14834, Zath 11348, Lor'Khan 11347, Thekal 14509, Mar'li 14510, Mandokir 11382, Venoxis 14507, Jeklik 14517.

Not spawned, so not in the route:
- Arlokk 14515 (gong);
- Gahz'ranka 15114 (template only, lure);
- the Edge of Madness bosses (summoned).

Zanza 15042 is neutral and is dropped.

**Order: Venoxis, Jeklik, Mar'li, Mandokir, Zath/Lor'Khan/Thekal, Jin'do, Hakkar.** This is the classic priest-first order, and Hakkar comes last. Over the 120 orders that start with Venoxis and end with Hakkar, it is within 1% of the lowest-danger order (4534 vs 4506).

Venoxis and Jeklik stand on separate navmesh islands:
- Venoxis is 1.3 yards off the main mesh. His boss point is the nearest main-mesh point, 15 yards from him.
- Jeklik is on a platform 11 yards above the main mesh. Her boss point is the nearest main-mesh point, 12 yards from her.

Exposure, measured the study's way (pull leash 30, aggro radius, packs = 10-yard assistance):

| stretch | route | yards | hostiles | danger | packs | over the 4-mob cap |
|---|---|---|---|---|---|---|
| entry to the 1st boss | shipped (Jin'do, point 27) | 559 | 40 | 432 | 8 | 53 (10), 55 (15, holds Jin'do) |
| entry to the 1st boss | **proposed (Venoxis, point 24)** | 635 | **32** | **355** | 13 | 130 (Venoxis + 4 cobras: the encounter), 137 (7) |
| entry to the 2nd boss | shipped (Hakkar, point 61) | 1251 | 69 | 927 | 17 | 53, 55, 97 (12, Hakkar's altar) |
| entry to the 2nd boss | proposed (Jeklik, point 38) | 1208 | 60 | 629 | 22 | 130, 137, 31/32/43 (bat-rider patrols) |
| whole route | shipped | 4884 (2 blocked legs) | 217 | 2690 | 66 | 19 |
| whole route | proposed | 5504 (0 blocked) | 246 | 2954 | 71 | 22 |

The whole proposed route meets more hostiles because it really reaches Venoxis and Jeklik. The shipped route's blocked legs never count them. The proposed route has more thrower camps up to Venoxis (packs 72, 81, 122, 135, 115, 137), but every one is 1-2 throwers. The shipped route trades them for one 25-mob court. The rendered study page for this route is at `$CLAUDE_JOB_DIR/tmp/zgr/out/zulgurub_route_study.html` (not committed, 3.2 MB).

### First stretch: point table (JSON index = the log's `point=N`)

| pt | x, y, z | yards | role, packs met on the leg to it |
|---|---|---|---|
| 0 | -11914.4, -1226.0, 92.7 | 0 | entry |
| 3 | -11906.9, -1269.9, 87.3 | 47 | **raid hold H1**: 50+ yards from packs 57, 61, 63 and 72 |
| 5 | -11872.5, -1287.7, 82.3 | 86 | **pull spot** (pullpos_near): shelf snakes 57 (26-30 yards); stairs camp 72 (thrower 51, priest 57 yards: out of Throw range, so they climb toward the tank) |
| 7 | -11840.0, -1301.3, 66.3 | 125 | stairs (72 is 9 yards off this leg: dead by now) |
| 8 | -11810.7, -1317.6, 62.0 | 159 | plateau: **end of segment 1** |
| 11 | -11854.7, -1354.1, 62.8 | 218 | **pull spot** for 81 (thrower + priest, 66/68 yards); the 73 snake patrol is near |
| 12 | -11882.7, -1408.0, 61.5 | 279 | 73 (3, patrol), 81 |
| 14 | -11939.5, -1519.7, 42.8 | 412 | **pull spot** for 122 (thrower, priest, adder, serpent: 36-48 yards) |
| 16 | -11972.5, -1553.3, 40.9 | 464 | 122 |
| 17 | -11987.2, -1567.2, 26.5 | 489 | **pull spot** for 135 (2 throwers, 48/55 yards) |
| 18 | -12001.1, -1600.0, 26.0 | 525 | 135; 158 (Hooktooth, in the water below) |
| 20 | -11996.5, -1622.1, 33.4 | 551 | 115 (thrower + priest, patrol) |
| 21 | -11996.8, -1653.3, 34.0 | 582 | 136 (4 snakes) |
| 22 | -11998.7, -1666.7, 32.4 | 596 | 138 (2 snakes); **pull spot** for 137 (7) |
| 23 | -12009.1, -1688.0, 32.4 | 619 | 130, 137 |
| 24 | -12018.6, -1698.4, 39.5 | 635 | **Venoxis** (boss point, 15 yards from him): **end of segment 2** |

Further boss points: Jeklik 38, Mar'li 65, Mandokir 83, Zath/Lor'Khan/Thekal 108-110, Jin'do 122, Hakkar 140.

Segments for wipe recovery:
- S1: points 0-8 (the ledge);
- S2: 9-24 (Venoxis);
- S3: 25-38 (Jeklik);
- S4: 39-65 (Mar'li);
- S5: 66-83 (Mandokir);
- S6: 84-110 (Thekal);
- S7: 111-122 (Jin'do);
- S8: 123-140 (Hakkar).

With `ScenarioWipeRecovery = 1` (+ `WipeBackoff*`), a wipe puts the party back at a safe route point before the pack. A quiet party's ghosts already go to the leader (`party_teleport`). A start-of-segment checkpoint is plan item 5.

## 3. Changes on this branch (all default off or neutral, unit-tested)

1. **`GhostHoldPartyFightMaxMs`** (new key, default 0 = off; 180000 recommended).
   - While a living member of the party is in combat, a dungeon ghost's teleport to the entrance waits, at most this long per death.
   - After the fight it is revived beside the leader (the existing `party_teleport`).
   - While it is held, the 90 s entrance wait starts over, so a ghost whose hold has ended goes to the entrance as before, not on a corpse run into the fight.
   - The leader's own ghost is not held (the Onyxia lesson).
   - Event: `ghost_teleport_hold: ... reason=party_fight`.
   - Code: `BotPolicy::GhostPartyFightHeld`, the entrance branch of `DeadGhostRun` (`src/bot_policy_turn_dead.cpp`).
2. **`DungeonRevivesPerMinute`** (new key, default 0 = off; 6 suggested).
   - At most N ghosts of one party are put back (entrance, leader, leader rejoin) in any 60 s.
   - Event: `ghost_teleport_hold: ... reason=revive_cap`.
   - Code: `GhostReviveCapHeld`, `NoteDungeonRevive`.
3. **Raid rule row `pullpos_near|map|entry|x|y|z|yards|cx|cy|reach|name`** (new row kind).
   - It is a pull position for one camp: the creatures of the entry within `reach` of (cx, cy). The nearest centre wins, and a `pullpos` row is the fallback.
   - The callers now pass the target's position. Code: `raid_rules::Table::PullPositionOf(map, entry, x, y)`.
   - **Data change in `data/raid_rules.txt` (map 309 only):**
     - the two shelf `pullpos` rows become `pullpos_near` rows for the shelf camp. The same spot is used, so the ledge behaviour is unchanged and the map-wide side effect is gone;
     - new rows for the stairs camp 72, pulled from the ledge point;
     - new rows for packs 81, 122, 135 and 137 on the proposed route.
   - This acts wherever `RaidRulesFile` is set.
4. **Proposed route file** `data/dungeons-proposed/309-zulgurub.json` (only loaded through `DungeonDir`), with `kill_priority` [11830 Hakkari Priest, 11350 Axe Thrower]. The kill priority acts only with the `focus` tactic.

Tests (local, `nice -n 19 ninja -j2`, SDK 1281f83): `ctest -R '^(policy|raid_rules|settings|dungeon_files)$'`, 4/4 passed.
- `policy_test_05`: AGhostWaitsForItsPartysFightToEnd, APartysRevivesAreCappedPerMinute.
- `policy_test_04`: pullpos_near pulls.
- `raid_rules_test`: TestScopedPullPosition and the shipped file.
- `settings_test`: the two keys.
- `dungeon_files_test`: ZulGurubProposedRouteLoads.
- `jenkins/ci/check-bots-data.py`: OK.

## 4. Ranked plan (value, then risk)

| rank | what | value | risk | where |
|---|---|---|---|---|
| 1 | Turn on `GhostHoldPartyFightMaxMs = 180000` and `DungeonRevivesPerMinute = 6` for ZG, plus the already existing `GhostHoldAvoidEnemyMaxMs = 300000` | stops the revive-into-the-fight loop (34% re-death within 30 s) | low: holds are capped; the leader is never held | this branch |
| 2 | `pullpos_near` rows (stairs camp 72 from the ledge; scoped shelf rows) | removes the walk-into-72 death cluster and the plateau drag | low-medium: line of sight unverified; the spot is 51-57 yards out | this branch, data |
| 3 | Proposed route via `DungeonDir` (Venoxis first) | a first boss that is a 5-mob encounter, not a 25-mob court; no blocked legs | medium: 6 small thrower camps; Venoxis' and Jeklik's boss points are 12-15 yards from them (aggro is unverified) | this branch, data |
| 4 | Turn on the 11-14 levers that were never measured: `RaidFocusAvoidEnemy`, `PullOnePack`, `TankLooseSkipAvoidEnemy`, `RegroupRallyMs = 30000`; `DungeonTactics = focus,interrupt` (kill_priority, Great Heal kicks) | ranged kill the thrower first and the priest's heal is kicked | low: existing code, default off | config only |
| 5 | Segment checkpoints: an optional `"checkpoint": true` on a route waypoint. After a full wipe, the entrance teleport of `DeadGhostRun` and the scenario wipe recovery use the last checkpoint the leader passed (if it is quiet: no live hostile within 30 yards), not the entry. | a wipe at Jeklik does not walk the ledge again | medium: needs DungeonTeleport; schema addition | `dungeon_files.cpp` (parse), `dungeon_data.h` `DungeonWaypoint`, `bot_policy_turn_dead.cpp` `DeadGhostRun`, `scenario.cpp` wipe backoff |
| 6 | Raid ready gate (the etiquette branch's 5-man gate does not cover raids): before a pull, wait until 3 of 4 healers have 60% mana, the tank has 80% health and members are within 40 yards, 60 s cap | fewer pulls with a dry raid | medium: run 2 showed rest alone adds dead time; only useful after items 1-2 | `bot_policy_turn_move.cpp` route hold; `group_etiquette.*` (`feature/bot-group-etiquette` f1057cf, `GroupEtiquetteReadyPct` 80, stragglers 45 yards/20 s, wait 45 s, raids excluded) |
| 7 | Ranged mobs: an out-of-LOS pull needs the core to put a line-of-sight flag on nearby units in the snapshot (a protocol change; not now). Until then pullpos spots beyond Throw range stand in for it. A roster with 2 hunters (instead of 2 rogues) would add Concussive Shot and Distracting Shot on throwers. | real fix for throwers | high (protocol) / roster change | core snapshot, `sim-overnight.sh` ZG_ROSTER |
| 8 | Remove the Zanza boss point from the shipped route and fix shipped legs 216 and 223 (or swap in the proposed file) | the route can finish | low | `data/dungeons/309-zulgurub.json` |

## 5. How to test it (one overnight step, not run yet)

Use `ONLY=zulgurub` in `scripts/sim-overnight.sh` (3 raids of 20, 3600 s). Do it twice, each run with a drop-in through the step's extra keys:
- **A (shipped route):**
  - `GhostHoldPartyFightMaxMs = 180000`
  - `DungeonRevivesPerMinute = 6`
  - `GhostHoldAvoidEnemyMaxMs = 300000`
  - `RaidFocusAvoidEnemy = 1`
  - `PullOnePack = 1`
  - `TankLooseSkipAvoidEnemy = 1`
  - `DungeonTactics = focus,interrupt`
  - `RaidRulesFile` as now (the new rows)
- **B (proposed route):** the same, plus `DungeonDir = /srv/forever/sim/modules_runtime/forever-bots/data/dungeons-proposed`.

The build must contain this branch.

What to read:
- the result lines `scenario zulgurub#1-3`: `furthest`, `bosses`, `wipes`, `revived`, `time_s dead:`;
- the journal:
  - `ghost_teleport_hold: ... reason=party_fight|revive_cap`
  - `pull_position:` with `target_entry=11350|11830`
  - `died:` (`killer_entry`, `killer_spell=24020`)
  - `revived:`
  - `dungeon_entry_teleport` / `party_teleport`
  - `route_walk point=`
  - `boss_attempt:` (run telemetry)
  - `ASSERTION FAILED` (the 11-12 `_UnapplyAura` on 11350).

Pass (both runs):
- revives at most 200 per raid (baseline 682);
- under 10% of revived bots dead again within 30 s (baseline 34%);
- dead time at most 900 s of 3600 (baseline 1004-2710);
- deaths at the stairs cell (-11840,-1305) at most a third of the 11-14 count;
- no `ghost_stuck` above 300 s, and `corpse_run_start` not up;
- 0 asserts.

Pass for B:
- the leader goes beyond point 40 (past Venoxis 24 and on toward Jeklik) in 2 of 3 raids;
- at least one `boss_attempt` on 14507 in 2 of 3 raids.

A Venoxis kill is a bonus. For A, pass is the leader beyond point 24 in 2 of 3 raids.

Fail:
- the median furthest point is 11 or less again;
- or B is not better than A on deaths per minute;
- or `pull_position` lines show the tank walking for the full 30 s at point 5 (line of sight or reach is wrong; move the spot).

## 6. Not verified

- Line of sight at every pull spot (no vmaps read).
- Whether the throwers climb toward a tank 51 yards away or keep throwing from below.
- That Venoxis (15 yards from his boss point, 1.3-yard mesh seam) and Jeklik (11 yards below her platform) aggro and reach the raid.
- The 73 and 115 patrols' timing.
- Axe Flurry targeting (one member or all in 30 yards).
- The `revived how=unknown` path for the 493 quick re-deaths that had no teleport event: `revive_corpse` and `leader_rejoin` are not in the journal extract.
- Any sim run of the new keys or route.
