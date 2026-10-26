# Class rotations from data, per spec, with upkeep buffs and pets

Status (0.33.0): built and unit-tested (`src/combat_rotation.*`, `tests/rotation_data_test.cpp`, `tests/tame_test.cpp`, `tests/make_rotations_test.py`). The rotation file is
made (`data/rotations.txt`, 374 rows in a default block and a block per spec, from the level 30 guides) and **loaded by default**: `RotationFile` defaults to `data/rotations.txt` beside the
module's executable. Before 0.33.0 the default was no file and every class ran the few built-in rank 1 rows (the 2026-10-26 sim run had no `RotationFile` line, so mages cast only
Fireball, Fire Blast and Frost Nova, hunters shot nothing, and the warrior tank had no Taunt, no stance and no Shield Block). `RotationFile = ""` switches back to the built-in rows.

## Why

The built-in rotations are a few rank 1 spells per class, enough for levels 1 to 15. From level 15 on, bots cast the same three spells, and nothing keeps
buffs up (Power Word: Fortitude, Mark of the Wild, Arcane Intellect, armor spells, Battle Shout, auras, weapon imbues). The ids in the built-in tables were also
copied from another branch, and a wrong id fails silently.

## What it does

- `RotationFile` names a file of rows per class (`src/combat_rotation.h` has the format). A class in the file uses its rows; a class not in it keeps the built-in ones.
- A class has a default block (`class 3`) and one block per spec (`class 3 spec bm`). A bot plays the block of its spec, else the class's default block, else the built-in rows.
- **Fight rows** (`row`): in priority order, with a condition, a target (self or the target), a range, the level the spell is learned at (`level`), an optional last
  level (`maxlevel`, when a better spell replaces it) and an optional effect length (`duration`: a damage-over-time effect or a buff is not recast while it runs).
- **Upkeep buffs** (`buff`): cast out of combat, standing, with nothing to fight, when the last cast has run out. `self` buffs the bot; `party` buffs the bot and every
  member of its group within 30 yards, one cast per global cooldown. The cast result is read as for any spell: a refusal retries after the usual time. A buff row may carry
  a condition: `in_party` (a warrior holds Defensive Stance only in a party), `no_pet` (a pet summon), `self_aura_below <spell> <stacks>` (a druid form, only when the aura list
  does not show it).
- Conditions: `always`, `target_hp_above/below N`, `self_hp_below N`, `self_power_below/above N`, `enemies N`, `enemies_below N`, `target_casting`, `opener N`, `target_strong N`,
  `combo N`, `in_party`, `no_pet`, `self_aura_below <spell> <stacks>` and `self_aura_at_least <spell> <stacks>` (a proc such as Missile Barrage, or a stack count such as Arcane Blast's).
  One condition per row. `self_aura_*` read the bot's own auras (layout 13: at most 16, with their stacks).
- Heals and resurrects are not rows: the policy's heal block and rez logic do them (they need the group's health). See "The priest" below.

## Specs

The spec of a bot is the third field of its scenario roster entry, `race:class:spec` (for example `1:1:prot`, `3:3:bm`, `1:5:holy`). Without it the bot plays the default spec of its
class and role (`rotation::DefaultSpec`): tank warrior prot, paladin prot, druid feral_tank; healer priest holy, paladin holy, shaman resto, druid resto; damage warrior arms, paladin ret,
hunter bm, rogue combat, priest shadow, shaman enh, mage arcane, warlock demonology, druid feral_cat. The role is the one the policy gives the bot (a role set by chat command, else a
warrior in a party tanks, a healer class heals, the rest deal damage). A spec in the roster also sets the role of a bot that has none: a shadow priest does not heal, a protection warrior tanks.

