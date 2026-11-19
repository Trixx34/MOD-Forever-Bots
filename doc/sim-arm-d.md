# Sim arm D: keys and counts (2026-11-19)

Branch `integrate/arm-d-20261119` = `origin/test` (938e7ff) + four branches, in this order:

1. `fix/graveyard-death-loop-20261119` (DeathLoopBreaker)
2. `fix/arm-c-small-20261119` (level-safe line throttle, z-aware auction stand point, below-map false positives)
3. `feature/skyborne-exit-quests-20261119` (Skyborne exit quests, make room)
4. `feature/level-dungeon-early-20261119` (level dungeon rules)

Every new key defaults off (0 or empty) except `LevelSafeLogThrottleSec`, which is logging only and defaults to 60.
The prod column is what prod gets when this build reaches it (add the keys with these values, as the add-all-keys rule asks); nothing here is switched on
for prod.

## Keys

All module keys take the prefix `Module.forever-bots.` in a drop-in (for example a new `zz-arm-d.conf` in
`/srv/forever/sim/conf/worldserver.conf.d/modules.d/`, beside the `zz-arm-c.conf` of arm C).

| key | branch | default | sim (arm D) | prod | events to count (journal) |
|---|---|---|---|---|---|
| `DeathLoopBreaker` | death loop | 0 | 1 | 0 | `death_loop_break` per `stage=` (detect, yard_unsafe, yard, own_yard, wait, wait_over, yard_held, escape, revived_away, in_combat, no_spot); deaths per bot-hour and the `deaths_top` line (the Raven Hill loop: same creature entry, same graveyard) against arm C |
| `LevelSafeLogThrottleSec` | arm C small | 60 | 60 | 60 | `deaths_top` fields `level_safe_lines=` and `level_safe_held=`; the count of `quest_walk_blocked reason=level_unsafe`, `level_unsafe_escape result=no_spot`, `level_safe_yard result=none_known` lines (arm C: bot 225 logged 4,655 level_unsafe lines) |
| `BelowMapRecover` | (on test since 938e7ff; changed by arm C small) | 0 | 1 | 0 | `below_map_recover reason=z_below\|no_route_storm`, `below_map_skip reason=on_transport\|swimming\|on_path\|near_ground` (false positives held back) |
| `BelowMapDropYards` | (on test) | 60 | 100 | 60 (default; only read when BelowMapRecover = 1) | `below_map_recover reason=z_below` per hour and per bot against arm C (5,148 on 376 bots with 60 there), and each recovered bot's `no_route` count after it |
| `AuctionWalk3D` | (on test since 938e7ff; z-aware stand point from arm C small) | 0 | 1 | 0 | `auction_reach check=ok_3d\|failed_3d\|skip_bad_z\|ignore_z` (with `where=errand`), `errand_failed reason=auctioneer_bad_z`, `errand_failed reason=auction_reach_3d`; `listed` lines per hour and `list_refused` / `buy_failed` (with the dist/z fields) against arm C (194 `failed_3d` there) |
| `SkyborneExitQuests` | Skyborne | 0 | 1 | 0 | `skyborne_hop_start`, `skyborne_hop_done`, `skyborne_hop_refused` (a refused teleport blocks the quest 1 h, then 6 h, then for good); bots on map 2991 above the level of its quests that reach map 0 or 1 |
| `SkyborneExitMakeRoom` | Skyborne | 0 | 1 | 0 | `skyborne_make_room`; quest accept refusals for full bags (reason 9) on map 2991 |
| `LevelDungeonEarlyLevels` | level dungeon | 0 | 3 | 0 | `level_dungeon_due ... early=` (rfc/hot from 15, rol from 17, dm from 20, wc from 22); level dungeon runs started and their results |
| `LevelDungeonSides` | level dungeon | empty | `rfc:horde,rol:horde,hot:alliance,stockade:alliance` | empty | `level_dungeon_skip reason=wrong_side`; `level_dungeon_due ... side=`; no Horde party in hot or stockade, no Alliance party in rfc or rol |
| `LevelDungeonFileDuration` | level dungeon | 0 | 0 | 0 | none (kept at the fixed 45-minute cap; with 1 most shipped files would cut runs to 30 minutes) |
| `LevelDungeonNeedRoles` | level dungeon | 0 | 0 | 0 | none in arm D; `level_dungeon_skip reason=no_tank_healer` shows only with 1 |
| `LevelDungeonLog` | level dungeon | 0 | 1 | 0 | `level_dungeon_due` and `level_dungeon_skip reason=` per reason (no_dungeon, wrong_side, cleared, runs_used, same_level, too_few, not_grouped, leader_busy, member_not_ready, no_tank_healer) |

Core key, not a module key: `Bot.Actions.Teleport = 1` in the core config is needed by DeathLoopBreaker (graveyard and grind spot moves; the grind spot move walks
when refused), BelowMapRecover and SkyborneExitQuests (the hop to map 0 or 1). The sim already has it at 1 for arm C (below-map recovery); keep it. Prod: leave
it as it is; none of the arm D features is on there.

