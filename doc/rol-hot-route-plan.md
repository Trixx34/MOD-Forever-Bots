# Ruins of Lordaeron and Hall of Thanes: diagnosis, proposed routes and a plan (2026-11-15)

Branch `feature/rol-hot-route-plan` (from `origin/test` a192123). This is a proposal. The shipped routes `data/dungeons/2999-ruins-of-lordaeron.json` and
`data/dungeons/3065-hall-of-thanes.json` are unchanged, and so are all key defaults.

Sources:
- the forever-ops route study (branch `docs/rol-hot-studies`): `docs/dungeon-studies/{rol,hot}_route_study.html`, `{rol,hot}_route_proposal.txt`, and the `rol` and
  `hot` entries of `scripts/dungeon-study/dungeons.json` (hand plan `plan_20261115`);
- the sim's world DB (read-only SELECTs, `ds_fetch.sh`, 2026-11-15; for rol also with the statue's summons 250631, 250618, 250627, 250630, 250624, 250626)
  and the sim's mmaps for maps 2999 and 3065. The local copies of `3065_31_31` and `3065_32_32` differ from the sim's, so the sim's tiles were used;
- the sim journal (`ds_journal_fetch.sh`) and `/var/log/forever/sim/scenario-results.log` for two data sets, always named below:
  - **11-13**: the cycles 2026-11-13 16:26 (1800 s) and 17:00 (cut to 1426 s), 3 parties per dungeon each;
  - **11-15**: the overnight run 4 of 2026-11-15 (`r1-rol-hot`, cycle 03:54, 20 parties per dungeon, 3600 s, base roster `2:1,5:5,8:8,2:9,5:4`);
- the module code on `origin/test` and the Zul'Gurub plan on `feature/zg-route-plan` (`doc/zulgurub-route-plan.md`, not merged).

Nothing was changed or run on the sim.

## 1. Diagnosis

### 1.1 Hall of Thanes (hot, map 3065, levels 13-18): it works, but slowly

| data | parties | outcome | bosses | run (s) | fight (s) | deaths (outcome lines) |
|---|---|---|---|---|---|---|
| 11-13, 16:26 (1800 s) | 3 | 3 complete | 4/4 | 1433-1526 | 1000-1163 | 3 |
| 11-13, 17:00 (1426 s) | 3 | 3 timeout | 3/4 (Durgen missing) | 1426 | 910-1046 | 7 |
| 11-15 (3600 s) | 20 | **20 complete** | **4/4** | 1538-2302, median 1714 | 916-1672, median 1065 | 15 (13 parties none, one 5) |

- The shipped route finishes. Fight time is about two thirds of every run: 107 hostiles at roughly 10 s each.
- Deaths are few. There is no wipe, and 3 re-deaths within 30 s in 20 runs.
  - 11-15: Dark Iron Looter (263396) 15, mostly at shipped points 26-27 (around 105, 0, by Plunder); Dark Iron Summoner 3; Faldrim 2.
  - 11-13: Dark Iron Engineer (263397, Throw Dynamite 1294260) 6, all in one party in 36 s at shipped points 23-24.
- Longest index stall: points 1-3 in 17 of the 20 11-15 runs, 149-370 s. That is the room of 10 Enraged Apparitions and Tormented Souls (pack 12) before Faldrim. Point 13 (the west-wing loop) costs 111-157 s in the 11-13 runs.
- Route problem: the west wing loop (points 7-12) and the east wing loop to Magmatus (13-18) double the walk (1679 yards).
- Plunder patrols the hall on the way. About half the parties killed him second (2 of 3 on 11-13 17:00, 9 of 20 on 11-15).
- No pack, tactics or bot-behaviour problem stands out.

### 1.2 Ruins of Lordaeron (rol, map 2999, levels 15-20): a statue event, a gallery, and the Baron's room

| data | parties | outcome | bosses | leader index (max) | Witherfang / Baron killed at (s) | deaths (journal, on the map) |
|---|---|---|---|---|---|---|
| 11-13, 16:26 (1800 s) | 3 | 3 timeout | 2/4 | 25-29/66 | 372-448 / 1257-1525 | 23 (both cycles) |
| 11-13, 17:00 (1426 s) | 3 | 3 timeout | 1/4 | 16-22/66 | 337-388 / - | |
| 11-15 (3600 s) | 20 | 19 timeout, 1 complete (3567 s) | 14 x 2/4, 5 x 3/4, 1 x 4/4 | **36/66 in all 20** | 342-642 / 1422-2112 | 453 |

