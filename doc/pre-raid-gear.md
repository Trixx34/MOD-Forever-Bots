# Pre-raid gear tier

Goal: raid bots wear the best gear that exists before the raids (dungeon drops, crafted, quest rewards, PvP rank pieces, world blues and epics), no raid-tier drops.
Sim only; prod is untouched until the owner says so.

## How it works

1. `tools/gen_prebis.py` reads the world database (read-only SELECTs) and writes `data/prebis/<classId>.txt`, one item entry per line, with a comment line above each
   entry (`# slot=chest ilvl=68 quality=3 source=pvp <name>`). The lists in the repository were generated from the sim database.
2. `ScenarioGearFile = /path/to/data/prebis` makes a scenario read those lists. For each class without its own `ScenarioGear<classId>` key the Dressing phase grants the
   listed items one per bot per tick (`kGrant`) instead of sending `kAutoGear`. A key of your own wins over the file; a class with no file stays on auto gear.
3. The grant only puts the item in the bags. The core's equip service (bag upgrade pass, `EquipBagUpgrades`) wears the best of them by its gear score
   (item level times ten plus stat fit), but it applies the same `CanUseItem` and armour-weight checks as auto gear (see below), so an item the core refuses stays in the bag.

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
- not dropped by any creature or chest on the raid maps (`--raid-maps`, default 409 469 509 531 533 249 309 230): `creature_template_difficulty.LootID` of every creature
  spawned there and chest loot (`gameobject_template.Data1` of type 3), references followed recursively;
- has a source: dungeon drop (`--dungeon-maps`), world drop (a creature or reference drop elsewhere), quest reward, vendor, crafted (`spell_effect` create-item), or PvP
  (`item_sparse.RequiredPVPRank` / `RequiredPVPMedal`, or a rank title in the name: Knight-Captain's, Blood Guard's, General's, ...). Tag order when several apply:
  pvp, crafted, dungeon, quest, world, vendor. `--keep-unsourced` keeps the rest;
- quest rewards of raid quests are not a source: a quest in a raid zone (`QuestSortID` in `--raid-quest-sorts`), of a raid kind (`QuestInfoID` 62, 83) or suggested for ten
  or more players;
- usable by the class: allowable class mask, armour of the class's heaviest type at level 60 (plate for warrior and paladin, mail for hunter and shaman, leather for rogue
  and druid, cloth for the rest; lighter armour only for a slot with no heavier piece, as the core's `ChooseForSlot` does), weapon types of the class, shields for warrior,
  paladin and shaman, off-hand weapons only for dual wielders, relics by class, held items for caster classes.

Ranking inside a slot is the core's `GearScore`: item level times ten plus the stat score (three times a wanted primary stat, once another wanted stat, minus three times an
unwanted primary) with the stat list of the class profile (`PreferredStats` in `ModuleBotActions.cpp`). Per slot one item is kept; rings, trinkets and a dual wielder's
one-handers keep two (`--per-slot` changes the rest).

Roles: `ScenarioGear<classId>` and the file are keyed by class only, so one list per class. The warrior list uses the shield tank profile (the sim rosters send the warrior
as the tank); `--profile 1=0` makes it a damage list. Other classes use the class default stats. Two lists for one class (tank and damage) would need a role key in the
scenario; not built.

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

- Epics: allowed (decided); `--max-quality 3` for blues only.
- PvP rank pieces: included, tagged `source=pvp`; `--no-pvp` leaves them out. Counts per class are in the table below. High rank pieces (General's, Warlord's) may be more than
  "pre-raid" intends; decide whether to keep them.
- Test items ("N Green/Epic <class> ..."): excluded.
- Raid map list includes 230 (Blackrock Depths, a dungeon, as listed in the task): its loot is excluded from the lists. Remove 230 from `--raid-maps` if BRD loot is wanted.
- Raid-tier quest rewards that sit in no raid zone (Ahn'Qiraj gate quest rewards such as the Bronze Dragonflight rings, quest sort Tanaris) are still in the lists;
  no database column marks them.
- Vendor is a wide source in this database (13,544 vendor items); reputation vendors of raid factions are in.
- Prod: lists are for the sim; generating for prod is a later decision (the generator only needs a read-only mysql command).

## Regenerate

    tools/gen_prebis.py --mysql "ssh forever-test sudo -n mysql forever_test_world" --out data/prebis
    python3 -I tests/gen_prebis_test.py .          # or ctest -R gen_prebis

Counts of the committed lists (items, PvP pieces, epics, per slot):

    warrior   19 items  pvp=6 epic=11  chest=1 cloak=1 feet=1 finger=2 hands=1 head=1 legs=1 neck=1 ranged=1 shield=1 shoulder=1 trinket=2 waist=1 weapon1h=2 weapon2h=1 wrist=1
    paladin   18 items  pvp=5 epic=11  chest=1 cloak=1 feet=1 finger=2 hands=1 head=1 holdable=1 legs=1 neck=1 shield=1 shoulder=1 trinket=2 waist=1 weapon1h=1 weapon2h=1 wrist=1
    hunter    18 items  pvp=6 epic=8  chest=1 cloak=1 feet=1 finger=2 hands=1 head=1 legs=1 neck=1 ranged=1 shoulder=1 trinket=2 waist=1 weapon1h=2 weapon2h=1 wrist=1
    rogue     17 items  pvp=4 epic=9  chest=1 cloak=1 feet=1 finger=2 hands=1 head=1 legs=1 neck=1 ranged=1 shoulder=1 trinket=2 waist=1 weapon1h=2 wrist=1
    priest    18 items  pvp=6 epic=8  chest=1 cloak=1 feet=1 finger=2 hands=1 head=1 holdable=1 legs=1 neck=1 ranged=1 shoulder=1 trinket=2 waist=1 weapon1h=1 weapon2h=1 wrist=1
    shaman    19 items  pvp=6 epic=10  chest=1 cloak=1 feet=1 finger=2 hands=1 head=1 holdable=1 legs=1 neck=1 shield=1 shoulder=1 trinket=2 waist=1 weapon1h=2 weapon2h=1 wrist=1
    mage      18 items  pvp=6 epic=8  chest=1 cloak=1 feet=1 finger=2 hands=1 head=1 holdable=1 legs=1 neck=1 ranged=1 shoulder=1 trinket=2 waist=1 weapon1h=1 weapon2h=1 wrist=1
    warlock   18 items  pvp=5 epic=8  chest=1 cloak=1 feet=1 finger=2 hands=1 head=1 holdable=1 legs=1 neck=1 ranged=1 shoulder=1 trinket=2 waist=1 weapon1h=1 weapon2h=1 wrist=1
    druid     17 items  pvp=6 epic=8  chest=1 cloak=1 feet=1 finger=2 hands=1 head=1 holdable=1 legs=1 neck=1 shoulder=1 trinket=2 waist=1 weapon1h=1 weapon2h=1 wrist=1

## What to verify on the sim

- Log of the first Dressing phase: `grant` results (refusals "bags cannot hold the item", "unknown item"), then `autogear pick` lines from the equip pass.
- A dump of each bot's worn item levels (average and per slot): the list items should appear; any list item left in the bags points at a `CanUseItem` or `ArmorTooHeavy` refusal
  (`.item` equip error in the log), and its flags2, `MinReputation`, `RequiredSpell` and `AllowableRace` show why.
- Then a Zul'Gurub run to compare deaths and damage taken with the earlier runs.
