# Ruins of Lordaeron and Hall of Thanes: diagnosis, proposed routes and a plan (2026-11-15)

Branch `feature/rol-hot-route-plan` (from `origin/test` a192123). This is a proposal. The shipped routes `data/dungeons/2999-ruins-of-lordaeron.json` and
`data/dungeons/3065-hall-of-thanes.json` are unchanged, and so are all key defaults.

Sources:
- the forever-ops route study (branch `docs/rol-hot-studies`): `docs/dungeon-studies/{rol,hot}_route_study.html`, `{rol,hot}_route_proposal.txt`, the `rol` and
  `hot` entries of `scripts/dungeon-study/dungeons.json` (hand plan `plan_20261115`);
- the sim's world DB (read-only SELECTs, `ds_fetch.sh`, 2026-11-15) and the sim's mmaps for maps 2999 and 3065 (the local copy of `3065_31_31` and `3065_32_32`
  differs from the sim's; the sim's tiles were used);
- the sim journal (`ds_journal_fetch.sh`): the old cycles 2026-11-13 16:26 and 17:00 (3 parties each per dungeon), and the overnight run 4 of 2026-11-15
  (section 1.3: which numbers come from it is said there);
- `/var/log/forever/sim/scenario-results.log` (outcome lines);
- the module code on `origin/test` and the Zul'Gurub plan on `feature/zg-route-plan` (`doc/zulgurub-route-plan.md`, not merged).

Nothing was changed or run on the sim.

## 1. Diagnosis

### 1.1 Hall of Thanes (hot, map 3065, levels 13-18): the route is slow, not deadly

Old cycles (2026-11-13, 6 parties of the base roster `2:1,5:5,8:8,2:9,5:4`, level 18):

| cycle | length | outcome | bosses | leader index | fight / walk / rest / dead (s) |
|---|---|---|---|---|---|
| 16:26 | 1800 s | 3 x complete | 4/4 in 1433, 1489, 1526 s | 30-32/36 | 1000-1163 / 225-245 / 48-279 / 0 |
| 17:00 | 1426 s (the cycle was cut) | 3 x timeout | 3/4 (Durgen missing) | 24-31/36 | 910-1046 / 190-237 / 142-324 / 0 |

- The shipped route finishes, but only just: the three complete runs used 80-85% of 1800 s. Fight time is 64-76% of every run. With 107 hostiles on the
  route that is about 10 s of fight per hostile.
- Deaths are few: 10 in the outcome lines (0-4 per party), no wipe, no ghost, no re-death within 30 s. On the map the killers are the Dark Iron Engineer
  (263397, Throw Dynamite 1294260: 6 deaths, all in one party in 36 s at (147..159, -61..-112), shipped points 23-24), Magmatus 2, Plunder 2 (Knock Away),
  Dark Iron Summoner 1.
- Leader plateaus (longest index stall per run): point 3 (130-203 s, the 10-mob Apparition/Soul room before Faldrim, pack 12), point 16 (162 s, Magmatus),
  point 23 (157 s, the Engineers), point 27 (170 s). Point 13 (the west-wing loop) costs 111-157 s in every run.
- Route problem: the shipped route loops through the west wing (points 7-12) and the east wing to Magmatus (13-18): 1679 yards and 107 hostiles for 4 bosses.
  Pack/tactics: the Engineers' dynamite. Bot behaviour: nothing that stands out (no revive loop, no stuck members).
- Plunder patrols (mv 2) through the hall between Faldrim and Magmatus: in 2 of the 3 17:00 runs he was killed second, before Magmatus.

### 1.2 Ruins of Lordaeron (rol, map 2999, levels 15-20): too long a route, a gallery the party cannot follow down, and the Baron's room

Old cycles (2026-11-13, 6 parties, level 20):

| cycle | length | outcome | bosses | leader index | fight / walk / rest / dead (s) |
|---|---|---|---|---|---|
| 16:26 | 1800 s | 3 x timeout | 2/4 (Witherfang at 372-448 s, the Baron at 1257-1525 s) | 25-29/66 | 1136-1244 / 266-316 / 209-321 / 8-52 |
| 17:00 | 1426 s | 3 x timeout | 1/4 (Witherfang at 337-388 s) | 16-22/66 | 985-1205 / 137-268 / 82-166 / 0-8 |

