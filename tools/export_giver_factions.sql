-- Writes exports/givers.tsv for tools/make_world_quests.py: one row per quest and giver faction (quest, creature_template.faction), with no header.
-- The generator turns the faction ids into Alliance / Horde / neutral with tools/faction_sides.tsv and keeps a quest out of a side's list when every
-- giver of the quest belongs to the other side.
-- Run on the world database, read-only:  mysql -N <world db> < tools/export_giver_factions.sql > exports/givers.tsv
SELECT DISTINCT s.quest, ct.faction
FROM creature_queststarter s
JOIN creature_template ct ON ct.entry = s.id
ORDER BY s.quest, ct.faction;