**1. The statue at shipped point 36 (a route problem; new in the 11-15 data):**
- What it is:
  - A stalker (guid 21300268) at (1601.4, 341.8, 67.2) has a SmartAI "player in range" trigger: event 10, 25 yards.
  - The trigger starts action list 25515900:
    - 8 Skeletons;
    - then 4 Ghouls (after 90 s);
    - then a Skeletal Soldier, a Plague Ghoul and a Skeletal Mage (after 90 s);
    - then **The Abandoned** (250631), a level-18 elite (Life Drain 1266011, Frost Nova, Chilled), who flies out of the statue.
  - If he despawns unkilled, the statue re-arms. After 15 minutes it also re-arms.
- Why the route meets it: the shipped leg 35 to 36 (to Bjork) passes 14.5 yards from it.
- Effect in the 11-15 run:
  - every leader ended on point 36 and stayed there 171-1090 s;
  - 330 of the 453 deaths are at points 35-36;
  - The Abandoned killed 228 members in 17 of 20 parties, and the wave adds 66 more.
- Why the 11-07 study did not see it: the summons are in an action list (`smart9`), not in the creature table.

**2. The upper gallery (a route problem):**
- Shipped points 14-17 climb to z 83, and point 18 drops back to z 62. The members do not follow the leader down.
- 11-13: in 5 of 6 parties, 20 of the 29 `member_recover` teleports came from z >= 78, after `regroup_stuck yards=30`.
- 11-15: in 19 of 20 parties, 69 recoveries came from z >= 78. The leader index sits 134-342 s at point 18 in every run.

**3. The Baron's room, shipped points 20-24 (pack/tactics and bot behaviour):**
- 103 deaths on 11-15 and 23 on 11-13.
- **Cadaver Worms** (16143, from the Mangled Cadavers 250623) killed 76 on 11-15 and 10 on 11-13.
  - It is a disease (SpellCategories DispelType 3) that ticks for 150 on bots of about 1000 health.
  - Several bots died after the fight in a `route_hold why=health` at 4-17% health. On 11-13 the leader of party 1 sat at 8%, then 7%, then died.
  - Nobody cures it: the module dispels only auras listed in the raid rule table, and map 2999 had no row.
- **The Baron** (250660) killed 21 on 11-15 (Knockout 17307: 14) and 7 on 11-13 (Knockout: 6, including one wipe of 5 Knockouts in 83 s).
- Pack 119 (5: 3 Mangled Cadavers, a Ragged Ghoul, a Skeletal Soldier) is over the 4-mob cap. It is 35 yards from the Baron's spawn, and he patrols (mv 2) within 20 yards of it. The shipped route walks into it from the north-west (points 20-21), with the Baron in reach.

**4. Other findings:**
- Revives: 367 on 11-15 (11-28 per party); 14 wipes in 11 parties; only 4 re-deaths within 30 s. There is no revive loop.
- Latent: the shipped legs 56-58 and 63-65 (after the last boss) cannot be walked on the navmesh.

## 2. Proposed routes

Both routes are in `data/dungeons-proposed/`. They load only through the existing key `DungeonDir`; the shipped files stay.

How they were made:
- with forever-ops `scripts/dungeon-study/ds_plan.py`, `DS_PLAN_KEY=plan_20261115`: boss to boss over the sim's navmesh, plus hand anchors, plus for rol an avoid circle of 27 yards around the statue;
- every leg from the entry was checked again with `navtool ... query` on the sim's mmaps:
  - hot: 16/16 legs `path ok`, 807 yards;
  - rol: 42/42 legs `path ok`, 1253 yards.

Exposure is counted the study's way: pull leash 30 yards, aggro radius, packs by the 10-yard assistance radius. Line of sight is not counted, and neither are the statue's summons.