Sim drop-in text (arm D, on top of arm C's `zz-arm-c.conf`):

```
Module.forever-bots.DeathLoopBreaker = 1
Module.forever-bots.LevelSafeLogThrottleSec = 60
Module.forever-bots.BelowMapRecover = 1
Module.forever-bots.BelowMapDropYards = 100
Module.forever-bots.AuctionWalk3D = 1
Module.forever-bots.SkyborneExitQuests = 1
Module.forever-bots.SkyborneExitMakeRoom = 1
Module.forever-bots.LevelDungeonEarlyLevels = 3
Module.forever-bots.LevelDungeonLog = 1
Module.forever-bots.LevelDungeonSides = rfc:horde,rol:horde,hot:alliance,stockade:alliance
Module.forever-bots.LevelDungeonFileDuration = 0
Module.forever-bots.LevelDungeonNeedRoles = 0
```

A key already set in another drop-in (for example `BelowMapRecover` in `zz-arm-c.conf`) must be set in one file only: the core refuses a duplicate key name
(the 2026-11-12 crash loop on `zz-dungeon-run.conf`). Change it there, or remove it there before adding it here.

Prod values (add with the deploy that brings this build, in the prod drop-in):

```
Module.forever-bots.DeathLoopBreaker = 0
Module.forever-bots.LevelSafeLogThrottleSec = 60
Module.forever-bots.SkyborneExitQuests = 0
Module.forever-bots.SkyborneExitMakeRoom = 0
Module.forever-bots.LevelDungeonEarlyLevels = 0
Module.forever-bots.LevelDungeonSides =
Module.forever-bots.LevelDungeonFileDuration = 0
Module.forever-bots.LevelDungeonNeedRoles = 0
Module.forever-bots.LevelDungeonLog = 0
```

(`BelowMapRecover`, `BelowMapDropYards` and `AuctionWalk3D` came with 938e7ff; prod keeps 0, 60 and 0.)

## The sim dungeon folder warning

The sim log says:

```
forever-bots: dungeons: dungeon folder '/srv/forever/sim/dungeons-mine' cannot be read
```

Cause, from the code: `LoadDungeonSet` (`src/dungeon_files.cpp`) reads `DungeonDir` with `required = true`; `LoadDungeonDir` writes this warning when the
value is not empty and is not a directory the server can open (`std::filesystem::is_directory` fails: missing, or no permission). `/srv/forever/sim/dungeons-mine`
is the example path from `config/modules.d/forever-bots.conf.example` (line `# Module.forever-bots.DungeonDir = /srv/forever/sim/dungeons-mine`, a comment)
and from `doc/dungeon-strategy.md` ("Make a folder for your files, for example ..."). The 2026-11-15 add-all-keys drop-in `zz-sim-new-keys.conf`
(forever-ops `docs/sim-zz-sim-new-keys-20261115.conf`, line 361) copied it as a real value:

```
Module.forever-bots.DungeonDir = /srv/forever/sim/dungeons-mine
```

No own dungeon files are kept on the sim, so the folder was never made. The shipped files are not affected by this warning: they come from `DungeonDataDir`, or,
when that is empty, from the folder `dungeons` beside `DungeonFile`. But nothing is due for level dungeons unless a dungeon is loaded (`doc/dungeons.md`), and
arm D tests the level dungeon rules, so the shipped files must load.

Config change for the sim (in `/srv/forever/sim/conf/worldserver.conf.d/modules.d/zz-sim-new-keys.conf`, dated `.bak-<date>` copy first, while no sim run is
going; the sim needs a restart or a config reload):

```
# was: Module.forever-bots.DungeonDir = /srv/forever/sim/dungeons-mine   (the example path; the folder does not exist)
Module.forever-bots.DungeonDir =
```

and, in the same file (if neither key is set in any sim drop-in yet; one file only), name the shipped dungeon data so level dungeons have dungeons outside a
scenario run as well:

```
Module.forever-bots.DungeonDataDir = /srv/forever/sim/modules_runtime/forever-bots/data/dungeons
```

(`tools/sim_dungeon_run.sh` sets `DungeonFile = /srv/forever/sim/modules_runtime/forever-bots/data/dungeons.txt` for scenario runs; that also finds the shipped
folder beside it. If `DungeonFile` is already set in a leveling drop-in, `DungeonDataDir` is not needed.)

Check after the restart: no `dungeon folder ... cannot be read` line; `forever-bots: dungeons: N from /srv/forever/sim/modules_runtime/forever-bots/data/dungeons`
and `forever-bots: tank lead knows N dungeon(s)` with N > 0 (15 JSON files ship today). If own files are wanted later instead: make the folder, readable by the
user the sim worldserver runs as, and put the `DungeonDir` value back.

Not verified from here (no host access in this slice): which sim drop-ins set `DungeonFile` or `DungeonDataDir` today, and whether the folder exists with
wrong permissions instead of being missing (the fix above works for both).
