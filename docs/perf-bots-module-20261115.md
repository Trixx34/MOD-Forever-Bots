# Bots module performance review, 2026-11-15

Branch `perf/bots-module-20261115` (from origin/test 897b81a2bf). SDK part: branch `perf/sdk-snapshot-20261115` in forever-module-sdk (from sdk-integ 3de5bc1).
Nothing here was run against the sim or prod; only the bots unit tests and the SDK tests ran locally.

## Findings (read from the code)

Pump loop: `src/main_pump.cpp` `PumpTier2`. It polls every 5 ms (`kPumpInterval`, `main_internal.h:71`), the core publishes once per world tick (500 to 1700 ms at 495 bots).

1. **The SDK copied the whole snapshot on every poll**, not once per tick. `forever-module-sdk/src/tier2_session.cpp` `ReadLatestSnapshot`: a fresh `WorldTickSnapshotView` per call, so
   `bots` (495 x 1856 B = 0.9 MB), `nearby` (bots x max_nearby x 136 B, all slots, not only `nearby_count`), party views and members were allocated, copied and the old ones freed,
   about 100 times per world tick. Allocations above the mmap threshold also page-fault every time. This is the largest module-side cost and costs the host CPU, not only the pump thread.
2. `main_pump.cpp:122` (before the change): the "report what the core did" loop (a hash lookup per bot) also ran on every 5 ms poll, not only on a new snapshot.
3. **n squared in the decision loop**: `bot_policy_turn_target.cpp:410` scanned all bots for every grouped bot to find "is my group fighting". Other scans left alone because
   they run only for healers, buffers or leaders (`bot_policy_turn_cast.cpp:136,225,738`) or only for idle followers (`bot_policy_turn_town.cpp:1160`, stack avoidance);
   they are a follow-up if the profile shows them (replace by a per-tick index like `DecideTick::GroupInCombat`).
4. **Every bot ran its full turn (about 40 steps) on every tick**, including a bot that only walks. There was no staggering.
5. **Move re-sends: already handled, verified.** `bot_policy_turn_town.cpp:1209` (`TurnSendMove`) sends a kMoveTo only when it differs from `state.Last`;
   a refused destination waits with doubling pauses (`kNoRoutePauseMs` 5 s, up to 6 doublings, `bot_policy_internal.h:167`; `blockedMove` there, `bot_policy_turn_dead.cpp:45`,
   `bot_policy_turn_move.cpp:85`), and `move_gate.h` is a last gate for all sources (5 s doubling to 2.7 min, partial-path progress lifts the hold). Tests: `policy_test_02`
   (`ANoRouteAnswerEndsTheWalkInsteadOfBeingSentEveryTick`), `chat_test` (`ANoRouteAnswerPausesTheMoveToThatPlace`), `move_gate_test`. The repeated moves seen on prod come from
   fidget steps (`realism`, a 1.5 yard MoveTo to a new point each time, `bot_policy_turn_town.cpp:1112`) and path jitter, which are new destinations by design, and from the core side.
   I did not change the fidget cadence (a behaviour knob of `realism.h`: `Fidget*` keys); lowering its rate is the next lever if the core profile shows short MoveTo calls.
6. **Allocations per tick**: `Decide` built `indexOf`, `present` without reserve; `MoveGate::Filter` built a second vector of actions each tick; `nowMs()` (a clock read) was called twice per bot
   for the move gate.
7. **Logging in hot paths**: none found at default settings. `OnEvent` calls (`move_why`, etc.) are gated on scenario routes or rate limited (5 s per bot); the refusal log in the pump is once per action
   sequence and service refusals are limited to once per bot per 10 minutes. `ActionStats` (a `std::map` insert per action) is off by default.
8. **Unbounded growth**: `BotState::AvoidCount` and `BotState::StallCount` (`bot_policy.h:811,897`) got one entry per creature a bot ever avoided and were never erased (creature references are unique per
   spawn), so a bot that grinds for days keeps thousands of entries. Fixed below. Others checked and bounded: `FoughtAtMs`, `LootedUntilMs`, `AvoidUntilMs` (pruned on expiry),
   `_lootClaims`, `_healClaims` (capped sweeps), `_bots` (erased when the bot leaves the view). Per-guid maps in the pump (`seenSequence`, `serviceNoticeMs`, `ambientLevel`, `move_gate::_bots`,
   telemetry records) are keyed by bot guid and so bounded by the pool size (a few KB per bot), not by time; they are not pruned when a bot is deleted (a purge adds nothing new).
9. **Memory**: the module's own heap is small: one snapshot (about 1 MB bots + nearby) twice in the SDK now (copy buffer and held snapshot), BotState maps, static tables under 3 MB of data files.
   The module is not the cause of the 4.3 GB worldserver RSS. The shared region it maps is `2 x (max_bots x (1856 + max_nearby x 136)) + party sections` (about 35 MB at 2500 bots and 32 nearby),
   owned by the core.

## Changes