Spec keys (lower case; the SDK's spec keys): hunter `bm mm sv`; warrior `arms fury prot`; paladin `holy prot ret`; priest `disc holy shadow`; shaman `ele enh resto`; mage `arcane fire frost`;
warlock `affliction demonology destruction`; rogue `assassination combat subtlety`; druid `balance feral_tank feral_cat resto`. A key the class does not have is ignored with a log line.

Talent profiles are still keyed by (class, role), not by spec: the policy passes the role to `SpendBotTalents`.

What each spec does (the rows are in `data/rotation_spec.txt`, with a comment per block; the source is `docs/102-wowhead-class-rotations.md` in forever-ops):

| Class | Specs with rows | Notes |
| --- | --- | --- |
| Warrior | prot, arms, fury | prot: Taunt (two or more enemies), Defensive Stance in a party, Shield Block, Revenge, Thunder Clap, Sunder Armor in a group, Retaliation. arms: Sweeping Strikes, Overpower, Mortal Strike. fury: Death Wish, Bloodthirst, Overpower. Berserker Stance is a class quest and is not cast. |
| Paladin | prot, holy, ret | prot: Righteous Fury and Retribution Aura, Seal of Fury, Consecration, Holy Strike, Judgement. ret: Judgement of the Crusader, Seal of Righteousness, Hammer of Wrath. |
| Hunter | bm, mm, sv | bm: Aimed Shot or Multi-Shot, Arcane Shot, Summon Hawk (from level 25, id 1293241). mm adds Rapid Fire and Trueshot Aura; sv has melee rows (Raptor Strike, Mongoose Bite, Strider Kick, Counterattack) for a mob within 5 yards. The bot does not walk in to weave melee. |
| Rogue | combat, assassination, subtlety | builders Sinister Strike, Mutilate (two combo points) and Hemorrhage; finishers Eviscerate and Slice and Dice. Garrote and openers need Stealth and are not cast. |
| Priest | holy, disc, shadow | holy and disc: Holy Fire opener, Pain, Mind Blast, Smite; the heals are the heal block. shadow: Pain, Void Plague, Vampiric Touch, Mind Blast, Devouring Plague, Mind Flay, Vampiric Embrace. |
| Shaman | resto, ele, enh | totems that need placing are not cast except Searing and Magma Totem (self spells); Water Shield replaces Lightning Shield from level 20. |
| Mage | arcane, fire, frost | arcane: Arcane Blast while under two stacks, Missiles on a proc, Fire Blast to reset; fire: Scorch, Pyroblast on Heating Up; frost: Ice Lance on Fingers of Frost, Cone of Cold, Icy Veins. The aura ids (400573 Arcane Blast, 400588 Missile Barrage, 400624 Heating Up, 400647 Fingers of Frost) are the spell names' ids in the client data; whether the bot's aura list reports them under these ids is checked on the sim. |
| Warlock | demonology, affliction, destruction | the demon is an upkeep row (see Pets). demonology: Corruption, Bane of Agony, Searing Pain, Drain Life; affliction adds Immolate; destruction: Conflagrate, Shadowburn, Immolate. |
| Druid | balance, feral_tank, feral_cat, resto | feral specs hold their form (Bear 5487, Cat 768) as a buff that is cast only when the aura list does not show it. Combo points are not counted for a druid, so Rip and Ferocious Bite are not rows. |

Classes whose guide says "no rotation on the page" (paladin holy, shaman resto) get a light block.

## The priest

The policy's heal block follows the Holy priest of the level 30 guide: **Renew** on the tank (the party leader) while it fights, cast again when its 15 s have run out (the module's own timer: the priest sees only its own auras);
**Binding Heal** (first rank 1240770, level 32) when the priest and the tank are both under 70%; **Prayer of Healing** (596, level 30) when three or more members are under 50%; **Flash Heal** in an emergency; **Heal** as the main heal;
**Power Word: Shield** under 50% on a member that took damage, **never on a warrior or a druid** (the absorb starves a rage user of rage). Fortitude, Inner Fire and Divine Spirit are upkeep buffs. Not done: the wand when the party is healthy,
Power Word: Shield on the tank before the pull (the tank is a warrior in these runs), Dispel Magic, Fade.

## Pets

- **Warlock.** The demon is an upkeep buff row with `no_pet`: it is cast whenever the snapshot shows no pet (`pet_state` 0), at once and without the one-hour party claim, and in the file's order, so a summon the bot has not
  learned (refused: "not known") is left for a minute and the next row is tried: Succubus (level 20), Voidwalker (level 10), Imp. The affliction and destruction blocks have no Succubus (the Voidwalker holds threat). Voidwalker and Succubus use a
  Soul Shard each: the scenario gives a warlock 10 (`ScenarioClassSupplies`, default `9:6265:10`). The pet is sent at the fight's target and told to follow after it (since 0.33.0; before, it was told to stay).