- No party got past the Baron (shipped point 22 of 66). At the 16:26 pace (83 hostiles met by the Baron in about 1300-1500 s) the shipped route's 242
  hostiles need well over an hour.
- **Gallery (route problem):** shipped points 14-17 climb to the upper gallery (z 83) and point 18 drops back to the floor (z 62). In 5 of 6 runs all four
  members were left on the gallery: 20 of the 29 `member_recover` teleports of the rol leaders' parties came from z >= 78 (4 per run), with
  `regroup_stuck yards=30` before them. The leader index sits at point 18 for 201-267 s in 4 runs (and 488 s at point 16 in one).
- **The Baron's room (pack/tactics and bot behaviour):** 23 deaths on the map, all at shipped points 20-29 around (1485, 135..180):
  - Mangled Cadaver (250623) 10: every one by Cadaver Worms (16143, a disease, SpellCategories DispelType 3; the smart script casts it every 6 s on the
    victim, ticks of 150 on bots of about 1000 health). Several died after the fight in a `route_hold why=health` at 4-17% health (leader 16 at 16:53:09-45:
    8%, 7%, dead). Nobody cured the disease: the module dispels only auras listed in the raid rule table, and map 2999 had no row.
  - The Baron (250660) 7: Knockout (17307) 6, Trample (5568) 1. One party wiped on him (17:24:49-17:26:12, 5 deaths, all Knockout).
  - Pack 119 (5: 3 Mangled Cadavers, a Ragged Ghoul, a Skeletal Soldier) is over the 4-mob cap and 35 yards from the Baron's spawn; he patrols
    (mv 2) within 20 yards of it. The shipped route walks into it from the north-west (points 20-21) with the Baron in reach.
- No revive loop: 0 re-deaths within 30 s of the same bot, `revived` 0-7 per run.
- Latent: legs 56-58 and 63-65 (after the last boss) cannot be walked on the navmesh.

### 1.3 The overnight run 4 (2026-11-15, rol and hot, 20 parties each, 3600 s)

NEW_CYCLES_PLACEHOLDER

## 2. Proposed routes

