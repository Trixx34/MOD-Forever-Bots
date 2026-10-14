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

## Forever's own abilities

The fork's client data (`/srv/forever/data/dbc/enUS`: `SpellName`, `SkillLine`, `SkillLineAbility`, `SpellMisc`, `SpellEffect`, `SpellPower`, `SpellCooldowns`,
`SpellLevels`) has class abilities the vanilla kits lack, many of them from the Season of Discovery era (ids above 400000). No list of "changed abilities" exists in
the core's source or in docs, and the ability dump (`BotAbilities:`) has not been run yet, so what the core really grants a bot is not known. The spec therefore
names abilities and the tool takes ids and levels from the dump: a name the dump does not show for the class is dropped with a warning, never guessed.

Rows added to `data/rotation_spec.txt` (all read in the client data: class skill line, cost, cooldown, range, effects):

| Class | Rows (priority as in the spec) |
| --- | --- |
| Warrior | Shockwave (2+ enemies), Spearing Strike (rage above 30) |
| Paladin | Rebuke (interrupt), Holy Strike, Divine Storm, Hammer of the Righteous, all before Judgement |
| Hunter | Black Arrow (30 s dot), Chimera Shot, before Multi-Shot |
| Rogue | Saber Slash (12 s bleed, one combo point), before Sinister Strike |
| Priest | Void Plague (18 s dot), Vampiric Touch (15 s dot), before Mind Blast |
| Shaman | Lava Burst before Lightning Bolt; Water Shield replaces Lightning Shield |
| Mage | Icy Veins (strong target), Ice Lance (power above 60%), before Frostbolt |
| Warlock | Wrack (dot), Chaos Bolt before Shadow Bolt; Fel Armor replaces Demon Armor |
| Druid | Starsurge, Sunfire (12 s dot) before Wrath |

The policy counts Saber Slash as a combo builder (one point, like Sinister Strike; `ComboPointsGained` in `src/bot_policy.cpp`). `tests/make_rotations_test.py`
runs the tool on a made-up dump and checks these rows, their order, and that a dump without them gives no rows.

Not added:

- **Demonic Brand** (warlock, spell 1293695 is a talent-tree effect referenced by `TraitDefinition`; 1293696 is a level 26, 10 s proc aura on an enemy): its use (what
  it procs, whether it is cast) cannot be told from the tables read; no row until the dump shows it and its effect is known.
- Rogue Mutilate (needs daggers; gives two combo points, verified in the data), Between the Eyes, Envenom and other finishers or builders: the policy counts only the builders it knows.
- Victory Rush, Overpower, Revenge, Raging Blow and other abilities that need a proc or a state the snapshot does not show; Penance, Mind Flay type channels; Sniper Shot (long cast, 8 yard minimum range).
- Forms and stances, pets (Shadowfiend, Summon Hawk, Kill Command), totems, trait-tree (talent) spells.