- **Hunter.** A hunter of level 10 or more with no pet that is not in a party walks to the nearest beast of `data/tame_beasts.txt` its level allows (the pet takes the beast's level, so the closest level wins; then a neutral beast over a hostile one;
  then cat, wolf, boar, bear), casts Tame Beast (1515) on it within 18 yards, and stands still for the 20 s channel; the core creates the pet when it ends. While a beast is in view and the hunter has no pet, the hunter does not pull anything else (0.34.1: in Westfall the hostile Fleshrippers
  were pulled first and the hunters fought and never cast), and a mob that bites during the channel does not end it unless the hunter drops under half its health. It needs Call Pet (883) known: without it the core refuses the cast, the hunter tries
  four times and then waits ten minutes. Call Pet and Revive Pet after that are the policy's earlier pet logic (`pet_state` from the snapshot). `TameBeastFile` names the list (default `data/tame_beasts.txt` beside the executable; empty = no taming).
- **In a scenario** the hunters stand in a city, so the scenario has a Taming phase (after the dressing, before the grouping): with `ScenarioTameSpot.<dungeon> = "map x y z"` the hunters without a pet are teleported there, tame, and are
  teleported back; the phase ends when all have a pet or after 300 s (a hunter that failed stays in the party without one). Westfall (`0 -10439.4 929.2 38.9`, level 14 to 15 Goretusks, Fleshrippers and Coyotes) is near the Deadmines.
- **Class quests.** Pets also come from class quests (the guide), and the core completes a quest with its reward on core quest operation 4. That operation needs the quest in the bot's log, and a bot in a scenario has accepted nothing, so
  `ScenarioClassQuests` (off by default since 0.34.1: the refusals filled the log and paused the bots' quest planning; the warlock's pet quests are 1504, 9619, 1474, 1513 and the hunter's Taming the Beast quests 6061 to 6064 and 6082 to 6085) sends operation 4 in the dressing and the core refuses it ("quest is not in the log") until
  operation 4 adds the quest first. On the sim the trainer rows now teach the warlock demons, so the quests are only a second way.

## The scenario's helpers

- **Ammunition.** A hunter whose snapshot says `ammo_entry != 0` and `ammo_count < 100` is given `ScenarioAmmo` pieces (default 200, at most one stack: the core refuses a larger count, which refused every grant of 1000 in the first 0.33.0 run) of the ammunition the core recommends for its weapon, in every phase from the dressing on
  (`kGrant`, at most every 5 s and 40 times a run; three grants in a row that never raised the count stop it). Without ammunition the policy leaves every ranged row out (the core would refuse it), so the 2026-10-26 hunters only swung in melee and logged no cast at all.
- **Class supplies and quests**: `ScenarioClassSupplies = "9:6265:10"` (class:item:count) and `ScenarioClassQuests = "9:1504,3:6061"` (class:quest).

## Making the file

1. Get the abilities per class and level. Either let the core log them (`Bot.Dump.Abilities = 1` in `worldserver.conf`, restart, one bot of each class; `BotAbilities: race R class C level L: ...` lines in `Server.log`), or build the same
   lines from the client files and a trainer export, without a restart:

        ssh forever-test sudo -n mysql -N -B forever_test_world -e "select SpellId, min(ReqLevel), count(*) from trainer_spell group by SpellId" > trainer.tsv
        python3 -I tools/abilities_from_db2.py --dbc /srv/forever/data/dbc/enUS --trainer trainer.tsv > abilities.log

   `abilities_from_db2.py` takes a class's spells from `SkillLineAbility` (class mask, or the skill line's class), names from `SpellName`, and the learn level from the trainers (the lowest `ReqLevel`; bots learn trainer spells only), else
   `SpellLevels`. For a name some trainer teaches, only the trainer's ids count (Execute's damage spell 20647 is not a rank); a name no trainer teaches keeps its client ids (Summon Hawk, Auto Shot).
2. Run the generator with the spec in `data/rotation_spec.txt` (spells by name, or by id where a name is ambiguous):

        python3 -I tools/make_rotations.py --dump abilities.log --spec data/rotation_spec.txt > data/rotations.txt

   It finds each spell by name for the class, uses the id of the rank learned first (the core casts the highest rank the bot knows), and sets `level` to the level it is learned at. It warns for each name it cannot find: fix the
   spec's spelling or drop the row. `supersede "Frost Armor" by "Ice Armor"` retires the old buff when the new one is learned. `class N spec KEY` blocks are passed through.
3. The tame beast list: `tools/make_tame_beasts.py` (the query is in its header) makes `data/tame_beasts.txt` from the world database.

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

