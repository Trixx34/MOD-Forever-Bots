#!/usr/bin/python3 -I
# SPDX-License-Identifier: GPL-2.0-or-later
"""Make data/gather_nodes.txt: the herb and ore nodes of the continents for the bots' gather walk (src/gather_nodes.h).

Step 1, on the sim (read only), export every herb and ore spawn of the two continents (a node is a gameobject_template of type 3 named for the herb or ore):

    mysql forever_test_world -N -B -e "select g.map,round(g.position_x,1),round(g.position_y,1),round(g.position_z,1),t.entry,t.name
        from gameobject g join gameobject_template t on t.entry=g.id where t.type=3 and g.map in (0,1)" > nodes.tsv

Step 2, make the table (the grind spots give the level and faction of the area, so a bot walks only to nodes in areas that suit it):

    python3 -I tools/gen_gather_nodes.py --nodes nodes.tsv --spots data/grind_spots.filtered.txt --out data/gather_nodes.txt

A row is "skill map x y z entry requiredSkill areaLevel side". The skill line (182 Herbalism, 186 Mining) and the skill value the node needs come from the
NAME of the node (tables HERBS and ORES, the vanilla values): the lock ids of this world DB are not readable (the hotfixes `lock` table is empty on the sim), and
names are unambiguous. A node whose name is not in the tables is left out. areaLevel is the lowest minLevel of the grind spots within --radius yards on the
same map (the nearest one decides the side); a node with no spot near is left out (no mobs known there: cave, sea or an unreachable place), and so is a node
whose area level is above --max-level (default 60). Skinning has no nodes (corpses) and is not made here.
"""
import argparse
import math
from collections import defaultdict

HERBALISM, MINING = 182, 186

# name -> required skill value (vanilla 1.12 gathering skill levels). "Ooze Covered" ore variants share the value of the plain ore.
HERBS = {
    "Peacebloom": 1, "Silverleaf": 1, "Wilted Peacebloom": 1, "Stunted Silverleaf": 1, "Earthroot": 15, "Mageroyal": 50, "Briarthorn": 70,
    "Stranglekelp": 85, "Bruiseweed": 100, "Wild Steelbloom": 115, "Grave Moss": 120, "Kingsblood": 125, "Liferoot": 150, "Fadeleaf": 160,
    "Goldthorn": 170, "Khadgar's Whisker": 185, "Wintersbite": 195, "Firebloom": 205, "Purple Lotus": 210, "Arthas' Tears": 220, "Sungrass": 230,
    "Blindweed": 235, "Ghost Mushroom": 245, "Gromsblood": 250, "Golden Sansam": 260, "Dreamfoil": 270, "Mountain Silversage": 280,
    "Plaguebloom": 285, "Icecap": 290, "Black Lotus": 300,
}
ORES = {
    "Copper Vein": 1, "Poor Copper Vein": 1, "Tin Vein": 65, "Incendicite Mineral Vein": 65, "Silver Vein": 75, "Lesser Bloodstone Deposit": 75,
    "Iron Deposit": 125, "Gold Vein": 155, "Mithril Deposit": 175, "Truesilver Deposit": 230, "Dark Iron Deposit": 230, "Small Thorium Vein": 245,
    "Rich Thorium Vein": 275,
}


def classify(name):
    """(skill line, required skill) of a node name, or None."""
    base = name[len("Ooze Covered "):] if name.startswith("Ooze Covered ") else name
    if base in HERBS:
        return HERBALISM, HERBS[base]
    if base in ORES:
        return MINING, ORES[base]
    return None


def read_spots(path):
    """Grind spots: {map: [(x, y, minLevel, side)]} from 'spot map x y z min max side [weight]' lines."""
    spots = defaultdict(list)
    for line in open(path, encoding="utf-8"):
        f = line.split()
        if len(f) >= 8 and f[0] == "spot":
            spots[int(f[1])].append((float(f[2]), float(f[3]), int(f[5]), int(f[7])))
    return spots


class SpotGrid:
    def __init__(self, spots, cell):
        self.cell = cell
        self.grid = defaultdict(list)
        for s in spots:
            self.grid[(int(s[0] // cell), int(s[1] // cell))].append(s)

    def near(self, x, y, radius):
        """(areaLevel, side) from the spots within radius: the lowest minLevel, the side of the nearest spot; None when no spot is near."""
        cx, cy = int(x // self.cell), int(y // self.cell)
        reach = int(radius // self.cell) + 1
        best, level = None, None
        for gx in range(cx - reach, cx + reach + 1):
            for gy in range(cy - reach, cy + reach + 1):
                for sx, sy, lo, side in self.grid.get((gx, gy), ()):
                    d = math.hypot(sx - x, sy - y)
                    if d > radius:
                        continue
                    level = lo if level is None else min(level, lo)
                    if best is None or d < best[0]:
                        best = (d, side)
        return None if best is None else (level, best[1])


def make_rows(node_lines, spots, radius=150.0, max_level=60):
    """Rows (skill, map, x, y, z, entry, req, level, side) from nodes.tsv lines 'map x y z entry name'; also the counts of what was left out."""
    grids = {m: SpotGrid(s, radius) for m, s in spots.items()}
    rows, skipped = [], defaultdict(int)
    for line in node_lines:
        f = line.rstrip("\n").split("\t")
        if len(f) < 6:
            continue
        try:
            m, x, y, z, entry = int(f[0]), float(f[1]), float(f[2]), float(f[3]), int(f[4])
        except ValueError:
            continue
        kind = classify(f[5])
        if kind is None:
            skipped["unknown_name"] += 1
            continue
        area = grids[m].near(x, y, radius) if m in grids else None
        if area is None:
            skipped["no_spot_near"] += 1
            continue
        if area[0] > max_level:
            skipped["above_max_level"] += 1
            continue
        rows.append((kind[0], m, x, y, z, entry, kind[1], area[0], area[1]))
    rows.sort()
    return rows, skipped


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nodes", required=True)
    ap.add_argument("--spots", required=True, help="data/grind_spots.filtered.txt")
    ap.add_argument("--out", required=True)
    ap.add_argument("--radius", type=float, default=150.0)
    ap.add_argument("--max-level", type=int, default=60)
    args = ap.parse_args()

    with open(args.nodes, encoding="utf-8") as f:
        rows, skipped = make_rows(f, read_spots(args.spots), args.radius, args.max_level)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write("# Gathering nodes of the continents (src/gather_nodes.h). One per line: skill map x y z entry requiredSkill areaLevel side\n")
        f.write("# skill: 182 Herbalism, 186 Mining. Made by tools/gen_gather_nodes.py from the world DB (read only) and data/grind_spots.filtered.txt;\n")
        f.write("# areaLevel/side are those of the grind spots near the node (side 1 Alliance, 2 Horde, 0 both).\n")
        for skill, m, x, y, z, entry, req, level, side in rows:
            f.write(f"{skill} {m} {x:g} {y:g} {z:g} {entry} {req} {level} {side}\n")
    print(f"{len(rows)} nodes -> {args.out}; left out: {dict(skipped)}")


if __name__ == "__main__":
    main()