| | route | points | yards | hostiles (elites) | danger | packs over the 4-mob cap | blocked legs | closest to the rol statue |
|---|---|---|---|---|---|---|---|---|
| hot | shipped | 36 | 1679 | 107 (87) | 227.8 | 1 (pack 12) | 0 | - |
| hot | **proposed** | 16 | **807** | **63 (58)** | **140.5** (-38%) | 1 (pack 12) | 0 | - |
| rol | shipped | 66 | 4506 | 242 (227) | 420.8 | 7 | 6 (56-58, 63-65) | **14.5 yards** (leg 36) |
| rol | 11-07 proposal | 35 | 1186 | 134 (124) | 230.0 | 6 | 0 | about 23 yards (points 23-24): **would trigger it** |
| rol | **proposed** | 42 | **1253** | **144 (134)** | **248.8** (-41%) | 7 | 0 | **33.1 yards** |

The 2026-11-07 boss-to-boss proposals were never applied. With today's world DB and mesh they give the same numbers as on 11-07:
- hot's (838 yards) still holds; the route here differs only in Plunder's place.
- rol's (1186 yards) does **not** hold. It walks the corridor past the statue at about 23 yards, inside the 25-yard trigger.

### 2.1 Hall of Thanes: Faldrim, Plunder, Magmatus, Durgen

Changes from the 11-07 proposal:
- Plunder is the second boss. He is killed where he patrols on the way (point 5), not on the way back from Magmatus. That is what half the parties did anyway, and it saves 31 yards.
- The west and east wing loops are gone.
- `kill_priority: [263397]` (Dark Iron Engineer). It acts only with the `focus` tactic, which the sim has. It is low value on the 11-15 data (Engineers killed 1).

| pt | x, y, z | yards | role | packs met on the leg to it (hostiles, danger) |
|---|---|---|---|---|
| 0 | -85.3, 0.0, 508.7 | 11 |  | 5 (1: Enraged Apparition); 1 hostiles, danger 1.8 |
| 1 | 19.5, 2.9, 500.2 | 116 |  | 3 (1: Faldrim Anvilmar, patrol), 6 (1: Tormented Soul), 7 (1: Tormented Soul), 8 (2: Enraged Apparition/Tormented Soul), 10 (3: Enraged Apparition/Tormented Soul), 15 (3: Enraged Apparition/Tormented Soul); 11 hostiles, danger 23.0 |
| 2 | 49.1, 8.8, 501.8 | 146 |  | 12 (10: Enraged Apparition/Tormented Soul); 4 hostiles, danger 8.2 |
| 3 | 61.1, 8.2, 501.6 | 158 | **Faldrim Anvilmar** | 12 (10: Enraged Apparition/Tormented Soul); 2 hostiles, danger 4.1 |
| 4 | 88.8, 2.9, 500.2 | 186 |  | 11 (1: Plunder, patrol), 12 (10: Enraged Apparition/Tormented Soul), 16 (3: Dark Iron Engineer/Dark Iron Looter); 5 hostiles, danger 11.2 |
| 5 | 129.9, 0.0, 500.2 | 228 | **Plunder** | 2 (4: Dark Iron Looter/Lesser Stone Golem, patrol); 1 hostiles, danger 2.3 |
| 6 | 134.1, -9.3, 500.2 | 238 |  |  |
| 7 | 149.3, -64.0, 493.0 | 295 |  | 13 (3: Bloodhound Runt/Dark Iron Looter), 21 (4: Dark Iron Engineer/Dark Iron Looter), 28 (4: Dark Iron Engineer/Dark Iron Looter/Dark, patrol); 9 hostiles, danger 20.8 |
| 8 | 164.2, -195.5, 486.3 | 429 | **Magmatus** | 32 (3: Dark Iron Engineer/Dark Iron Looter), 37 (1: Dark Iron Engineer), 38 (2: Dark Iron Engineer), 41 (1: Dark Iron Summoner), 42 (2: Dark Iron Summoner/Magmatus), 43 (4: Bloodhound Runt/Dark Iron Engineer/Dark ), 44 (1: Dark Iron Engineer), 45 (1: Dark Iron Summoner, patrol), 46 (1: Dark Iron Summoner, patrol); 16 hostiles, danger 37.8 |
| 9 | 148.5, -42.7, 499.1 | 583 |  | 1 (1: Dark Iron Looter), 40 (3: Bloodhound Runt); 2 hostiles, danger 3.8 |
| 10 | 156.3, -31.2, 502.1 | 597 |  | 13 (3: Bloodhound Runt/Dark Iron Looter); 2 hostiles, danger 3.8 |
| 11 | 177.3, -2.9, 500.2 | 632 |  | 9 (1: Lesser Stone Golem); 1 hostiles, danger 2.3 |
| 12 | 242.9, -2.7, 500.2 | 698 |  | 17 (2: Lesser Stone Golem); 2 hostiles, danger 4.7 |
| 13 | 304.0, -2.7, 500.2 | 759 |  | 14 (3: Dark Iron Looter/Dark Iron Summoner/Less); 3 hostiles, danger 7.9 |
| 14 | 323.2, -2.9, 500.2 | 778 |  | 4 (3: Durgen Dirgehammer/Lesser Stone Golem, patrol); 1 hostiles, danger 1.8 |
| 15 | 351.6, -9.8, 500.8 | 807 | **Durgen Dirgehammer** | 4 (3: Durgen Dirgehammer/Lesser Stone Golem, patrol), 19 (1: Dark Iron Looter); 3 hostiles, danger 7.1 |

