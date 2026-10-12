# MOD-Forever-Bots

Bot control as a ForeverBots module. The core creates and controls bots; this module decides what they do.

## Status

The module connects to the core, asks for one bot with `RequestControl`, tracks the bots it owns, and logs
revocations. It requests Tier 2 and drives its bots from snapshots: a pump thread reads each new snapshot and
publishes actions from `BotPolicy`. The policy is a placeholder (fight the threat target or the closest hostile,
else follow the closest player, else stand still).

## Build

    cmake -S . -B build -DFOREVER_MODULE_SDK_SOURCE=/path/to/forever-module-sdk
    cmake --build build
    ctest --test-dir build

Without `FOREVER_MODULE_SDK_SOURCE`, CMake fetches `Trixx34/forever-module-sdk` (`main`).

## Layout

- `module.json`: manifest (name, protocol version, Tier 1 hooks).
- `src/bot_roster.{h,cpp}`: owned bots, pending requests, the max-bots cap. No SDK dependency.
- `src/bot_policy.{h,cpp}`: decides actions from one snapshot. No SDK state; only the snapshot types.
- `src/main.cpp`: handshake, control requests, revocations, and the Tier 2 pump thread.
- `tests/roster_test.cpp`, `tests/policy_test.cpp`: unit tests on hand-built data.

## Open items

- Spawn race and class are hard-coded defaults (Human, Warrior) until module config is delivered by the core.
- Max bots is a fixed default of 1, not the core's Tier 2 grant.
- The policy does not use spell cooldowns (the core does not fill them yet) or the bot's health.
- Not yet tested against a running worldserver.
