# Raid rules (`data/raid_rules.txt`)

Status: built and unit-tested (`src/raid_rules.*`, `tests/raid_rules_test.cpp`, `tests/party_planner_test.cpp`, one policy test). **Not run on a sim yet.** Off until `Module.forever-bots.RaidRulesFile` names the file.

A raid fight needs facts that the per-boss block of a dungeon file does not carry and that repeat across maps. They live in one text file, one row per line, `|` separated, `#` for comments.
The format is our own; no table or code of another project was copied.

| Row | Fields | Used by |
|---|---|---|
| `boss` | `map|entry|name` | the swap taunt (which unit is "the boss") |
| `add` | `map|entry|rank|name` | kill priority, rank 0 first; merged at start into each scenario's kill priority (the dungeon file's own ranks win), so the targeting and the planner's shared kill target both read it |
| `interrupt` | `map|spell_id|name` | the planner serves a cast with this spell id before any other cast (the nearest ready kickers go to it first) |
| `dispel` | `map|aura_id|magic/curse/disease/poison|name` | the planner gives a member that carries this aura to the nearest ready member of a class that can dispel the kind; `TurnRaidRules` sends the cast |
| `avoid` | `map|spell|spell_id|radius|name` | a ground effect (dynamic object or area trigger) of this spell is harmful with this radius (needs `AvoidGroundEffects = 1`) |
| `avoid` | `map|object|entry|radius|name` | parsed and kept; **not acted on** (the snapshot has no game objects, see the gaps) |
| `swap` | `map|boss_entry|aura_id|stacks|name` | the main tank holding `stacks` of this aura is handed over to another tank with fewer; the new main tank (a warrior) taunts the boss |
| `noaoe` | `map|boss_entry|name` (0 = whole map) | `Order::NoAoe`, readable through `party_planner::NoAoeNow`; **no rotation reads it yet** |
| `frontal` | `map|boss_entry|spell_id|name` | parsed and kept (`Table::IsFrontal`); **no tank positioning uses it yet** |

Dispel spells by class (`DispelSpellFor`, first ranks; ids, dispel kinds and learn levels **verified 2026-11-12** against SpellName/SpellEffect/SpellLevels of the client data): priest Dispel Magic 527 and Cure Disease 528; paladin Cleanse 4987 (magic, level 42) and Purify 1152 (poison, disease); mage Remove Lesser Curse 475; druid Remove Curse 2782 and Cure Poison 8946; shaman Cure Poison 526 and Cure Disease 2870.
A healer on an emergency heal (a member below 20 %) is not given a dispel. A bot reads its **own** auras from its snapshot (`auras[]`, up to 16), so a member's debuff is seen when that member is one of the module's bots.

## What ships

Onyxia's Lair (249): the whelps first, then the wardens and the lair guards; Fireball and Deep Breath as `avoid` rows (casts with a landing area, not persistent ground effects: **they only matter if the core reports them as a ground effect**, see the gaps); Wing Buffet and Flame Breath as `frontal` rows; fear needs no tremor totem or fear ward from the party, so no row.
Zul'Gurub (309): interrupt Great Heal 23954 and 24208, Drain Life 24300, Mind Flay 23953; bats 11368 and 14965 first; dispel Curse of Blood 16098 (curse) and Shadow Word: Pain 23952 (magic); Liquid Fire (object 177764, radius 6); swap on Pierce Armor 12097 at 3 stacks for Jeklik 14517.
Spell ids verified 2026-11-12 against `SpellName.db2` and `SpellEffect.db2` (and the sim world DB `creature_template_spell` for Onyxia): Fireball 18392; Wing Buffet 18500; Flame Breath 18435 (also in Onyxia's spell list); the Deep Breath is eight spells named `Breath` (17086, 18351, 18564, 18576, 18584, 18596, 18609, 18617; one per flight direction, all `avoid` rows now; 18351 and 18564 are in her creature_template_spell, the rest are the same effect shape with trigger chains); the Zul'Gurub ids by name. No `(check)` mark is left in the file.
**Still open:** Bellowing Roar is 22686 in the spell data (18431 does not exist there; no row uses it). Which of the eight Breath spells a telegraph carries, and that Onyxia's script casts all eight, is read from the names and effect shape only: confirm with the `telegraph_escape:` log line on the first Onyxia run. The Fireball and the frontal radius values (8, and the breath's 35) are our estimates, not data.

## Gaps (what the snapshot does not offer)

- **Game objects.** `GroundEffect` (layout 21/25) carries a spell id, a position, a radius and a kind (1 dynamic object, 2 area trigger). A game object such as Liquid Fire (177764) has an entry, not a spell, and is not listed. The `object` row is parsed and tested but cannot be matched until the core lists game objects (or says which spell its dynamic object is).
- **Casts that land on an area** (Fireball, Deep Breath) arrive as `Telegraph` entries (layout 25: circle or cone, `spell_id`, `radius`, `cone_angle`). Since the telegraph escape (`TurnTelegraphEscape`, below) a bot inside one steps out; the `avoid|spell` rows act on ground effects and also mark a telegraph spell as lethal-class.
- **Debuffs on bots that are not the module's** (humans) are not in a snapshot; the core's party view (`PartyDebuff`, `dispel_type`) could feed the dispel plan once `PartyViewPlanner` adopts it.
- **Tank positioning and the no-AoE flag** have no consumer: no coordinates for the room, and the rotation rows have no area-damage marker.
- **Swap needs a second tank.** With one tank nothing changes; the taunt is warrior only.

## Telegraph escape

`Module.forever-bots.TelegraphEscape` (default ON; it only acts on a map that has rules in the raid rule table, so dungeons and the open world are untouched) and `TelegraphEscapeCasters` (default 0). Needs the core's `Bot.Snapshot.Telegraphs = 1`. The step runs right after the ground-effect step: a bot that stands inside a telegraph (`party_view::TelegraphsHitting`) walks to `EscapeFrom(.., margin 3)`; the end point is kept for 3 s so it does not shift each tick; the log line is `telegraph_escape: bot= map= spell= to=`.
Who holds: a **tank** (role tank, or a warrior in a party with no role set) holds unless the spell is an `avoid|spell` row (lethal-class) or a `frontal` row of any boss of the map; a bot **mid-cast** (healer or caster) holds unless the spell is an `avoid|spell` row or `TelegraphEscapeCasters = 1`. Everyone else steps out of any telegraph. The tank exception for lethal rows is deliberate: Deep Breath kills a tank as well.

## Settings

    Module.forever-bots.RaidRulesFile = /srv/forever/sim/modules_runtime/forever-bots/data/raid_rules.txt
    Module.forever-bots.AvoidGroundEffects = 1       # for the avoid rows
    # core: Bot.Snapshot.GroundEffects = 1

A bad row is logged (`forever-bots: RaidRulesFile: line N: ...`) and skipped. The start log says `raid rules: N map(s) from <file>`.

## What a sim run checks

Onyxia (`Scenario = onyxia`, the 40-roster) and Zul'Gurub (`Scenario = id:3` with the 20-roster, `ScenarioRaid.id = 1`): the same run with and without `RaidRulesFile`. Compare bosses killed, deaths per raid, time per boss, and the log events `raid_dispel:`, `raid_swap_taunt:` and `interrupt_target:`. Expect `raid_dispel` for Curse of Blood and Shadow Word: Pain in Zul'Gurub; expect no `raid_swap_taunt` unless the roster has two tanks.
