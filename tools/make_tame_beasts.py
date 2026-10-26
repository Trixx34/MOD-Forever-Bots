#!/usr/bin/python3 -I
"""Makes data/tame_beasts.txt: the beasts a hunter may tame (Tame Beast), from a read-only export of the world database.

    ssh forever-test sudo -n mysql -N -B forever_test_world -e "<the query below>" > tame_all.tsv
    python3 -I tools/make_tame_beasts.py tame_all.tsv > data/tame_beasts.txt

The query (tab separated: entry, name, family, faction, level_min, level_max, spawns):

    select ct.entry, ct.name, ct.family, ct.faction, l.level_min, l.level_max, count(*) n
    from creature_template ct
    join creature_template_difficulty ctd on ctd.Entry = ct.entry and ctd.DifficultyID = 0
    join creature_classic_level l on l.entry = ct.entry
    join creature c on c.id = ct.entry and c.map in (0, 1)
    where ct.type = 1 and (ctd.TypeFlags & 1) = 1 and ct.Classification = 0 and ct.family in (<the families below>) and l.level_max <= 45
    group by ct.entry, ct.name, ct.family, ct.faction, l.level_min, l.level_max having n >= 4 order by l.level_min, n desc

What makes a beast tameable in the core (spell_hunter.cpp, Tame Beast): a creature of a beast type with the tameable type flag (TypeFlags bit 1), of a family a pet may be. Normal creatures only (no
rare or elite), on a continent, with at least four spawns (a creature a hunter can find). The file is one line per beast: `entry family level_min level_max   # name`.
"""
import sys

FAMILIES = {1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 20, 21, 24, 25, 26, 27}


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    lines = []
    for raw in open(sys.argv[1], encoding="utf-8"):
        parts = raw.rstrip("\n").split("\t")
        if len(parts) < 7 or not parts[0].isdigit():
            continue
        entry, name, family, _faction, low, high, _spawns = parts[:7]
        if int(family) not in FAMILIES:
            continue
        lines.append((int(low), int(high), int(entry), int(family), name))
    lines.sort()
    print("# Beasts a hunter may tame (Tame Beast). Made by tools/make_tame_beasts.py from the world database; format: src/tame_beasts.h")
    print("# beast <entry> <family> <level min> <level max>   # name")
    for low, high, entry, family, name in lines:
        print(f"beast {entry} {family} {low} {high}   # {name}")


if __name__ == "__main__":
    main()
