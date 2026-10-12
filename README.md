# MOD-Forever-Bots

Bot control as a ForeverBots module. The core creates and controls bots; this module decides what they do.

## Status

First slice: the module connects to the core, asks for one bot with `RequestControl`, tracks the bots it owns, and
logs revocations. It does not drive bots yet. Tier 2 bot actions (`Tier2Session::PublishActions`) are not in the
SDK, so `tier2_participates` is `false`.

## Build

    cmake -S . -B build -DFOREVER_MODULE_SDK_SOURCE=/path/to/forever-module-sdk
    cmake --build build
    ctest --test-dir build

Without `FOREVER_MODULE_SDK_SOURCE`, CMake fetches `Trixx34/forever-module-sdk` (`main`).

## Layout

- `module.json`: manifest (name, protocol version, Tier 1 hooks).
- `src/bot_roster.{h,cpp}`: owned bots, pending requests, the max-bots cap. No SDK dependency.
- `src/main.cpp`: handshake, control requests, revocation callbacks.
- `tests/roster_test.cpp`: unit tests for the roster.

## Open items

- Spawn race and class are hard-coded defaults (Human, Warrior) until module config is delivered by the core.
- Max bots is a fixed default of 1 until the core's Tier 2 grant is used.
- No bot behavior yet; that needs Tier 2 actions in the SDK.
