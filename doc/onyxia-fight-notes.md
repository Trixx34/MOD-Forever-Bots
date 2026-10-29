# Onyxia's Lair (map 249, boss entry 10184) — fight mechanics

Status: phase/ability shape confirmed 2026-10-29 against Wowhead's Classic strategy guide (pasted into the session by the owner, since
this sandbox's web-fetch failed on 2026-10-28 — see `docs/108-onyxia-raid-job.md`). Source: Wowhead, "Onyxia's Lair Raid Strategy and
Loot Guide — WoW Classic" (updated 2024/12/11), `https://www.wowhead.com/classic/guide/onyxia-onyxias-lair-strategy-wow-classic`.

**This is still a TODO outline, not implemented bot behaviour.** The phases and abilities below are now a confirmed, cited source
instead of "commonly known shape" guesswork — but translating them into actual `bosses[].strategy` content, spell ids from this
fork's own data, room coordinates, and bot avoidance code is still unstarted, and explicitly out of scope for the dungeon-data branch
this file lives on (see the non-scope reminder at the end).

## Trash before the boss room

Several Onyxian Warder patrols line the path into the lair. Their one dangerous ability is **Fire Nova**. The common approach: pull one
patrol, have melee wait out one Fire Nova cast before closing to melee range. Needs confirming against this fork's own `creature_template`/
`smart_scripts` for whichever creature entry the Onyxian Warder is in this database (name lookup, same pattern as the boss entry).

## Phase 1 — ground (100% → 65% health)

Abilities: **Tail Sweep**, **Wing Buffet**, **Flame Breath**.

- Standing behind or in front of Onyxia is dangerous. The raid stays at her sides.
- Tail Sweep hits anyone too close to her tail/rear, and can knock a player into the nest of eggs — which spawns a whelp swarm if that
  happens. This is the mechanical reason tank positioning matters here, not just aesthetics.
- The tank pulls her and slowly turns her so her tail faces the entrance (away from the raid), then backs her all the way to the rear
  wall — this matters because **Wing Buffet** knocks the tank back, and if the tank isn't already against a wall the knockback can send
  them flying across the room (and potentially break line of sight/healing, or reposition Onyxia badly for phase 2).
- DPS should not start attacking until Onyxia is fully repositioned against the wall — starting early risks an aggro pull that wipes the
  raid before positioning is set.
- Conventional wisdom: keep damage conservative in phase 1 and save damage-cooldowns for the phase 1→2 transition (burn window), since
  phase 2 has a hard time limit (she takes flight once she can).

Positioning diagram (as described in the source, not redrawn here): tank at the top of the room holding Onyxia facing away from the
raid; the raid grouped at a side wall.

## Phase 2 — airborne (65% → 40% health)

Trigger: **65% health**, confirmed threshold (not "around 65%" — the source states it plainly). She walks to the center of the room,
then takes flight. This is the burn window: DPS cooldowns are best saved for the moment she hits 65%, to maximize damage done before she
actually gets airborne (there appears to be a brief ground window right at the 65% mark before flight starts).

Abilities once airborne: **Fireball**, **Breath** (commonly called "Deep Breath").