Points 6-8 still pass the Engineer packs 21, 28 and 32.

Pack 12 (10 Apparitions and Souls before Faldrim) stays over the cap on both routes. The bots pull it in pieces, so it costs time, not lives. It is the longest stall in 17 of 20 runs.

### 2.2 Ruins of Lordaeron: Witherfang, the Baron, Bjork, Rath'mael, clear of the statue

- **Statue:** an avoid circle of 27 yards around the stalker. The route goes north around to Bjork (points 25-33, through 1536,384 and 1601,395) and comes back along the south side of the corridor to Rath'mael (anchors 1625,305 and 1600,300 at points 37 and 39). Its closest approach is 33.1 yards; the shipped route comes to 14.5.
- **Before the Baron:** three hand anchors (points 9, 10, 13).
  - The party comes in from the east and meets pack 116 alone at 30 yards.
  - Then it meets pack 119 with the Baron 60-77 yards away.
  - Then it pulls the Baron from 25 yards.
- **No gallery:** the route stays on the floor (z 61-65), and it does not walk on after the last boss.
- `kill_priority: [250623]` (Mangled Cadaver).
- `duration_sec` 2700: the 11-15 parties needed 1422-2112 s to reach the Baron on the shipped route.
- The boss order stays the shipped one. Rath'mael third was tried (`ds_plan`: 44 points, 1271 yards). His room opens to the east: the navmesh ray from the west corner (1523.7, 329.6) and from point 25 (1514.7, 320.0) is blocked, and the walk round is 288 yards. So the study's count of his 14-mob court (pack 99) at points 25-26 is through a wall.

