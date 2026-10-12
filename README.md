# MOD-Forever-Bots

Bot control as a ForeverBots module. The core creates and controls bots; this module decides what they do.

## Status

M2 (combat, in progress): the policy approaches a target beyond 5 yards, attacks within it, and casts the class rotation
once per global cooldown. The warrior rotation (`src/combat_rotation.cpp`) is ported from the forever branch's rules.
Refused casts are retried after a short wait. Not yet verified on a sim server.

Earlier status:

The module connects to the core, asks for one bot with `RequestControl`, tracks the bots it owns, and logs
revocations. It requests Tier 2 and drives its bots from snapshots: a pump thread reads each new snapshot and
publishes actions from `BotPolicy`. The policy is a placeholder: below 30% health a bot disengages (follows the closest
player, or stands still); otherwise it fights its threat target or the closest hostile, else follows the closest player,
else stands still. A dead bot gets no orders.

## Stored state

The bots the module controls are rows in the core's module store (doc 15): `bot/<guid>`. On start the module lists
these rows and takes its bots back first, up to `MaxBots`, spawning only for the rest. A row is removed only when the core
says the character does not exist, not on a revoke, because the core revokes every bot at shutdown.

## Configuration

The core sends the module the `Module.forever-bots.<Key>` entries from `worldserver.conf.d/modules.d/forever-bots.conf`
(under the config directory), with the prefix removed. See `config/modules.d/forever-bots.conf.example`:

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
- `src/bot_policy.{h,cpp}`: decides actions from snapshots: approach, attack, rotation casts, retries, disengage.
- `src/combat_rotation.{h,cpp}`: rotation rules (ported from the forever branch), warrior rows, facts from a snapshot.
- `src/bot_settings.{h,cpp}`: reads the module's config (MaxBots, Race, Class).
- `src/bot_store.{h,cpp}`: the store row keys (`bot/<guid>`) for the bots the module controls.
- `src/main.cpp`: handshake, config, control requests, revocations, and the Tier 2 pump thread.
- `tests/roster_test.cpp`, `tests/policy_test.cpp`, `tests/settings_test.cpp`, `tests/bot_store_test.cpp`, `tests/rotation_test.cpp`: unit tests.

## Open items

- The policy does not use spell cooldowns (the core does not fill them yet).
- Not yet tested against a running worldserver.