- She periodically picks one of **eight fixed room positions** (N, NE, E, SE, S, SW, W, NW) to fly to.
- From one of those eight positions she will sometimes cast **Deep Breath** — the client shows a "taking a deep breath" warning text when
  this starts. On that warning, the whole raid must move to a safe zone on the **sides** of whichever direction she's facing (the pasted
  diagrams show the danger/safe zones per facing direction — a frontal/rear cone or lane is the danger zone, the sides are safe). This
  needs the actual room geometry (map 249's navmesh/coordinates) to translate into real safe-zone coordinates or an angle-based check —
  not available in this sandbox (same blocker as the entrance/route data; see `data/dungeons.txt`'s OPEN ITEM note).
- Any time she is **not** mid-Deep-Breath, raid members should stay spread at least **8 yards** apart — **Fireball** targets a random
  player, and anyone within 8 yards of the target also takes damage (a splash radius, not a pure single-target hit). `Greater Fire
  Protection Potion` is called out as a strong mitigation here — not a bot behavior item, but worth noting for a "use consumables" policy
  if one exists for raid scenarios.
- Trigger out of phase 2: **40% health** — she lands, phase 3 begins.

## Phase 3 — ground again (40% → 0% health)

Reuses phase 1's three abilities (Tail Sweep, Wing Buffet, Flame Breath) **plus two new ones**:

- **Bellowing Roar** — a periodic fear effect. Tanks need a fear break/immunity (`Berserker Rage` for a warrior tank, or a healer's
  `Tremor Totem`/`Fear Ward`) to keep holding aggro through it — a feared tank dropping aggro mid-phase-3 is a real wipe risk worth a bot
  policy consideration (does a tank bot have/need a fear-break ability queued for this phase specifically?).
- **Whelp adds** — whelps spawn periodically and attack the raid. The described handling is an **off-tank** collecting them and AoE
  classes cleaning them up; the main tank stays on Onyxia. This is a second tank/role assignment implication for a raid roster, beyond
  the per-subgroup tank/healer/dps split the parallel raid-roster effort is building — worth flagging to that effort as a possible
  follow-up (a dedicated add-tank role), though not implemented here.
- **Lava cracks** — the room's floor cracks pulse with lava periodically, dealing fire damage to anyone standing on an active crack.
  `Greater Fire Protection Potion` is called out again here. This needs real room/crack-position data (navmesh or DB) to turn into a
  "stand here, not there" bot rule — not available in this sandbox.
- Same as phase 1: when she lands and repositions, DPS must **not** attack until she's back in position, or risk a wipe-triggering aggro
  pull.

As with phase 1→2, **no exact spell ids, cast times, cone angles/ranges, or room coordinates are in this file** — those need this
fork's own `creature_template`/`smart_scripts`/spell data for entry 10184 (and whatever the actual Onyxian Warder trash entry is), plus
the real room geometry, neither of which this sandbox has access to. The phase/ability/trigger *shape* above is now sourced and
reasonably trustworthy; the *numbers* (spell ids, exact ranges/angles, exact coordinates) are still the open TODO.

## Loot (context only, not fight-mechanics, not a bot-behavior item)

Onyxia always drops `Head of Onyxia` (starts the `Victory for the Horde`/Alliance-equivalent quest chain) and the Tier 2 helm pattern
appropriate to whichever class looted it, plus BoE blues, gem sacks, and gold. Hunters specifically want `Mature Black Dragon Sinew`
(a `Rhok'delar` quest reagent). Noted here only in case a future "bot loot priority" policy needs raid-boss-specific loot rules; no
action taken on this in the current scope.

## What's still genuinely unconfirmed (narrowed down from the 2026-10-28 version of this file)

1. **This fork's exact spell ids** for every named ability above (Tail Sweep, Wing Buffet, Flame Breath, Fireball, Breath/Deep Breath,
   Bellowing Roar, Fire Nova on the trash) — needs a look at `creature_template`/`smart_scripts`/spell data for entry 10184 and the
   Onyxian Warder entry.
2. **Room geometry**: the real coordinates for the 8 flight positions, the Deep Breath safe/danger zones per facing, the lava crack
   locations, and the egg-nest location that spawns whelps if a player lands in it. None of this exists until `make_dungeons_from_db.py`
   is actually run against the live world database for the `onyxia` entry (see `data/dungeons.txt`'s OPEN ITEM note) — the fight
   mechanics and the entrance/route data are blocked on the same missing step.
3. **Exact phase-transition behavior**: is 65%→2 and 40%→3 strictly one-way (as described), or can phase 2 repeat/cycle on later attempts
   in this fork's specific scripting? The source describes one straightforward 1→2→3 progression; confirm this is also true of however
   this fork's `smart_scripts` actually implements it, since forks sometimes diverge from the "classic" shape.
4. **Add-tank / off-tank role**: phase 3's whelp handling implies a second tank role beyond the per-subgroup tank the raid-roster effort
   assigns — not designed or implemented anywhere yet, flagged above as a possible follow-up for that parallel effort.

## Explicit non-scope reminder

This file is still a research/outline document, not implemented game logic. It intentionally does not:

- add any `bosses[].strategy` content to a real dungeon JSON (there is no `data/dungeons/249-onyxias-lair.json` yet — see
  `data/dungeons.txt`'s onyxia OPEN ITEM note and the `onyxia` entry in `tools/make_dungeons_from_db.py`),
- implement any new bot behaviour (ranged-attack avoidance, add/whelp handling, phase-aware tank positioning, fear-break usage), and
- touch the raid-roster/grouping work (`bot_policy.cpp`, `bot_settings.h/.cpp`, `scenario.h/.cpp`), which is a separate, parallel effort
  per `docs/108-onyxia-raid-job.md`.

Next step for a human (or a session with real DB access): run `make_dungeons_from_db.py`'s `onyxia` entry against the live world
database to get real entrance/route/room coordinates, then cross-reference this fork's own `creature_template`/`smart_scripts`/spell
data for entry 10184 (and the Onyxian Warder trash entry) to fill in the exact spell ids and numbers this file still lacks, before any
of this becomes a real dungeon JSON or bot behavior code.