Both are in `data/dungeons-proposed/` (only loaded through the existing key `DungeonDir`; the shipped files stay). They were made with forever-ops
`scripts/dungeon-study/ds_plan.py`, `DS_PLAN_KEY=plan_20261115` (boss to boss over the sim's navmesh, plus hand anchors), and every leg from the entry was checked
again with `navtool ... query` on the sim's mmaps: hot 16/16 legs `path ok` (807 yards), rol 39/39 legs `path ok` (1189 yards). Exposure is counted the study's
way (pull leash 30 yards, aggro radius, packs by the 10-yard assistance radius; no line of sight).

| | route | points | yards | hostiles (elites) | danger | packs | packs over the 4-mob cap | blocked legs |
|---|---|---|---|---|---|---|---|---|
| hot | shipped | 36 | 1679 | 107 (87) | 227.8 | 46 | 1 (pack 12) | 0 |
| hot | **proposed** | 16 | **807** | **63 (58)** | **140.5** (-38%) | 30 | 1 (pack 12) | 0 |
| rol | shipped | 66 | 4506 | 242 (227) | 420.8 | 115 | 7 | 6 (56-58, 63-65) |
| rol | **proposed** | 39 | **1189** | **134 (124)** | **230.0** (-45%) | 63 | 6 | 0 |

The 2026-11-07 boss-to-boss proposals (hot 838 yards, rol 1186 yards) were never applied. They still hold with the new data (the world DB and the mesh give the
same numbers today); the two routes here are those proposals with two changes, below.

### 2.1 Hall of Thanes: Faldrim, Plunder, Magmatus, Durgen

Change from the 11-07 proposal: Plunder is the second boss, killed where he patrols on the way (point 5), not on the way back from Magmatus. That is what 2 of
the 3 17:00 parties did anyway, and it saves 31 yards. The west and east wing loops are gone. `kill_priority: [263397]` (Dark Iron Engineer; acts only with
the `focus` tactic, which the sim has).

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

Points 6-8 still pass the Engineer packs 21, 28 and 32 (the 6-death dynamite cluster of the 17:00 run). Pack 12 (10 Apparitions and Souls before Faldrim) stays
over the cap on both routes; the bots pulled it in pieces (Enraged Apparition 7-9 pulls, Tormented Soul 4-13 per run), so it costs time, not lives.

### 2.2 Ruins of Lordaeron: Witherfang, the Baron, Bjork, Rath'mael

Changes from the 11-07 proposal: three hand anchors before the Baron (points 9, 10, 13), so the party comes in from the east, meets pack 116 alone at 30 yards,
then pack 119 with the Baron 60-77 yards away, and pulls the Baron from 25 yards after both are dead. `kill_priority: [250623]` (Mangled Cadaver).
`duration_sec` 2700 (134 hostiles at the 11-13 pace, about 16 s each including walk and rest, do not fit 1800 s).

The boss order stays the shipped one. Rath'mael third was tried (`ds_plan`: 44 points, 1271 yards, 132 hostiles), but his room opens to the east: from the
corner at point 25 (1523.7, 329.6), 19 yards from him, the navmesh ray is blocked and the walk is 288 yards. So the route passes his 14-mob court (pack 99)
through a wall on the way to Bjork and comes back to it from the east; the study counts pack 99 at point 25 because it ignores walls. No gallery: the route
stays on the floor (z 61-64).

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
| 25 | 1523.7, 329.6, 61.6 | 811 |  | 64 (2: Fallen Necromancer/Skeletal Mage), 99 (14: Ghoul/Rath'mael), 101 (4: Ghoul/Ragged Ghoul), 104 (8: Fallen Necromancer/Ghoul/Ragged Ghoul), 114 (1: Fallen Berserker), 120 (1: Fallen Berserker, patrol), 122 (2: Fallen Necromancer/Mindless Undead); 21 hostiles, danger 41.6 |
| 26 | 1536.0, 336.8, 61.6 | 825 |  | 78 (1: Ghoul); 1 hostiles, danger 1.9 |
| 27 | 1547.5, 340.8, 61.9 | 837 |  | 96 (4: Ghoul/Ragged Ghoul); 3 hostiles, danger 5.3 |
| 28 | 1578.7, 341.3, 61.9 | 868 |  | 96 (4: Ghoul/Ragged Ghoul); 1 hostiles, danger 1.5 |
| 29 | 1617.9, 371.7, 61.8 | 918 |  | 38 (8: Plague Ghoul/Skeletal Mage/Skeletal Sold, patrol), 39 (2: Skeletal Soldier), 42 (1: Skeletal Soldier); 4 hostiles, danger 7.5 |
| 30 | 1664.0, 341.3, 61.8 | 975 |  | 37 (1: Bjork, patrol), 38 (8: Plague Ghoul/Skeletal Mage/Skeletal Sold, patrol), 65 (5: Ghoul/Ragged Ghoul), 82 (2: Ragged Ghoul); 14 hostiles, danger 23.9 |
| 31 | 1677.6, 332.5, 61.8 | 991 |  |  |
| 32 | 1700.3, 319.7, 61.8 | 1017 | **Bjork** |  |
| 33 | 1676.3, 324.8, 61.8 | 1041 |  | 83 (1: Ragged Ghoul); 1 hostiles, danger 1.5 |
| 34 | 1653.9, 325.1, 61.8 | 1064 |  | 81 (3: Ghoul/Ragged Ghoul); 2 hostiles, danger 3.4 |
| 35 | 1618.9, 317.6, 61.8 | 1100 |  | 81 (3: Ghoul/Ragged Ghoul), 123 (2: Ghoul/Ragged Ghoul); 3 hostiles, danger 5.3 |
| 36 | 1559.5, 308.8, 61.9 | 1160 |  | 77 (2: Ghoul/Ragged Ghoul), 98 (7: Ghoul/Ragged Ghoul); 5 hostiles, danger 8.7 |
| 37 | 1539.2, 310.4, 61.9 | 1180 |  | 101 (4: Ghoul/Ragged Ghoul); 2 hostiles, danger 3.4 |
| 38 | 1530.0, 311.6, 61.7 | 1189 | **Rath'mael** | 103 (1: Ghoul); 1 hostiles, danger 1.9 |

Packs over the cap on the proposed route: 38 (8, patrol), 65 (5), 98 (7), 99 (14: Rath'mael's court, the boss fight), 104 (8), 119 (5: the Cadavers).

## 3. Changes on this branch

All of them are data, or a check of existing code. No new config key, no default changed, protocol unchanged.

1. **`data/dungeons-proposed/2999-ruins-of-lordaeron.json` and `3065-hall-of-thanes.json`**: the routes of section 2 with `kill_priority` and notes. They act only
   with `DungeonDir = <runtime>/data/dungeons-proposed`. The folder also holds the Zul'Gurub file on `feature/zg-route-plan`: when both branches merge,
   `ZulGurubProposedRouteLoads` (`CHECK(json.size() == 1)`) must count 3 files or find its dungeon by id, as `RuinsAndHallProposedRoutesLoad` does.
2. **`data/raid_rules.txt`: `dispel|2999|16143|disease|Cadaver Worms (Mangled Cadaver)`**. The party planner already assigns dispels from this table to any
   party (5-man too) with `PartyPlanner` on (the default) and `RaidRulesFile` set (the sim sets it), and `TurnRaidRules` casts them in and out of combat. The
   base roster's undead priest has Cure Disease (528, level 14). This row does act wherever the file is loaded (map 2999 only); to keep today's behaviour,
   drop the row.
3. Tests: `dungeon_files_test` `RuinsAndHallProposedRoutesLoad` (both files load without warnings; boss points, the anchors, `kill_priority`, rol's 2700 s);
   `raid_rules_test` (the shipped file has the row, disease); `policy_test_10` `DungeonPartyCuresCadaverWormsOutOfCombat` (on map 2999, out of combat, the
   priest casts 528 on the diseased leader; not on map 309; not without the row).

Run locally (`nice -n 19 ninja -j2`, SDK 1281, under the build lock): `ctest -R '^(policy|dungeon_files|raid_rules|settings)$'`, 4/4 passed.
forever-ops `jenkins/ci/check-bots-data.py`: OK (13 dungeons, 18 events, 326 config keys).

## 4. Ranked plan

| rank | what | why (numbers) | risk | where |
|---|---|---|---|---|
| 1 | Use the proposed routes (`DungeonDir`) in the next rol+hot sim run | rol: no gallery (20 of 29 member recoveries), 134 instead of 242 hostiles, no blocked legs; hot: 63 instead of 107 hostiles, about 40% less fight time | low: data only; doors and line of sight unverified | this branch, data |
| 2 | Cure Cadaver Worms (the dispel row) | 10 of 23 rol deaths, several after the fight in a health hold | low: existing code path; one row, map 2999 | this branch, `data/raid_rules.txt` |
| 3 | `kill_priority` Mangled Cadaver (rol), Dark Iron Engineer (hot) | fewer Worms stacks; the 6-death dynamite cluster | low: acts only with `focus` (set on the sim) | this branch, proposed files |
| 4 | The Baron: a tank that keeps him. One wipe (5 x Knockout 17307) and 6 of 7 Baron deaths were Knockout on whoever held him after the tank. Look first at `tank_pickup` / `TurnTankPickup` and the tank's threat on him in the run-4 journal; if the tank died first, a raid-rule `tank_spot` or `frontal` row does not help (Knockout is on the victim). A `noaoe`-style rule is not needed. | 7 deaths, 1 wipe in 6 runs | medium, unknown cause | `src/bot_policy_turn_cast.cpp` (`TurnTankPickup`), journal `tank_pickup:` |
| 5 | Scoped pull spot for pack 119 with the ZG branch's `pullpos_near` row (do not duplicate it here): `pullpos_near|2999|250623|1545.0|140.0|61.6|8|1505|159|12|rol Cadavers east of the Baron` (and the same for 250622, 250626 if the pack still comes with the Baron). Not as a plain `pullpos` row: there are 7 Mangled Cadavers on the map (packs 23 and 119), and a map-wide row would drag every Cadaver pull to this spot (ZG plan, cause 4). | the Baron's room | low-medium; needs `feature/zg-route-plan` merged | `data/raid_rules.txt`, `raid_rules::Table::PullPositionOf(map, entry, x, y)` |
| 6 | Members who cannot follow the leader down a drop (rol gallery). The proposed route avoids it; the code gap stays: `regroup_stuck` fires after 10 s at exactly 30 yards, then `member_recover` teleports. A follower path that uses the leader's last route point instead of the leader's position would fix it for other dungeons. | 20 recoveries in 5 of 6 rol runs (shipped route only) | medium | `src/bot_policy_turn_move.cpp` (follow / regroup), `member_recover` |
| 7 | Revive and ghost holds: not needed here (0 re-deaths within 30 s in rol and hot). If a run-4 wipe shows a loop, use the ZG branch's `GhostHoldPartyFightMaxMs` and `DungeonRevivesPerMinute` (default 0) rather than new keys. | none observed | - | `feature/zg-route-plan` |
| 8 | Ship the routes: after a passing sim run, copy the proposed files over `data/dungeons/` and the matching blocks in `data/dungeons.txt` (`ShippedFilesEqualTheOldTextFiles` compares them), and raise rol's `duration_sec`. | | low | `data/dungeons/*.json`, `data/dungeons.txt` |

## 5. How to test it (not run)

Use the forever-ops `scripts/sim-overnight.sh` step `all4` (`ONLY=all4`: rol and hot, 20 parties each, `DUNGEON_LEN` 3600 s each). Two runs, each with a build that contains
this branch:
- **A (shipped routes, new data rows):** the step as it is (the dispel row comes with the runtime `raid_rules.txt`).
- **B (proposed routes):** the same plus the extra key `Module.forever-bots.DungeonDir = /srv/forever/sim/modules_runtime/forever-bots/data/dungeons-proposed`
  (add an `all4p` case with that extra key, or pass it through the step's `extra` argument). Note that `DungeonDir` also swaps in any other file in that folder
  (Zul'Gurub after the ZG merge), which does not matter for a rol+hot run.

What to read:
- the result lines `scenario rol#N` and `scenario hot#N` in `/var/log/forever/sim/scenario-results.log`: `outcome`, `bosses`, `killed`, `boss_ms`, `furthest`,
  `deaths`, `wipes`, `time_s fight:/walk:/rest:/dead:`;
- the journal (`ds_journal_fetch.sh` from the run's start, then `ds_journal.py` and `ds_plan.py`): `route_walk point=` (the leader's index), `route_hold why=`,
  `died:` with `killer_entry` and `killer_spell` (16143, 17307, 1294260), `regroup_stuck`, `member_recover` (with `from=` z), `revived`;
- `raid_dispel: ... spell=528` lines on map 2999 (the cure; this event type is not in `ds_journal_fetch.sh`'s list, grep the sim journal for it).

Pass:
- hot (B): 4/4 bosses in at least 18 of 20 parties, median run under 1200 s (shipped: 1433-1526 s with 1800 s), no more deaths per party than A.
- rol (B): 4/4 bosses in at least half the parties within 3600 s, leader at the last point (38) in those; at least 3/4 in 15 of 20.
- rol (A and B): Cadaver Worms deaths at most a third of the per-party rate of the 11-13 runs (10 in 6 parties: under 0.6 per party), and `raid_dispel`
  lines with `spell=528` present.
- rol (B): `member_recover` from z >= 78: 0 (the gallery is gone).
Fail: rol B below 2/4 bosses in median (no better than the 11-13 shipped runs), or `route_stall` / `regroup_stuck` piling up at the anchors 9, 10 or 13,
or hot B slower than A.

## 6. Not verified

- Line of sight and doors on every proposed leg (navmesh paths only; no vmaps).
- That the leader really pulls pack 119 from the east at points 10-12 before the Baron comes (his patrol timing).
- That the bot priest knows Cure Disease (528) at level 20 in the sim (spell learning) and casts it during a `route_hold why=health`.
- The Baron's Knockout deaths: whether the tank lost him or died first.
- Any sim run of the proposed routes or of the dispel row.
