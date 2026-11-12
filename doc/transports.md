# Ships and zeppelins: design (2026-11-10)

Status: design only. No module code was written because every part needs an SDK field and a core action first. Read-only findings come from the core
(`Trinity-Forever-Modules` master, `8502697184`) and the SDK (`tier2_layout.h`); nothing in the core or the SDK was changed.

## What the core does today

- Transports are `Transport` game objects (`Entities/Transport/Transport.h`). A passenger is added by `Transport::AddPassenger(passenger, offset)`; the
  transport moves its passengers in `UpdatePassengerPositions`, and when its path crosses to another map (`TeleportTransport` /
  `TeleportPassengersAndHideTransport`) the passengers go with it.
- For a real player, boarding happens in `Handlers/MovementHandler.cpp` (~510-555): the client sends a movement packet whose `MovementInfo.transport`
  holds the transport guid and the offset; the server then calls `AddPassenger`. Leaving is the packet without a transport guid (`RemovePassenger`).
- A bot has no client, so no movement packet ever carries a transport guid. `Bots/*.cpp` and `Modules/ModuleBotActions.cpp` contain no transport code
  at all (keyword search: Transport, ship, zeppelin, boat). A bot that walks onto a deck is therefore never a passenger: it stands at world
  coordinates and the ship sails from under it (the same applies to a lift or an elevator that is a transport).
- The only server-side route onto a transport is a teleport with a transport guid (`TeleportLocation.TransportGuid`, `Player.cpp` ~1507; the worldport
  ack in `MovementHandler.cpp` ~262-270 calls `AddPassenger`). That is how a player is put back on a ship after login (`Player.cpp` ~19587-19625 reads
  `characters.transguid` and re-boards) and how a graveyard on a ship works (`Player.cpp` ~5233).
- `SummonAction`/`MoveToSpeaker` (the chat order "summon") teleports a bot to the speaker's world position on the speaker's map. For a speaker on a
  ship that position is the ship's world position, with no transport guid, so the bot lands in the water (or on the deck by luck, but not as a
  passenger).
- `PathGenerator` knows ship gangplanks only as a navmesh corridor (a comment at ~795 mentions "partway up a ship gangplank"); it does not board.
- `BotSnapshot` (SDK, `tier2_layout.h`) has `map_id`, position, `in_flight` (taxi) but no transport guid, no movement flags and no leader map or
  position. The module sees real players only as nearby units.

## What is needed

### Core (needs a PR in Trinity-Forever-Modules; not done)

1. Snapshot fields, appended at the end of `BotSnapshot` (next layout):
   - `std::uint64_t transport_guid` - the transport the bot is a passenger of (`Unit::GetTransport()->GetTransportGUID()`, 0 when none).
   - `std::uint32_t movement_flags` - `Unit::GetUnitMovementFlags()` (bit for SWIMMING, FLYING, ON_TRANSPORT ...), also what swimming needs
     (see 'Water' below).
   - For the group leader (the module needs to know where it is, and the leader is often a real player who is not in `bots[]`):
     `group_leader_map`, `group_leader_x/y/z`, `group_leader_transport_guid` (0 when not on a transport).
   - Optionally `nearest_transport_guid`, `nearest_transport_entry`, `nearest_transport_distance` of the nearest transport within ~40 yards of the
     bot (so a bot can find a ship in port without a world scan).
2. A new action `ActionType::kTransport` (layout: reuse `spell_id`, `target_guid`, `dest_x`):
   - `spell_id` 1 = board: the transport named by `target_guid` (guid from the snapshot) must be on the bot's map and within ~15 yards of the bot (a
     deck-side check: the transport's collision box expanded by a few yards). The core calls `AddPassenger(bot, offset)` with the bot's offset to the transport
     (`GetPositionOffsetTo`) and sets the bot's movement info transport fields, so it rides like a player. Refused in combat, dead, in flight; `kOutOfRange` when
     too far, `kInvalidTarget` for an unknown guid.
   - `spell_id` 2 = leave: `RemovePassenger`, the bot keeps its world position. Refused unless the transport is stopped at a dock or within ~10 yards of the ground
     (the core decides; `kRefused` with a reason).
   - `spell_id` 3 = join the leader's transport: when the bot's group leader is a passenger of a transport on the bot's map, board that transport at the
     leader's offset; if the leader is on a transport on another map, teleport with `TransportGuid` set. This covers the case 'the leader is on the ship, the
     bot is still on the dock' in one action and is the one the module needs most.
   - Config gate `Bot.Actions.Transport` (default on), like the other actions.
3. `kFollow`/`MoveFollow` on a target that is on a transport: not read in detail (the transport case of `FollowMovementGenerator` is unchecked). Verify on the
   sim once boarding exists that a passenger bot following a passenger leader does not walk off the deck.
4. When the leader is on a transport that changes map (Menethil - Auberdine, Orgrimmar - Undercity zeppelin) the passengers go with it, but a bot that
   stayed behind is on another map than its leader: the existing group-follow logic (module side: a bot far from its leader on the same map follows,
   on another map it does nothing) is not enough. Action 3 above handles it, so no change to dungeon or LFG teleports is needed.

### Module (MOD-Forever-Bots), after the SDK/core part exists

