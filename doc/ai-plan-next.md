# Bot AI plan items, overnight job 2026-11-16

Items of `forever-ops/docs/bot-ai-plan-status.md`, each behind a config key that is off by default. Every key is in `config/modules.d/forever-bots.conf.example` and the README table. Tests: `tests/ai_next_test.cpp` (ctest `ai_next`).

## DotPerTarget (bug 14)

`BotState::Dots` holds one entry per spell (the last target it was cast at). A warlock or priest that switches between two targets then recasts Corruption or Shadow Word: Pain on the first target when it returns, although it still runs there. With `DotPerTarget = 1` the policy also keeps `DotUntilMs`, keyed by (spell, target), and the rotation skips a row whose effect is still up on the current target. A self buff and Slice and Dice use target 0. The old `Dots` map stays (the crowd-control code reads it). At most 64 entries per bot; expired ones are dropped when the map is full. No new events: the check is `cast` counts of the DoT spells per fight.
