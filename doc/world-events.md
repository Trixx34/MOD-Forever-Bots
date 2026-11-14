# World events and holidays

Status: module side built and unit-tested, **off by default** (`WorldEvents = 0`) and **inert until two things are supplied**: the list of active events
(the core does not report it, see "What the core would need") and a site table (`data/world_events.txt` ships empty, see "Making the data"). Not run on the
sim.

## What a bot does

While a game event is active, a small share of the idle bots visits one of the event's sites:

1. A timer (every 10 s, `world_events::Scheduler`) takes the bots the pump reports as free (online, alive, out of combat, not in flight, not on a town
   errand, not in a party) and starts visits within the caps below. A site suits a bot when its event is active, its side fits (a one-sided site needs
   a known side), the bot's level is in the site's range and the bot is on the site's map (no continent change for a visit). A bot picks one of the
   three nearest suiting sites by a seeded draw (`RandomSeed`), so a crowd does not take the same one.
2. The policy walks the bot to the site (`kMoveTo`, the same walk as a town errand; a walk with no route fails the visit).
3. In reach (5 yards) it sends `kQuest` accept for each quest of the site, one every 3 s, then `kQuest` hand-in for each, then stays 20 s and the visit is
   done. The core refuses a quest that does not apply (done, level, no giver in reach); the visit ignores the refusal and the quest engine does not count it
   (no `quest_paused`).
4. The visit ends early on: the 10 minute limit (`WorldEventsMaxMin`), the bot called to a party, the bot gone or busy, the event over (the active list no
   longer holds it). A bot never visits the same site twice while its event runs and waits `WorldEventsGapMin` between visits.

Caps: `WorldEventsPct` percent of the bots in the feed on a visit at once (rounded up, so a share above 0 allows at least one), never more than
`WorldEventsMaxBots`, at most `WorldEventsPerSite` at a site, at most `WorldEventsStartsPerTick` starts per check.

Journal lines (`forever-bots: event <line>`): `world_event_start bot= event= site= quests=`, `world_event_done bot= event= site= took_s=`,
`world_event_abandoned reason=timeout|called|unavailable|event_over|failed bot= event= site= took_s=`; the policy adds `world_event_visit_done` and
`world_event_visit_failed` (no route).

Not built: buying event items and dressing for the event. Buying needs a vendor guid and an item entry (the `kVendor` buy bits buy food, drink,
ammunition and potions only; `BuyItemAction` is a Tier 1 request the pump does not use today), and "equip the best gear" (`kVendor` bit 4) does not pick a
holiday hat or costume. Both are core or wiring work, listed below.

## Keys

All in `config/modules.d/forever-bots.conf.example` and the README table: `WorldEvents` (0), `WorldEventsFile`, `WorldEventsActive` (empty, comma separated
event ids), `WorldEventsPct` (2, 0 to 20), `WorldEventsMaxBots` (10), `WorldEventsPerSite` (3), `WorldEventsMaxMin` (10), `WorldEventsGapMin` (180),
`WorldEventsStartsPerTick` (1). A config reload changes the caps and the active list; the site table is read at start.

## What the core knows (checked read-only on forever-test, 2026-11-14)

- `game_event` has 89 rows (Lunar Festival 7, Hallow's End 12, Winter Veil 2, Love is in the Air 8, Noblegarden 9, Children's Week 10, Harvest Festival 11,
  Midsummer 1, Brewfest 24, Pilgrim's Bounty 26, Day of the Dead 51, Darkmoon Faire 3 ...).
- **Every dated event ends in 2020 or earlier** (`end_time`, mostly 2020-12-31). The core only treats a `NORMAL` event as active while
  `start < now < end` (`GameEventMgr::CheckOneGameEvent`, source tree `Trinity-Forever-Modules`), so in 2026 none of the holidays is active on the sim as
  the data stands. Moving `end_time` out (a world SQL update) is needed before any holiday runs. The events with no dates (`start_time` NULL: Elemental
  Invasions, Scourge Invasion, PvP seasons, Wintergrasp) are world events driven by conditions, not holidays. I could not read the live active list: the
  core does not expose it (next section).
- **No event spawns exist.** `game_event_creature`, `game_event_gameobject`, `game_event_npc_vendor`, `game_event_npcflag` and `game_event_model_equip` have
  0 rows. The event quest relations exist: `game_event_creature_quest` 235 rows (172 givers; Lunar Festival 93, Midsummer 40, Brewfest 27, Children's Week
  44, Winter Veil 8 ...), `game_event_gameobject_quest` 101 rows, `game_event_seasonal_questrelation` ~700 rows. None of the 172 givers has a row in
  `creature`, so there is no position and no NPC to talk to even when an event runs. Without spawns a `kQuest` accept is refused (no giver in reach).
  So the world data needs the event NPC spawns (and vendors) before this feature can do anything; the module cannot invent positions.

## What the core would need to report active events

Today nothing in the snapshot (layout 25) or in the events the core sends carries the active game events. The module also has no database access.
The smallest change that keeps protocol 29: add to the SDK `ServerStatsEvent` (or a new `GameEventsEvent`) an `optional`/`repeated uint32 active_game_events`
field, and have the core send it:

1. on module attach (the full list), and
2. whenever the set changes: `GameEventMgr::AddActiveEvent` and `RemoveActiveEvent` (`m_ActiveEvents`, `GetActiveEventList()`), which `StartEvent`,
   `StopEvent` and the periodic `Update` all go through.

The core's wire list (`ModuleWire.h`) has `server_stats = 18` reserved but "not sent yet", and a hook the module does not subscribe to today needs a
`module approve`, so this is a deliberate core + SDK + approval step. The module side is ready for it: `world_events::ActiveEvents::SetReported` takes the
list (the report replaces the configured list as a whole, an empty report means none is active) and the scheduler already follows the list every tick; what
is missing is only the notify handler that calls it (a few lines in `main_commands.cpp` next to the other `OnNotify` handlers, once the SDK field exists).

Alternatives: a Tier 2 snapshot header field (a layout bump, with the byte-identical `tier2_layout.h` copies in core and SDK), or a core chat command the
operator runs. Until then the operator lists the running events in `WorldEventsActive`.

## What the core would need for the rest

- Buying: a `kVendor` bit (or a new action) that buys the item entry in `dest_x` (count in `dest_y`) from the vendor in reach, with the same money reserve as
  the supply buys; or the pump sends `BuyItemAction` with the vendor guid from the snapshot's nearby units.
- Dressing: an action that equips a named item entry from the bags (the event hat or costume); `kVendor` bit 4 chooses by gear score, not by theme.
- Event objects (the Lunar Festival fireworks, Hallow's End candy buckets): `kUseObject` already exists; a site line could list entries for it later.

## Making the data

`data/world_events.txt` lines: `event|side|map|x|y|z|minLevel|maxLevel|name|quests`. Source of the event, the quest ids and the levels: the world database
(`game_event_creature_quest` gives giver entry and quest; `quest_template` the level). Source of the position: the event NPC spawns, which this world does
not have (see above). Use only talk quests with no objectives (the Lunar Festival elders: accept, hand in on the spot); a quest with objectives is refused
at hand-in and wasted. Check each line on the sim with one bot before listing it.

## Unverified

Nothing here has run on a worldserver: the walk, the quest orders at an event NPC, the 5 yard reach, whether the core's hand-in right after an accept works
for the elder quests, and the active list from the core. The unit tests cover the table, the caps, the end rules and the policy's order sequence.
