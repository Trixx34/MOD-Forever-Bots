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

## Layout 13 (protocol 7): what the SDK now provides

`forever-module-sdk` at `cab5043` added the asks from `core-sdk-requests-2026-10-15.md`. Built against it, the module reads:

- `BotSnapshot::aura_count` / `auras[16]` (`AuraEntry{spell_id, remaining_ms, stacks}`) — the bot's own buffs and DOTs. `AuraRunning` in
  `bot_policy.cpp` replaces the module's own buff timer for self buffs, so a buff another player cast over the bot, or one lost
  to a death, is now the fact it is. A target's DOTs are still only `NearbyUnit::aura_mask`, so `state.Dots` stays.
- `pet_guid` / `pet_health` / `pet_max_health` / `pet_state` (0 none, 1 alive, 2 dead) — `PetOf` reads the pet as fact; the old
  inference from refused Call/Revive/Mend Pet casts is the fallback for a core that does not report pets.
- `ActionType::kPetCommand` (`spell_id` 1 attack `target_guid`, 2 follow, 3 stay, 4 stance in `dest_x`) — the pet is sent at the
  target when a fight starts and called back when it ends, once per fight.
- `casting_target` — who a member is casting on (not used by the policy yet: heal claims stay on the module's own one-cooldown
  claim, which works without it).
- `vendor_guid` / `repair_guid` / `auctioneer_guid` — the vendor action now carries the NPC's guid.
- `copper` / `bag_free_slots` — for buy and hand-in pre-checks (not used yet).
- `last_target_lost_reason` (0 none, 1 killed, 2 leashed, 3 despawned, 4 other player) — telemetry writes it on the `fight_end`
  line, and the policy avoids a creature that leashed or despawned for the usual avoid window.
- Tier 1: `BuyItem(vendorGuid, itemEntry, count)` and `AuctionHouse(op, auctioneerGuid, ...)`, with `PlayerScript.OnAuctionResult`.
  Tier 2: `Craft(botGuid, spellId)` and `Gather(botGuid, targetGuid)`.

**The deployment gate:** a core still on layout 12 / protocol 6 refuses the module at handshake, and a module built against layout
12 refuses to attach to a layout 13 region. The core has to move first; the SDK summary's §10 still lists the layout 12 contract
and the core-side handlers for `kPetCommand`, buy, auction house, craft and gather are not confirmed written yet.

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
- Buff timers are the module's own for party members: the snapshot has no auras. A buff another player cast over it, or a death, is not noticed until the timer runs out (a death does reset the bot's
  state; a ghost is not buffed). The bot's own buffs are read from `auras[]` since layout 13.
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
