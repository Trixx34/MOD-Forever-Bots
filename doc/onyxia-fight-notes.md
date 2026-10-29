# Onyxia's Lair (map 249, boss entry 10184) — fight mechanics: NEEDS VERIFICATION

Status: outline only, written 2026-10-29 per docs/108-onyxia-raid-job.md. **Nothing below is implemented game logic and nothing below has been checked
against a primary source from this sandbox.** This sandbox has no web-fetch tool and no database access; the wowhead strategy page
(`https://www.wowhead.com/classic/guide/onyxia-onyxias-lair-strategy-wow-classic`) also failed to fetch on 2026-10-28 (see the brief). The phase/ability
names below are the commonly known shape of this 1.15-classic encounter, offered only as a TODO outline for whoever implements the fight script next — they
must be confirmed against a real source (a working fetch of the strategy page, another reliable guide, or the sim's own spell/smart_scripts data) before any
of this is turned into `bosses[].strategy` content or bot behaviour code.

## What needs confirming, phase by phase

1. **Phase 1 (ground, tank-and-spank).** Onyxia fights on the ground. Needs confirming: her spell list in this fork's `creature_template`/`smart_scripts`
   (Spell.db2) for Phase 1 — autoattack, any fear or knockback kit active on the ground in this version.
2. **Phase transition trigger.** Commonly a health-percentage trigger (often cited around 65%) where she takes to the air. Needs confirming against this
   fork's smart_scripts/AI for entry 10184: the exact threshold and whether it is a single one-way transition or a repeating phase cycle (some fight
   versions cycle P1 → P2 (air/whelps) → P3 (deep breath air phase) → back to ground).
3. **Whelp phase.** Eggs/whelp spawns in the lair that may need to be cleared or managed (add) during an air phase. Needs confirming: whelp creature
   entries actually present on map 249 in this fork's `creature` table, whether they are scripted to spawn on a trigger or are static, and what (if
   anything) a raid is expected to do about them (kill them vs. ignore them vs. an add-tank assignment).
4. **Flight phase (fireballs).** While airborne, Onyxia is commonly described as casting a fireball-type ranged attack at random raid members. Needs
   confirming: the actual spell id(s) in this fork's spell data, the damage/radius, and whether `avoid`/`avoid_spells` (the dungeon-strategy.md ground-effect
   fields) are the right mechanism to flag it or whether it needs new bot behaviour (a ranged attack is not a ground effect).
5. **Deep breath (frontal cone fire breath).** A directional fire breath down the room, commonly dodged by not standing in a frontal cone/line. Needs
   confirming: the spell id, its cone/line shape and range in this fork's data, and where in the room it is safe to stand (this needs the actual room
   geometry from the database/navmesh, not a guess).
6. **Tank positioning.** Commonly: the tank holds Onyxia with her back to the raid (so the frontal cone/breath does not hit the raid) and away from the
   whelp spawn points. Needs confirming against this fork's actual room layout and spawn points once the entrance/route data exists
   (see the OPEN ITEM note in `data/dungeons.txt` and `tools/make_dungeons_from_db.py`'s `onyxia` entry — the real coordinates are not in this sandbox).

## Explicit non-scope reminder

This file is a research TODO for a future "what should the fight script do" pass. It intentionally does not:

- add any `bosses[].strategy` content to a real dungeon JSON (there is no `data/dungeons/249-onyxias-lair.json` yet — see `data/dungeons.txt`'s onyxia
  OPEN ITEM note and the `onyxia` entry in `tools/make_dungeons_from_db.py`),
- implement any new bot behaviour (ranged-attack avoidance, add/whelp handling, phase-aware tank positioning), and
- touch the raid-roster/grouping work (`bot_policy.cpp`, `bot_settings.h/.cpp`, `scenario.h/.cpp`), which is explicitly out of scope here and is being
  handled by a separate, parallel effort per docs/108-onyxia-raid-job.md.

Next step for a human (or a session with real web access): confirm each numbered item above against a working fetch of the wowhead classic strategy page
(or another primary source), and against this fork's own `creature_template` / `smart_scripts` / spell data for entry 10184, before writing any of it into
a dungeon JSON file.
