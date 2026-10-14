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

- Stances (warrior), forms (druid), and totems that need placing are not handled; Searing Totem is cast as a self spell at the fight's start.
- Auto Shot (75) is tracked in the module: once a cast is accepted on a target it is not cast again while the bot keeps that target and stands still. The state is cleared when the target changes, the bot moves, the fight ends, the bot dies, or a Mend Pet channel is accepted (it is cast again then). The snapshot does not say whether Auto Shot is running, so a shot stopped some other way (stun, interrupt, out of range) is not noticed until one of those events.
- Hunter pet, by spell casts only (policy code, not rotation rows; learn levels 10, 10 and 12 are in `LearnLevel`). Out of combat, standing, with nothing to fight: Call Pet (883) at level 10 and after each death; a refusal is read as a dead pet and Revive Pet (982) follows; a refused Revive Pet backs off 30 s, doubling to 5 min. After a fight Mend Pet (136, level 12) is cast as a check and a heal (accepted: the pet is there; refused: Call Pet follows; below level 12 a Call Pet check at most every 2 min). In a fight longer than 15 s Mend Pet is cast at most every 30 s with at least 50% mana and 50% health. All pet casts use target 0; whether the core resolves that to the pet for Mend Pet and Revive Pet is not verified. The pet casts wait for a bot that is standing with nothing to do, so a bot that keeps walking does not call its pet.
- The pet is not sent to attack the bot's target and its state is guessed. The SDK/core has no pet in the Tier 2 snapshot and no pet action (`ActionType` ends at `kMount`; `BotSnapshot` and `NearbyUnit` say nothing about a pet or ownership). Needed, in the core and the SDK `tier2_layout.h` (a layout bump, kept byte-identical in both):
  1. In `BotSnapshot`: `pet_guid` (unit reference, 0 = no pet), `pet_health`, `pet_max_health`, `pet_state` (0 none, 1 alive, 2 dead) and the pet's react state. With these Call Pet and Revive Pet are cast on fact, not on refusals, and Mend Pet when the pet is hurt. Alternatively an `is_own_pet` flag on `NearbyUnit`.
  2. A new `ActionType::kPetCommand` (the value after `kMount`; `kLastActionType` moves with it): `spell_id` 1 attack `target_guid`, 2 follow, 3 stay, 4 set the stance (passive, defensive or aggressive in `dest_x`), with a `BotActionStatus` result. The core would do what the pet action handler does for a player (set the command state, `Unit::Attack`). The policy would then send the pet at the bot's target when a fight starts and call it back when it ends.
  3. The core confirming that a `kCastSpell` with `target_guid` 0 reaches the pet for Mend Pet and Revive Pet.
- A warlock's pet is not handled either (same missing snapshot field and action).
- The rows named in policy code (Sinister Strike and Eviscerate for combo points, the heal ids) stay tied to their ids: if the dump gives other ids for them, the combo count is wrong.
- Buff timers are the module's own: the snapshot has no auras. A buff another player cast over it, or a death, is not noticed until the timer runs out (a death does reset the bot's
  state; a ghost is not buffed).
- Warlock's Life Tap has no health check yet.