| pt | x, y, z | yards | role | packs met on the leg to it (hostiles, danger) |
|---|---|---|---|---|
| 0 | 1781.3, 134.9, 70.9 | 27 |  |  |
| 1 | 1766.9, 118.9, 61.8 | 50 |  |  |
| 2 | 1749.3, 96.8, 61.8 | 79 |  | 29 (1: Deep Widow, patrol), 90 (3: Deep Widow), 91 (2: Broodling); 5 hostiles, danger 7.3 |
| 3 | 1749.9, 68.0, 61.8 | 110 |  | 59 (1: Witherfang, patrol), 60 (1: Tarantula, patrol), 89 (2: Broodling/Spider, patrol), 91 (2: Broodling), 93 (1: Spider), 95 (3: Spider); 7 hostiles, danger 10.2 |
| 4 | 1745.0, 58.4, 61.7 | 120 | **Witherfang** | 25 (3: Broodling), 95 (3: Spider); 5 hostiles, danger 6.6 |
| 5 | 1706.7, 64.0, 61.8 | 159 |  | 85 (3: Deep Widow/Spider); 3 hostiles, danger 4.6 |
| 6 | 1664.8, 82.1, 62.4 | 218 |  | 31 (1: Venom Lurker), 36 (3: Deep Widow/Spider), 49 (3: Deep Widow/Spider); 4 hostiles, danger 6.2 |
| 7 | 1636.0, 88.8, 61.8 | 248 |  | 49 (3: Deep Widow/Spider); 2 hostiles, danger 3.1 |
| 8 | 1623.2, 105.9, 61.8 | 274 |  | 41 (1: Venom Lurker, patrol), 44 (2: Broodwidow/Venom Lurker), 45 (1: Spider), 58 (1: Venom Lurker, patrol), 80 (2: Deep Widow/Spider); 7 hostiles, danger 11.4 |
| 9 | 1559.2, 127.3, 61.7 | 343 | stop 30 yards east of pack 116 (Ragged Ghoul, Skeletal Soldier): pull it alone | 62 (1: Venom Lurker), 116 (3: Ragged Ghoul/Skeletal Soldier); 2 hostiles, danger 3.1 |
| 10 | 1545.0, 140.0, 61.6 | 362 | stop 43 yards east of pack 119 (5: Mangled Cadavers, Ragged Ghoul, Skeletal Soldier; ov... | 116 (3: Ragged Ghoul/Skeletal Soldier); 2 hostiles, danger 3.0 |
| 11 | 1529.1, 139.5, 61.6 | 378 |  | 119 (5: Mangled Cadaver/Ragged Ghoul/Skeletal So); 2 hostiles, danger 3.4 |
| 12 | 1509.9, 142.1, 62.4 | 397 |  | 119 (5: Mangled Cadaver/Ragged Ghoul/Skeletal So); 3 hostiles, danger 5.2 |
| 13 | 1491.2, 153.9, 61.6 | 419 | Baron pull spot, 25 yards from his spawn, after 119 is dead | 23 (3: Mangled Cadaver/Skeletal Soldier), 110 (1: The Baron, patrol); 2 hostiles, danger 4.7 |
| 14 | 1470.4, 167.1, 61.6 | 444 | **The Baron** | 23 (3: Mangled Cadaver/Skeletal Soldier); 2 hostiles, danger 3.3 |
| 15 | 1490.9, 134.7, 63.0 | 482 |  |  |
| 16 | 1477.3, 107.7, 61.9 | 513 |  | 21 (2: Vengeful Citizen), 102 (2: Shrieking Banshee, patrol), 108 (1: Living Monstrosity, patrol), 115 (1: Living Monstrosity, patrol); 6 hostiles, danger 8.8 |
| 17 | 1464.8, 113.9, 62.4 | 529 |  | 34 (4: Shrieking Banshee/Skeletal Mage, patrol); 3 hostiles, danger 4.8 |
| 18 | 1450.7, 128.0, 62.4 | 548 |  | 34 (4: Shrieking Banshee/Skeletal Mage, patrol); 1 hostiles, danger 1.6 |
| 19 | 1441.6, 136.8, 62.4 | 561 |  |  |
| 20 | 1424.0, 160.5, 62.4 | 591 |  |  |
| 21 | 1417.6, 168.5, 63.2 | 601 |  | 69 (1: Ragged Ghoul); 1 hostiles, danger 1.5 |
| 22 | 1408.3, 200.0, 61.6 | 634 |  | 68 (1: Ghoul), 70 (1: Ghoul); 2 hostiles, danger 3.8 |
| 23 | 1422.1, 213.3, 64.3 | 655 |  | 71 (3: Fallen Necromancer/Mindless Undead); 2 hostiles, danger 4.0 |
| 24 | 1475.7, 278.4, 61.6 | 739 |  | 33 (2: Mindless Undead), 56 (1: Flesh Golem, patrol), 79 (1: Flesh Golem, patrol), 100 (3: Fallen Berserker/Mindless Undead, patrol), 104 (8: Fallen Necromancer/Ghoul/Ragged Ghoul), 105 (1: Fallen Necromancer, patrol), 106 (1: Skeletal Soldier, patrol), 107 (1: Flesh Golem, patrol), 109 (1: Flesh Golem, patrol), 111 (1: Flesh Golem), 113 (1: Fallen Berserker); 15 hostiles, danger 27.3 |
| 25 | 1514.7, 320.0, 61.9 | 798 |  | 99 (14: Ghoul/Rath'mael), 101 (4: Ghoul/Ragged Ghoul), 104 (8: Fallen Necromancer/Ghoul/Ragged Ghoul), 114 (1: Fallen Berserker), 120 (1: Fallen Berserker, patrol), 122 (2: Fallen Necromancer/Mindless Undead); 19 hostiles, danger 37.8 |
| 26 | 1536.0, 384.0, 61.6 | 865 |  | 64 (2: Fallen Necromancer/Skeletal Mage), 73 (4: Fallen Necromancer/Mindless Undead/Skele, patrol), 74 (2: Fallen Necromancer/Skeletal Soldier, patrol), 99 (14: Ghoul/Rath'mael); 6 hostiles, danger 12.4 |
| 27 | 1556.5, 394.7, 62.4 | 888 |  | 75 (6: Skeletal Mage/Skeletal Soldier, patrol), 117 (1: Flesh Golem, patrol), 118 (3: Flesh Golem/Skeletal Mage/Skeletal Soldi, patrol); 4 hostiles, danger 6.7 |
| 28 | 1568.8, 399.7, 62.4 | 901 |  |  |
| 29 | 1601.1, 395.2, 61.8 | 934 |  | 42 (1: Skeletal Soldier), 118 (3: Flesh Golem/Skeletal Mage/Skeletal Soldi, patrol); 2 hostiles, danger 3.5 |
| 30 | 1608.3, 385.9, 61.8 | 947 |  |  |
| 31 | 1620.5, 371.5, 63.7 | 966 |  | 38 (8: Plague Ghoul/Skeletal Mage/Skeletal Sold, patrol), 39 (2: Skeletal Soldier); 6 hostiles, danger 11.0 |
| 32 | 1664.0, 341.3, 61.8 | 1019 |  | 37 (1: Bjork, patrol), 38 (8: Plague Ghoul/Skeletal Mage/Skeletal Sold, patrol), 65 (5: Ghoul/Ragged Ghoul), 82 (2: Ragged Ghoul); 12 hostiles, danger 20.4 |
| 33 | 1677.6, 332.5, 61.8 | 1035 |  |  |
| 34 | 1700.3, 319.7, 61.8 | 1061 | **Bjork** |  |
| 35 | 1664.0, 298.7, 61.8 | 1103 |  | 83 (1: Ragged Ghoul); 1 hostiles, danger 1.5 |
| 36 | 1642.7, 298.7, 61.8 | 1129 |  | 81 (3: Ghoul/Ragged Ghoul), 123 (2: Ghoul/Ragged Ghoul); 5 hostiles, danger 8.7 |
| 37 | 1625.0, 305.0, 61.8 | 1148 | south side of the corridor: 33+ yards from the statue stalker (1601,342), whose 25-yard... |  |
| 38 | 1617.9, 312.8, 61.8 | 1159 |  |  |
| 39 | 1600.0, 300.0, 64.7 | 1181 | south side of the corridor, 42 yards from the statue | 77 (2: Ghoul/Ragged Ghoul); 1 hostiles, danger 1.5 |
| 40 | 1589.3, 302.9, 63.2 | 1193 |  | 77 (2: Ghoul/Ragged Ghoul), 96 (4: Ghoul/Ragged Ghoul); 4 hostiles, danger 7.2 |
| 41 | 1530.0, 311.6, 61.7 | 1253 | **Rath'mael** | 78 (1: Ghoul), 96 (4: Ghoul/Ragged Ghoul), 98 (7: Ghoul/Ragged Ghoul), 101 (4: Ghoul/Ragged Ghoul), 103 (1: Ghoul); 8 hostiles, danger 14.0 |

Packs over the cap on the proposed route:
- 38 (8, patrol);
- 65 (5);
- 75 (6, patrol, on the northern detour);
- 98 (7);
- 99 (14: Rath'mael's court, the boss fight);
- 104 (8);
- 119 (5: the Cadavers).

## 3. Changes on this branch

All of these are data, or tests of existing code. No new config key, no default changed, and the protocol is unchanged.

1. **`data/dungeons-proposed/2999-ruins-of-lordaeron.json` and `3065-hall-of-thanes.json`.**
   - They hold the routes of section 2, with `kill_priority` and notes.
   - They act only with `DungeonDir = <runtime>/data/dungeons-proposed`.
   - The same folder holds the Zul'Gurub file on `feature/zg-route-plan`. When both branches merge, `ZulGurubProposedRouteLoads` (`CHECK(json.size() == 1)`) must count 3 files or find its dungeon by id, as `RuinsAndHallProposedRoutesLoad` does.
2. **`data/raid_rules.txt`: `dispel|2999|16143|disease|Cadaver Worms (Mangled Cadaver)`.**
   - The party planner already assigns dispels from this table to any party (5-man too), with `PartyPlanner` on (the default) and `RaidRulesFile` set (the sim sets it). `TurnRaidRules` casts them in and out of combat.
   - The base roster's undead priest has Cure Disease (528, level 14).
   - The row acts wherever the file is loaded, on map 2999 only. To keep today's behaviour, drop the row.
3. **Tests.**
   - `dungeon_files_test` `RuinsAndHallProposedRoutesLoad`: both files load without warnings; it checks the boss points, the anchors, `kill_priority` and rol's 2700 s, and that no rol point is within 30 yards of the statue.
   - `raid_rules_test`: the shipped file has the row, as a disease.
   - `policy_test_10` `DungeonPartyCuresCadaverWormsOutOfCombat`: on map 2999, out of combat, the priest casts 528 on the diseased leader. It does not on map 309, and not without the row.
   - Run locally (`nice -n 19 ninja -j2`, SDK 1281, under the build lock): `ctest -R '^(policy|dungeon_files|raid_rules|settings)$'`, 4/4 passed. forever-ops `jenkins/ci/check-bots-data.py`: OK.

## 4. Ranked plan

| rank | what | why (numbers) | risk | where |
|---|---|---|---|---|
| 1 | rol: use the proposed route (`DungeonDir`) in the next rol+hot sim run | Away from the statue (all 20 11-15 parties stalled there: 330 of 453 deaths, 228 to The Abandoned). No gallery (69 recoveries in 19 of 20 runs). 144 instead of 242 hostiles. No blocked legs. | low: data only. Doors and line of sight unverified; the statue trigger is a 3D distance with LOS, and the margin is 8 yards | this branch, data |
| 2 | rol: cure Cadaver Worms (the dispel row) | 76 of 453 deaths (11-15), 10 of 23 (11-13) | low: existing code path; one row, map 2999 | this branch, `data/raid_rules.txt` |
| 3 | hot: use the proposed route | 20/20 already complete on the shipped route. The gain is time: 63 instead of 107 hostiles, so about 40% less fight time (estimate, median now 1714 s) | low | this branch, data |
| 4 | `kill_priority` Mangled Cadaver (rol), Dark Iron Engineer (hot) | fewer Worms stacks; the 11-13 dynamite cluster | low: acts only with `focus` (set on the sim) | this branch, proposed files |
| 5 | The statue as content: if the encounter should be done rather than avoided, add a `dungeon_events.txt` row or a boss block for The Abandoned (250631), with a rest gate before the 25-yard trigger and the wave adds (250618, 250627, 250630, 250624, 250626) in `kill_priority`. Today a 5-man of level 20 lost 11-25 members to it per party. | content, not progress | medium | `data/dungeon_events.txt`, `src/dungeon_events.*` |
| 6 | The Baron. Knockout (17307) caused 20 of the 28 Baron deaths, including one wipe. Read the tank's `tank_pickup:` and threat on him in the next run's journal first. Knockout is on the victim, so `tank_spot` or `frontal` rows do not help. | 21 + 7 deaths, 1 wipe | medium, cause unknown | `src/bot_policy_turn_cast.cpp` (`TurnTankPickup`), journal `tank_pickup:` |
| 7 | A scoped pull spot for pack 119, with the ZG branch's `pullpos_near` row (do not duplicate it here): `pullpos_near\|2999\|250623\|1545.0\|140.0\|61.6\|8\|1505\|159\|12\|rol Cadavers east of the Baron`. Not as a plain `pullpos` row: there are 7 Mangled Cadavers on the map (packs 23 and 119), and a map-wide row would drag every Cadaver pull to this spot (ZG plan, cause 4). | the Baron's room | low-medium; needs `feature/zg-route-plan` merged | `data/raid_rules.txt`, `raid_rules::Table::PullPositionOf(map, entry, x, y)` |
| 8 | Members who cannot follow the leader down a drop (the rol gallery). The proposed route avoids it, but the code gap stays: `regroup_stuck` fires after 10 s at 30 yards, then `member_recover` teleports. A follower that walks to the leader's last route point instead of the leader's position would fix it elsewhere too. | 20 + 69 recoveries (shipped route only) | medium | `src/bot_policy_turn_move.cpp` (follow and regroup), `member_recover` |
| 9 | A study-tool check for scripted triggers: list `smart_scripts` event 10 (OOC LOS) and areatrigger rows within 30 yards of a route, and summons in `smart9` action lists. That is how the statue was missed on 11-07. | finds the next statue | low | forever-ops `scripts/dungeon-study/ds_data.py` |
| 10 | Revive and ghost holds: not needed here (4 re-deaths within 30 s in 20 rol runs). If a run shows a loop, use the ZG branch's `GhostHoldPartyFightMaxMs` and `DungeonRevivesPerMinute` (default 0) rather than new keys. | none observed | - | `feature/zg-route-plan` |
| 11 | Ship the routes after a passing sim run: copy the proposed files over `data/dungeons/`, and the matching blocks in `data/dungeons.txt` (`ShippedFilesEqualTheOldTextFiles` compares them); raise rol's `duration_sec` | | low | `data/dungeons/*.json`, `data/dungeons.txt` |

## 5. How to test it (not run)

Use the forever-ops `scripts/sim-overnight.sh` step `all4` (`ONLY=all4`): rol and hot, 20 parties each, 3600 s each (`DUNGEON_LEN`). Do two runs, each with a build that contains this branch:
- **A (shipped routes, plus the dispel row):** the step as it is. The row comes with the runtime `raid_rules.txt`.
- **B (proposed routes):** the same, plus the extra key `Module.forever-bots.DungeonDir = /srv/forever/sim/modules_runtime/forever-bots/data/dungeons-proposed`.
  - Add an `all4p` case with that key in the step's `extra` argument.
  - `DungeonDir` also swaps in any other file in that folder (Zul'Gurub after the ZG merge). That does not matter for a rol+hot run.

What to read:
- the result lines `scenario rol#N` and `scenario hot#N`: `outcome`, `bosses`, `killed`, `boss_ms`, `deaths`, `wipes`, `revived`, `time_s fight:/rest:/dead:`;
- the journal (`ds_journal_fetch.sh` from the run's start, then `ds_journal.py` and `ds_plan.py` with `DS_PLAN_KEY=plan_20261115`):
  - `route_walk point=` (the leader's index);
  - `died:` with `killer_entry` 250631 (The Abandoned), 250623 / `killer_spell=16143` (Worms) and 250660 / 17307 (the Baron);
  - `member_recover` with `from=` z (z >= 78 is the gallery);
  - `regroup_stuck`;
- `raid_dispel: ... spell=528` on map 2999. This event type is not in `ds_journal_fetch.sh`'s list: grep the sim journal for it.

Pass:
- **rol B:**
  - 0 deaths to 250631 (A: 228 in 11-15);
  - at least 3/4 bosses in 15 of 20 parties, and 4/4 in at least 8 (11-15 shipped: 3/4 or better in 6 of 20, 4/4 in 1);
  - the leader beyond point 34 (Bjork) in at least 15 of 20;
  - 0 `member_recover` from z >= 78 (11-15: 69).
- **rol A and B:** Cadaver Worms deaths at most 1.3 per party (a third of 11-15's 76 in 20), and `raid_dispel` lines with `spell=528`.
- **hot B:** 4/4 in at least 18 of 20, median run at most 1300 s (11-15 shipped: 1714 s), and deaths per party no higher than A's.

Fail:
- rol B with any death to 250631: the route came within the trigger (move anchors 37 and 39, or the northern points 26-31, further out);
- `route_stall` or `regroup_stuck` piling up at the anchors 9, 10, 13, 37 or 39;
- hot B slower than A.

## 6. Not verified

- Line of sight and doors on every proposed leg (navmesh paths only; no vmaps). Whether the statue's 25-yard check also needs line of sight from its z 67 (the margin assumes it does not).
- That the northern detour to Bjork (points 25-31) is open. It passes the wave summon spots at 1619-1627, 391-397, which hold nothing until the statue fires, and pack 75 (6, patrol).
- That the leader really pulls pack 119 from the east at points 10-12 before the Baron comes (patrol timing).
- That the bot priest knows Cure Disease (528) at level 20 in the sim, and casts it during a `route_hold why=health`.
- The cause of the Baron's Knockout deaths.
- Any sim run of the proposed routes or of the dispel row.