- Policy rule 'ride with the leader' (in the follow part of `bot_policy_turn_move.cpp`): for a bot in a group whose leader snapshot says
  `group_leader_transport_guid != 0` and `transport_guid == 0`, send `kTransport` with `spell_id` 3 (once per 5 s, backed off like `kPartyRetryMs` when
  refused). When the leader's transport guid is 0 again and the bot's is not, send `spell_id` 2 (leave) so a bot that rode on its own does not stay
  aboard after the leader left; a bot with `transport_guid != 0` and no leader on the transport sends no moves of its own beyond `kFollow`.
- While `transport_guid != 0` the module must not send `kMoveTo` with world coordinates (the offsets would be wrong); `kStop`/`kFollow` only. Quest
  walks, grind spots, vendor errands and the move gate must be skipped for such a bot (a `OnTransport()` check in `TurnBegin`, like `in_flight`).
- A solo bot never boards by itself: crossing by ship or zeppelin is a travel feature, not a follow feature (a possible later extension with a table
  of docks and routes, like `taxi.md`).
- Telemetry: events `transport_board`, `transport_leave`, `transport_refused` with reasons; counters in the scorecard.

## Test plan on the sim (needs the SDK/core part deployed on `test`)

1. Unit tests in the module: the rule above with a fake snapshot (leader on transport, bot not; bot on transport, leader not; refused action backs off; a
   solo bot is untouched; a bot on a transport sends no `kMoveTo`). These can be written as soon as the SDK fields exist.
2. Sim, single bot: a GM account (or a scenario `kTeleport` to the dock) puts a player at Menethil Harbor; a bot in the same party; the player boards the
   ship to Auberdine by hand. Expected: within 10 s the bot is a passenger (`transport_guid` set, `.gps` of the bot shows the ship), crosses the map change with the
   ship, and `leave` fires on arrival (or the bot gets off by walking after `leave`).
3. Zeppelin (Orgrimmar - Undercity, Ratchet - Booty Bay boat, Menethil - Theramore): same, one pair per route, and the leader leaving the ship before the
   destination (the bot must leave too).
4. Failure cases: leader boards while the bot is in combat (action refused, retry); bot dies on board (graveyard on a transport: core
   `Player.cpp` ~5233 leaves it alone); the bot is logged out while on a ship (core re-boards at login via `characters.transguid`; verify on a bot
   login after a restart of the sim, not only for players).
5. Metrics: boarding success rate, time to board, bots left behind when the ship leaves.

## Implemented (module 0.47.0, off by default)

`src/transport_ride.h` (pure rules, `tests/transport_ride_test.cpp`) and `BotPolicy::TurnTransport` (after `TurnRevivedAndRoles`). Config `TransportRide`,
`TransportRideEveryMs` (5000), `TransportRideBackoffMs` (30000). Needs the core's `Bot.Actions.Transport = 1` and `Bot.Snapshot.PartyView = 1`.

- A bot in a group, not its leader, alive, not in combat, not swimming, not on a flight, whose leader (the party view member with the leader flag, online) has
  `transport_entry != 0` and which is not aboard that transport: `kTransport` spell 3 (join the leader's transport). The core brings the bot to the leader's
  map when needed and refuses a leader in an instance.
- The leader on no transport while the bot rides (outside combat): spell 2 (leave).
- A passenger gets no `kMoveTo`: `turn.transportHold` turns the step's move into a stop, and `DecideBot` drops any `kMoveTo` pushed in the turn and clears the
  remembered last order. The hold lasts while the snapshot says on-transport and for the turn of a join order.
- A refused order (`kRefused`, `kInvalidTarget`) backs the bot off for `TransportRideBackoffMs`; out of range and busy retry after `TransportRideEveryMs`.
  Events: `transport_board`, `transport_leave`, `transport_refused`.
- Not done: a bot that leads (a leading bot never rides by itself), board by entry for a lone bot (the action exists in the core, no policy uses it).

## Risks

- Bots that stand on a deck without being passengers (today's behaviour) are stranded when the ship leaves; with the action they would be moved
  correctly, but only if the module stops sending world-coordinate moves to them.
- `AddPassenger` with a wrong offset puts the bot in the sea or under the deck: the core must validate the offset against the transport's
  bounds (`|x|,|y|,|z| <= 75`, the limit the movement handler uses).
- Maps: a transport that changes map takes its passengers through `TeleportTransport`; bots are `Player` objects like others there, but this path was
  never exercised with bots. Test with a single bot first.
- Instances: `kSummon` already refuses speakers in an instance; transports are never in instances, so no extra rule.
- Weight: nearest-transport scan per snapshot costs a grid search per bot; only compute it for bots in a group whose leader is on a transport.
- Taxi vs transport: a bot in a flight path has `in_flight`; the two must be exclusive in the policy.

## Water (related, from the movement investigation)

The core already lets a swimming bot path (`PathGenerator.cpp` ~1020 adds the water terrain flag when the start is in water; ~425 builds a shortcut for
a swimmer under water). `HasFullRouteTo` treats `PATHFIND_NOT_USING_PATH` as a route, so a bot in water gets `kApplied`. What the module lacks is the
flag: `movement_flags` above (SWIMMING) or a `liquid_status` byte (`Unit::GetLiquidStatus()`: 0 none, 1 above water, 2 in water, 3 under water). With it the
module can (a) not count a stop in water as 'stuck', (b) let followers use the leader's flags, (c) pick a land route when `kNoRoute` came from a
swimming start. None of this is possible from the current snapshot.
