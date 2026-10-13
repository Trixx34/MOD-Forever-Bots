# Grind spots: leveling 1 to 60

Status: built and unit-tested (`src/grind_data.*`, `tests/grind_test.cpp`); the data file is made from the world database. Without a data file
(`GrindFile` unset) idle bots wander between the hubs of their map, as before.

## What it does

An idle bot (nothing in sight for 30 s) used to walk to the next hub of its map, whatever its level. With `GrindFile` set it walks to a **grind
spot** that suits its level instead:

- a spot is a place with a level band (`spot <map> <x> <y> <z> <minLevel> <maxLevel> <side> [weight]`, see `src/grind_data.h`);
- the bot takes a spot on its map whose band holds its level, nearest first (richer spots count as a little closer), and not the two spots it
  visited last, so it moves on instead of going back and forth;
- with no spot for its level (over-leveled for the map) it takes the spots with the highest top level below its level;
- `side` keeps a faction's own areas for that faction (1 Alliance, 2 Horde, 0 both); the bot's faction comes from its race;
- there is no walking between maps: a bot on a map with no spot for it stays where it is (boats, zeppelins, portals and dungeons are not part of this);
- quests (M5) still come first when the quest file covers the bot's start; a party member follows its leader as before.

The module logs `event grind_spot: bot=... level=... map=... spot=... levels=a-b` for each walk.

## Making the data file (on the sim)

1. Look at the columns first (this fork has custom tables, as with the quest export):

        SHOW COLUMNS FROM creature;
        SHOW COLUMNS FROM creature_template;
        SHOW COLUMNS FROM creature_template_scaling;   -- if the levels live there

2. Export the spawns of ordinary hostile creatures as a tab-separated file, one per line: `map x y z level [side]`. Adapt the column names to step 1
   (`minlevel`/`maxlevel` may be named differently, or come from a scaling table). The filters keep out vendors, trainers and quest givers (`npcflag`),
   elites and bosses (`rank`), critters and totems (`type`), and creatures that never move or that sit in an instance map:

        SELECT c.map, ROUND(c.position_x, 1), ROUND(c.position_y, 1), ROUND(c.position_z, 1), (ct.minlevel + ct.maxlevel) / 2 AS level
        FROM creature c
        JOIN creature_template ct ON ct.entry = c.id
        WHERE ct.npcflag = 0
          AND ct.`rank` = 0
          AND ct.type NOT IN (8, 11, 12, 13)         -- critter, totem, non-combat pet, gas cloud
          AND c.map IN (0, 1)                        -- the two continents; add 530 or others if the fork has them
          AND ct.minlevel BETWEEN 1 AND 60;

   Save the result as `spawns.tsv` (`mysql -N -B ... > spawns.tsv`; a header line is fine, it is skipped).

3. Turn it into spots:

        python3 -I tools/make_grind_spots.py spawns.tsv > data/grind_spots.txt

   `--cell 120` (yards per cell), `--min-count 6` (spawns a cell needs) and `--max-level 60` are the knobs. Each cell becomes one spot at the average
   position of its spawns, with the band from the 10th percentile of the creature levels to the 90th percentile plus 2 (a bot does not pull more than
   two levels above itself).

4. Set `GrindFile = "data/grind_spots.txt"` in the module's config (a path relative to the module's working directory).

## Known limits

- The spot is the average position of the creatures in a 120-yard cell: a cell cut by a lake or a cliff can put the spot where nothing is (the bot walks
  there, finds nothing, and moves on after 30 s). A smaller `--cell` helps at the cost of more, smaller spots.
- No faction filter on hostility: a spot may hold creatures that are friendly to one faction. The bot only pulls what the core says it may fight.
- Spots do not know about water, caves or walls: the server's pathing decides whether the bot gets there (see the `_checkPathLengths` note in doc 14).
