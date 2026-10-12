# MOD-Forever-Bots

Bot control as a ForeverBots module. The core creates and controls bots; this module decides what they do.

## Status

The module connects to the core, asks for one bot with `RequestControl`, tracks the bots it owns, and logs
revocations. It requests Tier 2 and drives its bots from snapshots: a pump thread reads each new snapshot and
publishes actions from `BotPolicy`. The policy is a placeholder (fight the threat target or the closest hostile,
else follow the closest player, else stand still).

## Configuration

The core sends the module the `Module.forever-bots.<Key>` entries from `modules.d/forever-bots.conf` (next to
`worldserver.conf`), with the prefix removed. See `config/modules.d/forever-bots.conf.example`:

- `MaxBots` (1..16, default 1): bots the module controls at once.
- `Race` and `Class` (default 1 and 1): the race and class to spawn bots with.

Invalid values fall back to the defaults, with a line in the module's log. The bots spawned are capped at the core's
Tier 2 grant, and none are requested when there is no Tier 2 region.

## Build

    cmake -S . -B build -DFOREVER_MODULE_SDK_SOURCE=/path/to/forever-module-sdk
    cmake --build build
    ctest --test-dir build

Without `FOREVER_MODULE_SDK_SOURCE`, CMake fetches `Trixx34/forever-module-sdk` (`main`).

## Layout

- `module.json`: manifest (name, protocol version, Tier 1 hooks).
- `src/bot_roster.{h,cpp}`: owned bots, pending requests, the max-bots cap. No SDK dependency.
- `src/bot_policy.{h,cpp}`: decides actions from one snapshot. No SDK state; only the snapshot types.
- `src/bot_settings.{h,cpp}`: reads the module's config (MaxBots, Race, Class).
- `src/main.cpp`: handshake, config, control requests, revocations, and the Tier 2 pump thread.
- `tests/roster_test.cpp`, `tests/policy_test.cpp`, `tests/settings_test.cpp`: unit tests on hand-built data.

## Open items

- The policy does not use spell cooldowns (the core does not fill them yet) or the bot's health.
- Not yet tested against a running worldserver.
