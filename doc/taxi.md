# Flight points: bots fly (layout 17 / protocol 12)

Status: built and unit-tested (module: `taxi_test`, `policy_test`, `settings_test`; core: compiled, layout round trip in `ModuleTier2Test`). **Not run on a sim.** Off by default in the module (`Taxi`).

## The rules (the owner's)

- Every bot knows every flight node of its faction, and flies free. There is no flight master to visit first and no money charged.
- Alts (a player's own characters) keep the player's rules: they fly where their character has been, and pay.

## What it does

A bot that is not in a party and has a long walk to make (to a quest start, a grind spot, a hub, a vendor) walks to the nearest flight master that lies on its way,
and flies from there to the flight node nearest to where it wants to go. Then it walks the rest.

- **Core** (`Bots/BotTaxi.*`, `ModuleBotActions.cpp` `TaxiAction`, `ModuleBotSnapshot.cpp` `FillTaxi`, `Player::ActivateTaxiPathTo`):
  - The snapshot names the nearest flight master the bot can walk to (`taxi_master_*`; the 4 nearest of its faction are checked against the navmesh like supply vendors) and says whether it is on a flight (`in_flight`).
  - `ActionType::kTaxi` (24): `target_guid` is the flight master (0 = the nearest in reach), `spell_id` the map (the bot's own), `dest_x/y/z` the point. The core takes the node at the flight master and the node
    nearest to the point, finds the shortest route in the faction's flight graph, and starts the flight. The check that a node is known is skipped for bots, and `Player::ActivateTaxiPathTo` charges a bot nothing.
  - Refused (and so the bot walks) in combat, on a flight, dead, with no flight master within 5 yards, on another map than the point, with no route, or when the landing is not at least `Bot.Taxi.MinGainYards` (600) closer to the point than the flight master is.
  - Settings: `Bot.Taxi.Enabled` (1), `Bot.Taxi.Free` (1), `Bot.Taxi.MinGainYards` (600).
- **Module** (`taxi_policy.*`, `bot_policy.cpp` just before the mount step):
  - A walk of at least `TaxiMinWalkYards` (1500) with a flight master within `TaxiMasterMaxYards` (600) that is not behind the bot (the detour is at most half the walk and the master is nearer the destination than the bot) becomes a walk to the master.
  - At the master (4 yards) it sends `kTaxi`; for 8 seconds it sends nothing, and while `in_flight` it sends no orders.
  - A refused flight makes the bot walk for `TaxiBlockedSec` (600 s). Party members and leaders never fly on their own; neither does a bot in a fight.
  - The event line `taxi: bot= level= map= walk=` is written for each order.
- Settings (module, `Module.forever-bots.`): `Taxi` (0), `TaxiMinWalkYards`, `TaxiMasterMaxYards`, `TaxiBlockedSec`.

## Limits and what to look at on the sim

- Flights stay on one map: a point on another map is refused (no boats, zeppelins or portals; the classic flight graph does not join continents).
- The module cannot know where the landing node is, so it can walk to a flight master and be refused (the core measures the gain); it then walks, and does not try again for 10 minutes.
- The mount step (`kMount`) still runs on the walk to the master. The core takes the mount off when the flight starts.
- Whether a bot really flies without a client (the flight is server driven; no acknowledgement is waited for) and lands at the node is **not verified**: watch `taxi:` events, a bot's position
  jumping along a path, the bot's `in_flight` during it, and the bot ending near its goal. Also whether the policy resumes after landing without a stuck bot (`stuck_idle` lines).
- Cost to watch: the 4 route checks for the flight master happen once per vendor lookup (every 100 yards of movement), within the same per-update budget of 24 path checks.
