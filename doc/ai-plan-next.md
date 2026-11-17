# Bot AI plan items, overnight job 2026-11-16

Items of `forever-ops/docs/bot-ai-plan-status.md`, each behind a config key that is off by default. Every key is in `config/modules.d/forever-bots.conf.example` and the README table. Tests: `tests/ai_next_test.cpp` (ctest `ai_next`).

## DotPerTarget (bug 14)

`BotState::Dots` holds one entry per spell (the last target it was cast at). A warlock or priest that switches between two targets then recasts Corruption or Shadow Word: Pain on the first target when it returns, although it still runs there. With `DotPerTarget = 1` the policy also keeps `DotUntilMs`, keyed by (spell, target), and the rotation skips a row whose effect is still up on the current target. A self buff and Slice and Dice use target 0. The old `Dots` map stays (the crowd-control code reads it). At most 64 entries per bot; expired ones are dropped when the map is full. No new events: the check is `cast` counts of the DoT spells per fight.

## The ready gate before a pull: SoloPullManaPct, PullBuffGate, PullWaitMaxMs

A bot short of mana, or with an upkeep buff to cast, waits before it starts a fight, for at most `PullWaitMaxMs` (60 s). Route parties already had a gate for health and mana (`PullMinHpPct`, `PullReadyManaPct`, event `pull_hold`); this adds the buff, and a gate for solo bots (leveling, questing), which had none for mana and no event for the health rest.

Solo (`bot_policy_pullready.cpp`, `TurnSoloReady`): not in a group, not on a route, not in combat, no target of its own, no chat order. It uses the solo hurt-rest path: no self pull, no quest walk, it walks away from a creature within 30 yards, and (mana only) sits down to drink.
- `SoloPullManaPct = N`: waits while mana is below N percent, and keeps waiting until N + 20 (so it does not start a fight the moment it passes the mark). Event `pull_wait: bot= reason=mana level= hp_pct= mana_pct= wait_ms=` every 10 s.
- `PullBuffGate = 1`: waits while a buff row of its rotation (`buff` rows of `rotations.txt`: Mark of the Wild, Arcane Intellect, Battle Shout...) is due: not on the bot (`AuraRunning`, all ranks), affordable, not refused a moment ago, not a pet summon. `TurnUpkeepBuffs` casts it meanwhile. Event `pull_wait ... reason=buff`.
- Health: the existing solo rest (`kRecoverPullPct`, 75 percent, 150 s) now logs `pull_wait ... reason=hp` as well; its behaviour is unchanged.
- A wait that reaches `PullWaitMaxMs` gives up: event `pull_wait_timeout: bot= reason= level= wait_ms=`, and the bot pulls. It waits again only after the need has been over once (mana above the mark, the buff on), so a bot that cannot get what it waits for is held once, not for ever.

Route party leader (`bot_policy_turn_move.cpp`, the `pull_hold` block): with `PullBuffGate = 1` the leader also holds the walk to the next pack while any member (itself included) has a buff due, event `pull_hold ... reason=buff`, at most `PullWaitMaxMs`, then `pull_wait_timeout ... reason=buff point=`. Only a member's own rotation buffs count (a party buff another class would cast on it is not looked for).

Cost: `UpkeepBuffDue` scans the buff rows and the bot's auras once per second per bot (kept in `BotState`); the route leader asks for every member. The solo mana check is a few integer compares per tick.

## KillerAvoidMs (default 0 = off)

A solo bot that dies to a creature (not in a party, not on a route) remembers its entry for KillerAvoidMs (0 or 10000..86400000; at most 8 entries) and its solo, out-of-combat pulls skip creatures of that entry. Event: `killer_avoid: bot= level= map= entry= avoid_ms=`. Cost: one map lookup per target candidate, only while the map has entries.

## AoeCcProtect (default 0 = off)

With the key on, a bot's area-damage rows are skipped while a creature that a party member holds with Polymorph, Sap or Banish (its Dots entry still running) is within 12 yards of the bot's target. Checked once per turn, only when an area row is reached. Event (at most every 10 s per bot): `aoe_cc_protect: bot= spell= target=`.
