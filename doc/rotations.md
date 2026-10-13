# Class rotations from data, with upkeep buffs

Status: built and unit-tested (`src/combat_rotation.*`, `tests/rotation_data_test.cpp`). The rotation file is not made yet; without `RotationFile` the
built-in rank 1 rows are used, as before.

## Why

The built-in rotations are a few rank 1 spells per class, enough for levels 1 to 15. From level 15 on, bots cast the same three spells, and nothing keeps
buffs up (Power Word: Fortitude, Mark of the Wild, Arcane Intellect, armor spells, Battle Shout, auras, weapon imbues). The ids in the built-in tables were also
copied from another branch, and a wrong id fails silently.

## What it does

- `RotationFile` names a file of rows per class (`src/combat_rotation.h` has the format). A class in the file uses its rows; a class not in it keeps the built-in ones.
- **Fight rows** (`row`): in priority order, with a condition, a target (self or the target), a range, the level the spell is learned at (`level`), an optional last
  level (`maxlevel`, when a better spell replaces it) and an optional effect length (`duration`: a damage-over-time effect or a buff is not recast while it runs).
- **Upkeep buffs** (`buff`): cast out of combat, standing, with nothing to fight, when the last cast has run out. `self` buffs the bot; `party` buffs the bot and every
  member of its group within 30 yards, one cast per global cooldown. The cast result is read as for any spell: a refusal retries after the usual time.
- Heals and resurrects are not rows: the policy's heal block and rez logic do them (they need the group's health).

## Making the file (on the sim)

1. Dump what each class gets: set `Bot.Dump.Abilities = 1` in the core's `worldserver.conf`, restart, and let the module create one bot of each class (any race). The core
   logs `BotAbilities: race R class C level L: ...` lines to `Server.log`, one per dump level (1, 5, 10 ... 60).
2. Run the tool with the spec in `data/rotation_spec.txt` (spells by name):

        python3 -I tools/make_rotations.py --dump Server.log --spec data/rotation_spec.txt > data/rotations.txt

   It finds each spell by name for the class, uses the lowest rank's id (the core casts the highest rank the bot knows), and sets `level` to the level after the previous dump
   level (a lower bound, so a spell may be tried a few levels before it is learned; a bot that does not know it gets a refusal and retries a minute later). It warns for each name it
   cannot find: fix the spec's spelling or drop the row. `supersede "Frost Armor" by "Ice Armor"` retires the old buff when the new one is learned.
3. Set `RotationFile = "data/rotations.txt"` in the module's config.

## Limits

- Stances (warrior), forms (druid), pets, and totems that need placing are not handled; Searing Totem is cast as a self spell at the fight's start.
- The rows named in policy code (Sinister Strike and Eviscerate for combo points, the heal ids) stay tied to their ids: if the dump gives other ids for them, the combo count is wrong.
- Buff timers are the module's own: the snapshot has no auras. A buff another player cast over it, or a death, is not noticed until the timer runs out (a death does reset the bot's
  state; a ghost is not buffed).
- Warlock's Life Tap has no health check yet.
