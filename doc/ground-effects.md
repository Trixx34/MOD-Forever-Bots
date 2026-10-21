# Ground effects: stepping out of the fire

Status: built and unit-tested (`src/ground_avoid.*`, `tests/ground_avoid_test.cpp`, policy tests). The core side (layout 21) is compile-checked only. **Not run on a sim yet.**

A bot standing in a harmful area on the ground (a Flamestrike, a Blizzard, a poison cloud) walks out of it before it does anything else. Off by default.

## What each part does

- **Core** (`Bot.Snapshot.GroundEffects = 1`, default 0): the snapshot of each bot lists up to 4 dynamic objects (a spell's persistent area) and area triggers within 30 yards, the one
  the bot stands in or is nearest to first: spell id, position, radius, kind, and whether the caster is hostile to the bot (`GroundEffect`, SDK 0.19.0, layout 21 / protocol 16).
  It is a grid search for every bot on every snapshot, so leave it off unless this module is on.
- **Module** (`Module.forever-bots.AvoidGroundEffects = 1`): an effect is harmful when its caster is hostile to the bot, or its spell is in `data/avoid_spells.txt` (`AvoidSpellsFile`, an
  absolute path) or in the dungeon file's `bosses[].strategy.avoid_spells`. The dungeon file's `avoid` zones (fixed places) count as well. A bot inside one (closer than its radius + 1 yard
  to the center) walks to the nearest point 3 yards outside every harmful circle (16 directions, up to 40 yards), so it does not step into a second effect. The end point is kept for
  3 seconds so that it does not shift every tick, and the move goes through the move gate like any other.
- **Data** (`data/avoid_spells.txt`, from `tools/gen_avoid_spells.py`): the spells of the Classic dungeons' creatures that leave a harmful persistent area, an area trigger or a trap.
  Method and counts: forever-ops `docs/99-ground-effects.md`.

## Settings

    Module.forever-bots.AvoidGroundEffects = 1
    Module.forever-bots.AvoidSpellsFile = /srv/forever/sim/modules_runtime/forever-bots/data/avoid_spells.txt
    # core, worldserver.conf:   Bot.Snapshot.GroundEffects = 1

## What it does not do

It reacts to the effect after it is there (a bot is hit once for the first tick at most); it does not run from a cast that is still going, and it does not know a boss's frontal attack or
a charge. A bot that walks out leaves its target for a moment; the policy takes its fight up again from where the bot stands. It does not pull: it picks the nearest way out, not
the way toward the party, so a bot can end a few yards from the group.
