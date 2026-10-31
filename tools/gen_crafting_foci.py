#!/usr/bin/python3 -I
# SPDX-License-Identifier: GPL-2.0-or-later
"""Make data/crafting_foci.txt: the anvils, forges and cooking fires in the capitals, for the bots that must stand at one to craft.

    mysql forever_test_world -N -B -e "select g.map,round(g.position_x,1),round(g.position_y,1),round(g.position_z,1),t.Data0,t.Data1,t.entry,
        replace(t.name,'\t',' ') from gameobject g join gameobject_template t on t.entry=g.id
        where t.type=8 and t.Data0 in (1,3,4) and g.map in (0,1)" > foci.tsv
    python3 -I tools/gen_crafting_foci.py --foci foci.tsv --trainers data/errand_trainers.txt --out data/crafting_foci.txt

A spell focus is a gameobject of type 8 (GAMEOBJECT_TYPE_SPELL_FOCUS): Data0 is the focus id a recipe asks for (Spell.RequiresSpellFocus), Data1 the radius
in yards the crafter must stand within. The ids in this client: 1 anvil, 3 forge, 4 cooking fire. Only the objects within 120 yards of a profession
trainer of a capital are kept (the errands fly the bots to the capitals); of the fires, the 6 nearest the middle of each capital's trainers.
"""
import argparse
import math

FOCUS = {1: "anvil", 3: "forge", 4: "fire"}
NEAR_TRAINER_YARDS = 120.0
FIRES_PER_CITY = 6


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--foci", required=True)
    ap.add_argument("--trainers", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    cities: dict[tuple[str, int], list[tuple[float, float]]] = {}
    for line in open(args.trainers, encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        f = line.rstrip("\n").split("|")
        x, y, _z = (float(v) for v in f[5].split())
        cities.setdefault((f[1], int(f[2])), []).append((x, y))

    rows = []
    for line in open(args.foci, encoding="utf-8"):
        f = line.rstrip("\n").split("\t")
        if len(f) < 8:
            continue
        m, x, y, z, focus, radius, entry, name = int(f[0]), float(f[1]), float(f[2]), float(f[3]), int(f[4]), int(f[5]), int(f[6]), f[7]
        for (city, cm), pts in cities.items():
            if cm == m and min(math.hypot(x - a, y - b) for a, b in pts) <= NEAR_TRAINER_YARDS:
                cx = sum(a for a, _ in pts) / len(pts)
                cy = sum(b for _, b in pts) / len(pts)
                rows.append((city, m, focus, x, y, z, radius, entry, name or "(unnamed)", math.hypot(x - cx, y - cy)))
                break

    out = []
    for (city, _m), _pts in sorted(cities.items()):
        mine = [r for r in rows if r[0] == city]
        keep = [r for r in mine if r[2] != 4] + sorted((r for r in mine if r[2] == 4), key=lambda r: r[9])[:FIRES_PER_CITY]
        out += sorted(keep, key=lambda r: (r[2], r[9]))

    with open(args.out, "w", encoding="utf-8") as f:
        f.write("# Anvils, forges and cooking fires in the capitals, for crafting (src/crafting_foci.h). One per line:\n")
        f.write("# city|map|focus|x y z|radius|entry|name\n")
        f.write("# focus: 1 anvil, 3 forge, 4 cooking fire (the Spell.RequiresSpellFocus ids of this client). radius: yards the crafter must stand within.\n")
        f.write("# From the scratch world DB (gameobject_template type 8, read only), made by tools/gen_crafting_foci.py: only objects within 120 yards of a\n")
        f.write("# profession trainer of the capital; of the fires the 6 nearest the middle of the trainers.\n")
        for city, m, focus, x, y, z, radius, entry, name, _d in out:
            f.write(f"{city}|{m}|{focus}|{x:g} {y:g} {z:g}|{radius}|{entry}|{name}\n")
    print(f"{len(out)} objects -> {args.out}")


if __name__ == "__main__":
    main()
