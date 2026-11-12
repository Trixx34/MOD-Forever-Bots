-- Writes the faction exports for tools/make_world_quests.py (read-only, run on the world database):
--   mysql -N <world db> < tools/export_giver_factions.sql > exports/givers.tsv        (the first query: quest, creature_template.faction of each giver)
--   the second query, as exports/npc_factions.tsv: creature entry, faction of every quest giver and ender creature
-- The generator turns the faction ids into Alliance / Horde / neutral with tools/faction_sides.tsv. With --side it keeps a quest out of a side's list when
-- every giver of the quest belongs to the other side, and it does not use a giver or ender spawn of a creature of the other side (a hostile NPC cannot be
-- talked to); a quest with no usable giver or ender spawn is left out.
-- With no MySQL server, tools/export_world_quests.py writes both files from a SQLite dump (it needs the creature_template table too).
SELECT DISTINCT s.quest, ct.faction
FROM creature_queststarter s
JOIN creature_template ct ON ct.entry = s.id
ORDER BY s.quest, ct.faction;
-- npc_factions.tsv:
-- SELECT DISTINCT ct.entry, ct.faction FROM creature_template ct
-- WHERE ct.entry IN (SELECT id FROM creature_queststarter UNION SELECT id FROM creature_questender) ORDER BY ct.entry;