- Stances (warrior) and forms (druid) are upkeep buffs with a condition; if the snapshot does not report the stance or form aura, a form could be toggled off again: check on the sim that `auras[]` lists them. Totems that need placing
  are not handled; Searing Totem is cast as a self spell at the fight's start. Berserker Stance (a class quest) is not cast.
- Auto Shot (75) is tracked in the module: once a cast is accepted on a target it is not cast again while the bot keeps that target and stands still. The state is cleared when the target changes, the bot moves, the fight ends or the target dies.
- The warrior's Taunt is cast against two or more enemies: the snapshot has no "target of target", so a mob that is not on the tank cannot be seen. Revenge, Overpower and Mutilate are tried and refused until usable.
- The rows named in policy code (Sinister Strike, Mutilate, Hemorrhage and Saber Slash for combo points; Eviscerate, Rupture and Slice and Dice spend them; the heal ids) stay tied to their ids: if the dump gives other ids for them, the combo count is wrong.
- Buff timers are the module's own for party members: the snapshot has no auras for them. A buff another player cast over it, or a death, is not noticed until the timer runs out. The bot's own buffs are read from `auras[]` since layout 13.
- Warlock's Life Tap has no health check yet. The survival hunter and the enhancement shaman do not walk in to melee: the policy keeps ranged classes at range.
- Two aura ids of the mage rows (Arcane Blast's stack 400573 and Missile Barrage 400588) are the names' ids in the client data and are not confirmed against the bot's own `auras[]`.

## Forever's own abilities

The fork's client data (`/srv/forever/data/dbc/enUS`: `SpellName`, `SkillLine`, `SkillLineAbility`, `SpellMisc`, `SpellEffect`, `SpellPower`, `SpellCooldowns`,
`SpellLevels`) has class abilities the vanilla kits lack, many of them from the Season of Discovery era (ids above 400000). No list of "changed abilities" exists in
the core's source or in docs, and the ability dump (`BotAbilities:`) has not been run yet, so what the core really grants a bot is not known. The spec therefore
names abilities and the tool takes ids and levels from the dump: a name the dump does not show for the class is dropped with a warning, never guessed.

Rows added to `data/rotation_spec.txt` (all read in the client data: class skill line, cost, cooldown, range, effects):

| Class | Rows (priority as in the spec) |
| --- | --- |
| Warrior | Shockwave (2+ enemies), Spearing Strike (rage above 30) |
| Paladin | Rebuke (interrupt), Holy Strike, Seal of Fury, Hammer of the Righteous, before Judgement |
| Hunter | Summon Hawk (1293241, level 25), Strider Kick, Counterattack, Trueshot Aura |
| Rogue | Saber Slash (12 s bleed, one combo point), before Sinister Strike |
| Priest | Void Plague (18 s dot), Vampiric Touch (15 s dot), before Mind Blast |
| Shaman | Lava Burst before Lightning Bolt; Water Shield replaces Lightning Shield |
| Mage | Icy Veins (strong target), Ice Lance (power above 60%), before Frostbolt |
| Warlock | Conflagrate (destruction); Fel Armor replaces Demon Armor |
| Druid | Starsurge, Sunfire (12 s dot) before Wrath |

Black Arrow, Chimera Shot, Divine Storm, Wrack and Chaos Bolt (earlier rows) are in no guide and are not rows now. The policy counts Saber Slash as a combo builder (one point, like Sinister Strike; `ComboPointsGained` in `src/bot_policy.cpp`). `tests/make_rotations_test.py`
runs the tool on a made-up dump and checks these rows, their order, and that a dump without them gives no rows.

Not added:

- **Demonic Brand** (warlock, spell 1293695 is a talent-tree effect referenced by `TraitDefinition`; 1293696 is a level 26, 10 s proc aura on an enemy): its use (what
  it procs, whether it is cast) cannot be told from the tables read; no row until the dump shows it and its effect is known.
- Rogue Mutilate (needs daggers; gives two combo points, verified in the data), Between the Eyes, Envenom and other finishers or builders: the policy counts only the builders it knows.
- Victory Rush, Overpower, Revenge, Raging Blow and other abilities that need a proc or a state the snapshot does not show; Penance, Mind Flay type channels; Sniper Shot (long cast, 8 yard minimum range).
- Forms and stances, pets (Shadowfiend, Summon Hawk, Kill Command), totems, trait-tree (talent) spells.
