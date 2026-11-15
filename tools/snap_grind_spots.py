#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Puts the spots of one map onto the navmesh: keeps only spots a bot can walk to. Reads the output of make_grind_spots.py for a map that has mmaps and writes
the spots again with positions taken from the mesh.

For each spot the nearest walkable polygon (navtool "near", 3 x 5 x 3 yards) must lie on the main island of the map, the island that holds most of the
spawns. A spot that is off the mesh (the average of a cell cut by a cliff or a lake) or on a small island is moved to the nearest spawn of the same spot
(within --radius yards, level inside the spot's band) that stands on the main island; with none it is dropped. Counts go to stderr.

navtool is scripts/dungeon-study/navtool.cpp of the forever-ops repo, built against the core's Detour (see tools/mmap_check/build.sh).

Usage: snap_grind_spots.py <navtool> <mmaps folder> <map> spawns.tsv spots.txt > map_spots.txt
"""
import math
import subprocess
import sys
from collections import Counter


def near(navtool, mmaps, map_id, points):
    text = "".join(f"near {x} {y} {z}\n" for x, y, z in points)
    out = subprocess.run([navtool, mmaps, str(map_id), "query"], input=text, capture_output=True, text=True, check=True).stdout.split("\n")
    result = []
    for line in out[:len(points)]:
        f = line.split()
        result.append((int(f[1]), float(f[2]), float(f[3]), float(f[4])))
    return result


def main():
    navtool, mmaps, map_id, spawn_file, spot_file = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4], sys.argv[5]
    radius = 120.0
    spawns = []
    for line in open(spawn_file, encoding="utf-8"):
        p = line.rstrip("\n").split("\t")
        try:
            if int(p[0]) == map_id:
                spawns.append((float(p[1]), float(p[2]), float(p[3]), float(p[4])))
        except (ValueError, IndexError):
            pass
    spawn_near = near(navtool, mmaps, map_id, [s[:3] for s in spawns])
    main_island = Counter(n[0] for n in spawn_near if n[0] >= 0).most_common(1)[0][0]
    on_main = [(s, n) for s, n in zip(spawns, spawn_near) if n[0] == main_island]

    spots = []
    for line in open(spot_file, encoding="utf-8"):
        f = line.split()
        if len(f) >= 8 and f[0] == "spot" and int(f[1]) == map_id:
            spots.append(f)
    snapped = near(navtool, mmaps, map_id, [(f[2], f[3], f[4]) for f in spots])
    kept = moved = dropped = 0
    for f, n in zip(spots, snapped):
        x, y, z, low, high = float(f[2]), float(f[3]), float(f[4]), int(f[5]), int(f[6])
        if n[0] == main_island:
            nx, ny, nz = n[1], n[2], n[3]
            kept += 1
        else:
            best = None
            for s, sn in on_main:
                d = math.hypot(s[0] - x, s[1] - y)
                if d <= radius and low - 1 <= s[3] <= high and (best is None or d < best[0]):
                    best = (d, sn)
            if best is None:
                dropped += 1
                continue
            nx, ny, nz = best[1][1], best[1][2], best[1][3]
            moved += 1
        print(f"spot {map_id} {nx:.1f} {ny:.1f} {nz:.1f} {f[5]} {f[6]} {f[7]} {f[8]}")
    print(f"map {map_id}: main island {main_island}; on it {kept}, moved onto it {moved}, dropped {dropped}", file=sys.stderr)


if __name__ == "__main__":
    main()
