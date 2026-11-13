# Pre-raid gear tier

Goal: raid bots wear the best gear that exists before the raids (dungeon drops, crafted, quest rewards, PvP rank pieces, world blues and epics), no raid-tier drops.
Sim only; prod is untouched until the owner says so.

## How it works

1. `tools/gen_prebis.py` reads the world database (read-only SELECTs) and writes `data/prebis/<classId>.txt` (and the same lists again in `data/prebis-nopvp/`, see Variants), one item entry per line, with a comment line above each
   entry (`# slot=chest ilvl=68 quality=3 source=pvp <name>`). The lists in the repository were generated from the sim database.
2. `ScenarioGearFile = /path/to/data/prebis` makes a scenario read those lists. For each class without its own `ScenarioGear<classId>` key the Dressing phase grants the
   listed items one per bot per tick (`kGrant`) instead of sending `kAutoGear`. A key of your own wins over the file; a class with no file stays on auto gear.
3. The grant only puts the item in the bags. The core's equip service (bag upgrade pass, `EquipBagUpgrades`) wears the best of them by its gear score
   (item level times ten plus the core's stat fit), but it applies the same `CanUseItem` and armour-weight checks as auto gear (see below), so an item the core refuses stays in the bag.

Enable on the sim (config of the sim, not edited by this change):

    # worldserver.conf
    Bot.Actions.Grant = 1            # default 0; without it every grant is refused
    Bot.AutoGear.MaxQuality = 3      # unchanged: auto gear stays at blues; the epics arrive by the lists
    # module config (forever-bots)
    ScenarioGearFile = /srv/forever/.../data/prebis

Bag space: a list holds 17-19 items. The 16-slot backpack holds them only because each one is equipped as soon as it is granted; a bot with full bags gets
"the bot's bags cannot hold the item" for the rest. Check the refusals in the module log on the first run.

## Selection rules (no item entry is ever used to select)

All of it is read from the item columns and the loot tables:

- class weapon or armour, quality 3-4 (`--min-quality`, `--max-quality`), item level 58-70 (`--min-ilvl`, `--max-ilvl`), required level at most 60 (`--level`), an equipment slot;
- not quest-bound (`bonding` 4), no `requiredSkill` (profession-gated pieces: the bot may lack the profession);
- not a test item: a placeholder name (the core's own rule: deprecated, unused, [ph], zz..., test..., plus monster - and qa), or a name that begins with its own item
  level ("90 Green Warrior Axe", "63 Epic Rogue Tunic"). Items with no source at all (below) are dropped too, so a test item with a normal name is still out;
- not dropped by any creature or chest on the raid maps (`--raid-maps`, default 409 469 509 531 533 249 309): `creature_template_difficulty.LootID` of every creature
  spawned there and chest loot (`gameobject_template.Data1` of type 3), references followed recursively;
- has a source: dungeon drop (`--dungeon-maps`), world drop (a creature or reference drop elsewhere), quest reward, vendor, crafted (`spell_effect` create-item), or PvP
  (`item_sparse.RequiredPVPRank` / `RequiredPVPMedal`, or a rank title in the name: Knight-Captain's, Blood Guard's, General's, ...). Tag order when several apply:
  pvp, crafted, dungeon, quest, world, vendor. `--keep-unsourced` keeps the rest;
- quest rewards of raid quests are not a source (see Quest chains of raids below);
- usable by the class: allowable class mask, armour of the class's heaviest type at level 60 (plate for warrior and paladin, mail for hunter and shaman, leather for rogue
  and druid, cloth for the rest; lighter armour only for a slot with no heavier piece, as the core's `ChooseForSlot` does), weapon types of the class, shields for warrior,
  paladin and shaman, off-hand weapons only for dual wielders, relics by class, held items for caster classes.

## Ranking, profiles and weapons

Ranking inside a slot, in this order:

1. PvE first: a candidate that is not PvP and has a positive score beats every PvP piece (PvP rank pieces only fill a slot with no PvE option);
2. profile score (below), higher first;
3. item level, then quality, then name (deterministic; the item entry is only the last tie-break and never a filter).

The profile score is a weighted sum of the item's stats, plus `armor` and weapon damage per second where the profile has those keys. This replaces the earlier
"item level first" order, so a dungeon, crafted, quest, vendor or world piece with the right stats beats a PvP piece of a higher level.
Weights live in `PROFILES` in `tools/gen_prebis.py` (`--profile CLASS=NAME` overrides a class). Ranged weapons (bow, gun, crossbow, thrown, wand) use their own
`ranged_dps` weight when the profile has one; the hunter weights ranged dps 8 and melee dps 1.

Roles: the lists are keyed by class, so one profile per class. The ZG roster (and the other sim rosters) sends warrior as the tank, priest as the healer,
mage and warlock as caster damage, rogue as melee damage and hunter as ranged damage. Paladin, shaman and druid are in no roster, so they get the healer profile.

| class | profile | main weights |
|---|---|---|
| warrior | tank | Sta, Def, Dodge, Parry, Block, block value, armour, Str/Agi small, hit/crit small |
| paladin, shaman, druid, priest | healer | Int, spell healing, spell power, MP5, Sta small; Str, Agi, AP negative |
| mage, warlock | caster | spell power/damage, Int, Crit, Hit, Sta small; Spi, Str, Agi, AP negative |
| rogue | rogue | Agi, AP, Hit, Crit, Sta; weapon dps |
| hunter | hunter | Agi, RAP/AP, Hit, Crit, Int, Sta; ranged dps heavy |

Stat codes used (core `ItemModType`, read from the item `stats` column): 3 Agi, 4 Str, 5 Int, 6 Spi, 7 Sta, 12 Def, 13 Dodge, 14 Parry, 15 Block, 31 Hit,
32 Crit, 38 AP, 39 RAP, 41 spell healing, 42 spell damage, 43 MP5, 45 spell power, 48 block value, 50 extra armour, 51-56 resistances.

Weapons: the generator picks one consistent weapon set per class (`WEAPON_MODE`): warrior, paladin and shaman take main hand plus shield,
rogue main hand plus off hand, hunter a two-hander plus ranged, priest, casters and druid the best of two-hander or one-hander plus holdable (`auto`). The equip service wears bag
items best gear score first into the first slot that fits, and a shield or off-hand cannot follow a two-hander, so a mixed list could end up with a two-hander
blocking the shield. One set avoids that. Gear score of the core is not the profile score, so the core may still pick another list item over the intended one of the same slot.

Per slot one item is kept; rings, trinkets and a dual wielder's one-handers keep two (`--per-slot` changes the rest).

## Quest chains of raids and the AQ gate

Quest rewards count as a source only when the quest is not a raid quest: not in a raid zone (`QuestSortID` in `--raid-quest-sorts`: 19, 2717, 2677, 2159, 3429, 3428,
3456, 1977), not of raid kind (`QuestInfoID` in `--raid-quest-infos`: 62, 83), not suggested for 10 or more players, and without the raid quest flag (0x40).
Then the chain is closed: a quest that follows a raid quest (`PrevQuestID` walked backward) or shares a positive `ExclusiveGroup` with one is excluded as well. This removes
the Ahn'Qiraj gate and Scarab Lord line (Bronze Dragonflight signets and the like) with no item entry named anywhere.

## Items 227070, 227074, 227075

Forever's higher-Stamina variants of standard PvP pieces, in the hotfix `item_sparse` with `RequiredPVPRank` 11 and 12. They are tagged pvp and, with PvE first, they are
used only if no PvE piece exists for the slot; none of them is in the current lists.

## Variants

`data/prebis/` is generated with PvP allowed (PvE first), `data/prebis-nopvp/` with `--no-pvp`. Because PvE always wins, the item lists are identical (only the header
line differs); keep both so that a future change in ranking or data shows up as a difference. Point `ScenarioGearFile` at either directory.

## Why auto gear takes poor items (CanUseItem finding)

Read from `AutoGearAction` (`ModuleBotActions.cpp`), `Player::CanUseItem(ItemTemplate const*)` and `BotGearRules.h`. A candidate is dropped when:

1. quality is below common or above `Bot.AutoGear.MaxQuality` (3 on the sim: all 2,231 epics are out of auto gear's pool), it is quest-bound, has no slot, or its name is a
   placeholder (starts with "test", contains deprecated, unused, [ph], or starts with zz);
2. `GetBaseRequiredLevel()` is above the bot's level;
3. `CanUseItem` fails: the INTERNAL_ITEM flag (flags2), a faction flag (Horde or Alliance only), `AllowableClass` or `AllowableRace` not matching, `RequiredSkill` unknown or
   below `RequiredSkillRank` (item_sparse), `RequiredSpell` not known, a holiday that is not active, `MinReputation` rank not reached;
4. `ArmorTooHeavy`: a body slot piece whose armour subclass is above the class maximum.

What was verified on the sim: the pool log line (`maxquality=3 ... candidates=9542`) matches armory_item (9,287 weapons and armour of quality 1-3), so the item store holds the
test items and the pool is not short of them; the log shows warriors wearing the ilvl 90 test pieces, so those pass; the "Green Rogue Tunic" (leather, no class mask, no
skill, required level 60 in armory_item) passes every check of items 1-4 as far as armory_item shows, and so do the ilvl 58-70 rings. Runtime data differs from armory_item
in places (item 20250 logs reqlevel 0, armory says 60), and the columns that decide 3 (flags2, MinReputation, RequiredSpell, AllowableRace) are only in the client item data
that the core loads, not in armory_item (the hotfix `item_sparse` table covers 5,677 of 24,243 armory items). So the exact reason a rogue skipped the tunic is NOT proven by
reading. What the code shows for certain: epics never reach auto gear on the sim, and the list path avoids the pool filter. What to check on the sim is in the last section.

## Owner decisions

- Epics (quality 3-4): allowed; `--max-quality 3` for blues only.
- PvE first: PvE, crafted, quest, vendor, dungeon and world pieces are used whenever one exists for the slot and fits the profile; PvP rank pieces only where no PvE option
  exists. `data/prebis-nopvp/` stays as the variant without PvP.
- AQ gate quest rewards (Scarab Lord line, AQ opening event) excluded by the quest rule above, not by item ID.
- Test items ("N Green/Epic <class> ..."): excluded.
- Raid maps are 409 469 509 531 533 249 309 (Molten Core, Blackwing Lair, AQ20, AQ40, Naxxramas, Onyxia, Zul'Gurub). Blackrock Depths (230) is a five-man dungeon: its loot is allowed and tagged dungeon.
- Vendor is a wide source in this database (13,544 vendor items); reputation and battleground vendor pieces (Defiler's, Outrider's, insignia) stay tagged vendor or quest, not pvp.
- Sim only; prod unchanged. Generating for prod is a later decision.

## Regenerate

    tools/gen_prebis.py --mysql "ssh forever-test sudo -n mysql forever_test_world" --out data/prebis --also-nopvp data/prebis-nopvp
    python3 -I tests/gen_prebis_test.py .          # or ctest -R gen_prebis

Counts of the committed lists (identical in both variants; no slot is empty except where a class has no item of the kind: no shield for rogue, hunter and the casters,
no off hand or held item for classes that use a two-hander):

    warrior  17 items pvp=0 epic=12  chest=1 cloak=1 feet=1 finger=2 hands=1 head=1 legs=1 mainhand=1 neck=1 ranged=1 shield=1 shoulder=1 trinket=2 waist=1 wrist=1
    paladin  16 items pvp=0 epic=6  chest=1 cloak=1 feet=1 finger=2 hands=1 head=1 legs=1 mainhand=1 neck=1 shield=1 shoulder=1 trinket=2 waist=1 wrist=1
    hunter   16 items pvp=0 epic=8  chest=1 cloak=1 feet=1 finger=2 hands=1 head=1 legs=1 neck=1 ranged=1 shoulder=1 trinket=2 waist=1 weapon2h=1 wrist=1
    rogue    17 items pvp=0 epic=9  chest=1 cloak=1 feet=1 finger=2 hands=1 head=1 legs=1 mainhand=1 neck=1 offhand=1 ranged=1 shoulder=1 trinket=2 waist=1 wrist=1
    priest   16 items pvp=0 epic=8  chest=1 cloak=1 feet=1 finger=2 hands=1 head=1 legs=1 neck=1 ranged=1 shoulder=1 trinket=2 waist=1 weapon2h=1 wrist=1
    shaman   16 items pvp=0 epic=7  chest=1 cloak=1 feet=1 finger=2 hands=1 head=1 legs=1 mainhand=1 neck=1 shield=1 shoulder=1 trinket=2 waist=1 wrist=1
    mage     16 items pvp=0 epic=9  chest=1 cloak=1 feet=1 finger=2 hands=1 head=1 legs=1 neck=1 ranged=1 shoulder=1 trinket=2 waist=1 weapon2h=1 wrist=1
    warlock  16 items pvp=0 epic=9  chest=1 cloak=1 feet=1 finger=2 hands=1 head=1 legs=1 neck=1 ranged=1 shoulder=1 trinket=2 waist=1 weapon2h=1 wrist=1
    druid    15 items pvp=0 epic=4  chest=1 cloak=1 feet=1 finger=2 hands=1 head=1 legs=1 neck=1 shoulder=1 trinket=2 waist=1 weapon2h=1 wrist=1

## Lists for hunter, mage and warrior


#### hunter

| slot | item | id | ilvl | q | source | main stats |
|---|---|---|---|---|---|---|
| head | Beastmaster's Cap | 22013 | 60 | 4 | quest | Agi 22, Sta 21, Int 12, Hit 10, arm 314 |
| neck | Onyxia Tooth Pendant | 18404 | 65 | 4 | quest | Agi 7, Sta 8, Hit 10, Crit 14 |
| shoulder | Defiler's Chain Pauldrons | 20158 | 65 | 4 | vendor | Agi 20, Sta 18, Int 17, arm 312 |
| cloak | Earthweave Cloak | 21187 | 62 | 3 | quest | Agi 15, Hit 10, arm 44 |
| chest | Beastmaster's Tunic | 22060 | 60 | 4 | quest | Agi 25, Sta 16, Int 13, Crit 14, arm 387 |
| wrist | Beastmaster's Bindings | 22011 | 65 | 3 | quest | Agi 16, Sta 7, Int 5, arm 167 |
| hands | Beaststalker's Gloves | 16676 | 59 | 3 | dungeon | Agi 14, Spi 10, Sta 10, Int 10, arm 218 |
| waist | Defiler's Chain Girdle | 20150 | 63 | 3 | vendor | Sta 10, Crit 14, AP 34, arm 208 |
| legs | Outrider's Chain Leggings | 22673 | 65 | 4 | vendor | Agi 35, Sta 15, Crit 14, Hit 10, arm 364 |
| feet | Beastmaster's Boots | 22061 | 60 | 4 | quest | Agi 24, Sta 9, arm 266 |
| finger | Don Julio's Band | 19325 | 65 | 4 | vendor | Sta 11, Crit 14, Hit 10, AP 16 |
| finger | Band of Earthen Might | 21182 | 66 | 3 | quest | Str 6, Crit 14, Hit 10 |
| trinket | Defiler's Talisman | 20072 | 63 | 3 | vendor | Sta 12 |
| trinket | Talisman of Arathor | 20071 | 63 | 3 | vendor | Sta 12 |
| weapon2h | Ironbark Staff | 20069 | 65 | 4 | vendor | Int 10, Sta 19, Crit 28, dps 61.5 |
| ranged | Bloodseeker | 19107 | 63 | 3 | quest | Agi 23, Str 26, dps 54.0 |

#### mage

| slot | item | id | ilvl | q | source | main stats |
|---|---|---|---|---|---|---|
| head | Sorcerer's Crown | 22065 | 60 | 4 | quest | Int 25, Spi 14, Sta 16, SP 11, Crit 14, arm 75 |
| neck | Onyxia Tooth Pendant | 18404 | 65 | 4 | quest | Agi 7, Sta 8, Hit 10, Crit 14 |
| shoulder | Defiler's Epaulets | 20176 | 65 | 4 | vendor | Sta 18, Int 17, SP 12, MP5 4, arm 75 |
| cloak | Frostwolf Advisor's Cloak | 19085 | 60 | 3 | vendor | Sta 11, SP 14, arm 43 |
| chest | Earthpower Vest | 21183 | 66 | 3 | quest | Int 22, Crit 14, SP 25, arm 93 |
| wrist | Rockfury Bracers | 21186 | 62 | 4 | quest | Sta 7, SP 27, Hit 10, arm 42 |
| hands | Dreadmist Wraps | 16705 | 59 | 3 | dungeon | Spi 14, Int 9, Sta 13, Hit 10, arm 52 |
| waist | Defiler's Cloth Girdle | 20163 | 63 | 3 | vendor | Sta 7, Int 6, Crit 14, SP 14, arm 50 |
| legs | Outrider's Silk Leggings | 22747 | 65 | 4 | vendor | Sta 23, Int 19, Spi 10, SP 28, arm 88 |
| feet | Sorcerer's Boots | 22064 | 60 | 4 | quest | Spi 10, Int 16, Sta 14, SP 21, arm 64 |
| finger | Don Julio's Band | 19325 | 65 | 4 | vendor | Sta 11, Crit 14, Hit 10, AP 16 |
| finger | Dragonslayer's Signet | 18403 | 65 | 4 | quest | Int 11, SP 7, Sta 11, Crit 14 |
| trinket | Defiler's Talisman | 20072 | 63 | 3 | vendor | Sta 12 |
| trinket | Talisman of Arathor | 20071 | 63 | 3 | vendor | Sta 12 |
| weapon2h | Ironbark Staff | 20069 | 65 | 4 | vendor | Int 10, Sta 19, Crit 28, dps 61.5 |
| ranged | Stormrager | 16997 | 62 | 3 | quest | Sta 11, Int 7, dps 40.8 |

#### warrior

| slot | item | id | ilvl | q | source | main stats |
|---|---|---|---|---|---|---|
| head | Helm of Heroism | 21999 | 60 | 4 | quest | Sta 32, Str 18, Crit 14, arm 556 |
| neck | Amulet of the Darkmoon | 19491 | 65 | 4 | quest | Agi 19, Sta 10, Str 10 |
| shoulder | Defiler's Plate Spaulders | 20212 | 65 | 4 | vendor | Str 18, Sta 20, Agi 17, arm 553 |
| cloak | Cloak of the Honor Guard | 20073 | 65 | 4 | vendor | Sta 11, Agi 5, AP 34, arm 50 |
| chest | Breastplate of Heroism | 21997 | 60 | 4 | quest | Sta 26, Str 21, Agi 13, Hit 10, arm 684 |
| wrist | Bracers of Heroism | 21996 | 65 | 3 | quest | Sta 14, Str 9, Agi 5, Def 3, arm 296 |
| hands | Gauntlets of Valor | 16737 | 59 | 3 | world | Str 17, Sta 10, Spi 8, Agi 3, Def 4, arm 386 |
| waist | Belt of Heroism | 21994 | 65 | 3 | quest | Str 15, Sta 12, Agi 9, Def 7, arm 380 |
| legs | Outrider's Plate Legguards | 22651 | 65 | 4 | vendor | Str 28, Sta 27, Crit 14, Hit 10, arm 646 |
| feet | Boots of Heroism | 21995 | 60 | 4 | quest | Sta 20, Str 20, Hit 10, arm 470 |
| finger | Protector's Band | 19514 | 63 | 3 | vendor | Str 12, Agi 11, Sta 8 |
| finger | Don Julio's Band | 19325 | 65 | 4 | vendor | Sta 11, Crit 14, Hit 10, AP 16 |
| trinket | Frostwolf Insignia Rank 6 | 17909 | 60 | 4 | quest | Dodge 24 |
| trinket | Stormpike Insignia Rank 6 | 17904 | 60 | 4 | quest | Dodge 24 |
| mainhand | Mindfang | 20214 | 65 | 4 | vendor | Sta 8, Crit 14, dps 47.4 |
| shield | The Immovable Object | 19321 | 65 | 4 | vendor | Sta 15, BlkVal 27, arm 2468 |
| ranged | Outrider's Bow | 19558 | 63 | 3 | vendor | Sta 33, Agi 13, dps 53.9 |

## What to verify on the sim

- Log of the first Dressing phase: `grant` results (refusals "bags cannot hold the item", "unknown item"), then `autogear pick` lines from the equip pass.
- A dump of each bot's worn item levels (average and per slot): the list items should appear; any list item left in the bags points at a `CanUseItem` or `ArmorTooHeavy` refusal
  (`.item` equip error in the log), and its flags2, `MinReputation`, `RequiredSpell` and `AllowableRace` show why.
- Then a Zul'Gurub run to compare deaths and damage taken with the earlier runs.