| Change | Where | Effect |
|---|---|---|
| Skip the snapshot copy when the published tick is unchanged; copy into a retained scratch view and swap; copy only the first `nearby_count` units of each bot | SDK `tier2_session.cpp/.h`, test in `tests/tier2_attach_test.cpp` | about 99 of 100 polls become two atomic loads; the other copies do not allocate |
| Seen-sequence report loop only on a new snapshot | `main_pump.cpp` | one pass over the bots per tick instead of per poll |
| `DecideTick::GroupInCombat`, built once per tick | `bot_policy_internal.h`, `bot_policy_turn_target.cpp` | O(n) per grouped bot to O(1) |
| `WalkDecideEvery` (default 2): a calm walker takes its full turn one tick in N | `src/calm_walk.h`, `BotPolicy::DecideBot`, `CalmFactsOf` | up to (N-1)/N of the policy time of walking bots |
| Prune per-unit avoid counters past 64 entries | `bot_policy_turn_life.cpp`, `PruneUnitCounters` | bounds `AvoidCount` and `StallCount` |
| `reserve` for `indexOf` and `present`; in-place `MoveGate::Filter`; one clock read for the gate | `bot_policy.cpp`, `move_gate.h`, `main_pump.cpp` | fewer allocations per tick |
| Pump minute line also reports the number of skipped calm turns | `main_pump.cpp`, `BotPolicy::CalmSkippedTurns` | to measure |

### WalkDecideEvery: what counts as calm

All of: the policy's last decision is a kMoveTo; the bot is alive, moving, not in combat, no threat target, not casting; no group, invite or resurrect offer; not in flight, swimming, falling or on a
transport; no ground effects or telegraphs; no living hostile in its nearby list; no errand, visit, tank lead, forced heal, LoS repositioning, level hold, taxi, route, staging hold, dungeon map; no fought
units waiting to be looted. The skip also needs the last full turn to have seen the same `last_action_sequence` and to have found the bot calm, so the first tick after any new result, and after the
end of the walk (moving = 0), is always a full turn. `WalkDecideEvery = 1` turns it off. Idle (standing) and resting bots are not throttled: their timers (wander, shopping, rest) are spread over many
steps and I did not find a safe single rule; that is the next candidate once the sim shows what share of bots stand.

## Expected saving (estimates, not measured)

- SDK polling: removes about 3 MB x ~100 polls per tick of copy, malloc and page faults (the largest single item, on the pump and the host CPU).
- Walk throttle: if a third of the bots are calm walkers (level 1-15 questing and travelling) and N = 2, the decide time drops by about 15 percent; N = 3 about 22 percent.
- Group scan: small at 495 mostly solo bots, grows with the number of parties.
- The core cost of actions (snapshot building, path searches) is unchanged except for the fewer fidget and repeat sends already suppressed.

## Risks

- A calm walker reacts up to N-1 ticks later to something that is not in its snapshot checks (a chat order, an auto-gear request, a low-health rest). At 1.7 s per tick and N = 2 that is under 2 s.
- The throttle relies on `moving` and `last_action_sequence` being truthful in the snapshot; a core that reports `moving = 1` for a stuck bot keeps it throttled (it is still decided every Nth tick, so
  stuck detection runs 2x slower).
- SDK: `Tier2Session` has two new members (class layout change, not the shared-memory protocol). The module and SDK are built together; any other module built against the old header must be rebuilt.
  The `nearby` vector slots past `nearby_count` hold stale data instead of a copy; `NearbyOf` never reads them. Modules that index `nearby` directly beyond `nearby_count` would see old units.
- Same-tick early return assumes the core bumps `published_snapshot_tick` for every publish (the existing torn-read check already relies on it).

## New config key

| Key | Default | Range | Files |
|---|---|---|---|
| `WalkDecideEvery` | 2 | 1..10 (1 = off) | `bot_settings.h` (`BotSettings::WalkDecideEvery`), `bot_settings.cpp`, `main_options.cpp`, README table, `config/modules.d/forever-bots.conf.example` |

Add `Module.forever-bots.WalkDecideEvery = 2` (or 3) to each environment's drop-in when the build is deployed (every key the build knows goes into each env).

## Tests

`tests/calm_walk_test.cpp` (calm rules, the gate, policy-level skipping, hostile and new result end it, off = no skips), `tests/settings_test.cpp` (`WalkDecideEverySetting`),
`tests/move_gate_test.cpp` (in-place filter keeps order), SDK `tests/tier2_attach_test.cpp` (unchanged tick keeps the same storage, new tick is read).

## How to measure

- Pump line in the module log once a minute: `pump: N bots, decide avg X us, max Y us over T ticks, A actions per tick, S calm-walk turns skipped in all`. Compare decide avg before and after with the same
  bot count; `S` per minute divided by (bots x ticks) is the share of skipped turns.
- `perf top -p <module pid>`: `memcpy`, `malloc`, `_int_free` and page-fault time in `ReadLatestSnapshot` should disappear.
- Core side: world update interval in the server log, with the module restarted with `WalkDecideEvery = 1` against 2 and 3.

## SDK items for the owner (protocol changes, not done)

- `BotSnapshot` is 1856 B per bot; the largest parts are `telegraphs` (192 B), `probes` (128 B), `damage_hits`, `ground_effects` (192 B), `quests` (8 entries), `auras` and `target_auras`, `results`
  (48 B), `spell_cooldowns`, which are zero for most bots most of the time. A per-bot variable-length tail (or a dirty flag per bot: "unchanged since the last tick") would let the module skip the copy and
  the decision for unchanged bots, and cut the core's snapshot build. Needs a layout bump.
- A `bot_flags` byte (idle, walking, resting) or a `snapshot_generation` per bot would let the module skip calm bots without inferring it.
- `NearbyUnit` is 136 B and every bot gets `max_nearby_units` slots (32 at the module's request); the module's own use is mostly hostile within 40 yd. A smaller requested `max_nearby_units` for solo
  bots (or a count per tier) would cut the section. The module can request a lower `requested_max_nearby_units` today; the cost is fewer units per bot.
- `PartyMember` (176 B) repeats per member the fields already in `BotSnapshot` for bot members.
