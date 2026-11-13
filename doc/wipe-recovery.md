# Ready gate and wipe recovery

Status: built and unit-tested (`tests/policy_test_09.cpp`, `tests/scenario_test_04.cpp`, `tests/settings_test.cpp`). **Not run on a sim yet.** Both are on by default and have their own settings (README table).
Goal: closer to the raid logic of a careful human group: nobody pulls while the party is dead tired, and after a wipe nobody pulls until everybody is back. Runs 29 and 31 spent about 2100 of 3600 s dead in Zul'Gurub: after a wipe the survivors were put at the entry, the route resumed at about point 7 and the same pack was pulled again at once with half-dead bots.

## 1. Ready gate before a pull (`PullReadyManaPct`, `PullReadyMaxMs`, with the existing `PullMinHpPct`)

- Members (not the tank), out of combat, while the leader is out of combat: a mana user below `PullReadyManaPct` rests like a hurt member (`pull_hold: bot=N reason=low_mana hp_pct= mana_pct= rest_ms=`), at most `PullReadyMaxMs`. A member that is attacked defends itself. The hurt-member hold (`reason=low_hp`, `PullMinHpPct`, `PullRestMaxMs`) is unchanged.
- The route leader (`RouteReadiness`, `bot_policy_turn_move.cpp`): it does not walk to the next waypoint while a living member is below `PullMinHpPct` or a mana user (itself included) is below `PullReadyManaPct`, at most `PullReadyMaxMs` from the start of the hold. Logged as `pull_hold: bot=LEADER reason=low_hp|low_mana point= wait_ms=` (once per 10 s) and as `route_hold ... why=pull_ready`. This sits on top of the older checks (own health 80 %, `RouteMinManaPct` 70 % with a 35 s cap, dead members).
- 0 for `PullReadyManaPct` turns the mana gate off; `PullMinHpPct` 0 turns the health part off.

## 2. Wipe recovery (`WipeReadyPct`, `WipeReadyMaxMs`, `WipeBackoffCount`, `WipeBackoffMs`)

Lives in the scenario director (`ScenarioDirector::UpdateWipeRecovery`, `src/scenario.cpp`) and needs a scenario run with `ScenarioWipeRecovery = 1` (otherwise all down ends the run). It does not touch the ghost flow: ghosts still walk or are teleported by the policy (`ghost_teleport_hold`, `corpse_run_teleport`) and the entry re-teleport of the living is unchanged. It only holds the living.

1. A wipe starts when every bot is down, or nearly (the leader is down and at most a tenth of a roster of ten or more is up). It is counted per route segment (the furthest route point reached).
2. The hold: members rest and pull nothing (`pull_hold reason=wipe_hold`, no cap in the policy), the route leader does not walk. It ends when every bot is up, on the entry map, with `WipeReadyPct` health and mana, or after `WipeReadyMaxMs`.
3. The tank first: the leader is freed `kWipeTankLeadMs` (6 s) before the members, so it is at the pack first and the existing tank-first pull hold applies to the members after that.
4. Backoff: the `WipeBackoffCount`-th wipe at the same segment waits at least `WipeBackoffMs` and the hold may last `WipeBackoffMs` longer; then the living on the map are teleported to route point (furthest - 2) and the leader's route index is set back to it once (`wipe_recovery: bot= reason=rewind from= to=`), so the pack is re-pulled from a safe point with a rested party instead of at once.

Events (run file and journal, `TakeTelemetry`): `wipe_recovery: party= reason=start|start_backoff|ready|timeout|backoff|released wipes= segment= total_wipes= waited_ms= alive=a/n [safe_point=]` and the policy's `wipe_recovery: bot= reason=rewind ...`.

## What only the sim can show

Whether the ghosts' revive points (corpse inside the dungeon) leave the hold at a safe place, whether the members drink during the hold, how long the holds really are in ZG, and whether the safe point two route points back is clear of the pack that wiped the party.
