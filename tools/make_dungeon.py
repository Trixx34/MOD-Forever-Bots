#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Builds one dungeon block for data/dungeons.txt from an export of creature spawns (see doc/dungeons.md).

Input: a tab-separated file of the dungeon map's spawns, one per line, with these columns (a header line is skipped):
    entry  x  y  z  name  kind
`kind` is `boss` for the bosses and `pack` for the creature groups the route should pass (optional column; default `pack`). Lines are taken in
the order of the file, so sort the export in the order the party should meet them (the doc shows how to do it with a path length from the entry).

Packs closer than --merge yards to the previous waypoint are folded into it (the route would reach both anyway), so the route does not hold hundreds of
points. A boss is never folded away.

Usage: make_dungeon.py --id rfc --name "Ragefire Chasm" --levels 13 18 --roster 2:1,5:5,8:8,2:9,5:4 \
           --entry 389 3.81 -14.82 -17.84 [--portal 1 1811 -4410 -18] [--staging 1 1629 -4373 31] spawns.tsv >> data/dungeons.txt
"""
import argparse
import math
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("spawns")
    parser.add_argument("--id", required=True)
    parser.add_argument("--name", default="")
    parser.add_argument("--levels", nargs=2, type=int, required=True)
    parser.add_argument("--roster", required=True, help="race:class for each bot, tank first, healer second")
    parser.add_argument("--entry", nargs=4, type=float, required=True, metavar=("MAP", "X", "Y", "Z"))
    parser.add_argument("--portal", nargs=4, type=float, metavar=("MAP", "X", "Y", "Z"))
    parser.add_argument("--staging", nargs=4, type=float, metavar=("MAP", "X", "Y", "Z"))
    parser.add_argument("--duration", type=int, default=1800)
    parser.add_argument("--wipe-recovery", type=int, default=300)
    parser.add_argument("--merge", type=float, default=25.0, help="yards: packs closer than this to the previous waypoint are folded into it")
    args = parser.parse_args()

    points = []  # (entry, x, y, z, name, is_boss)
    with open(args.spawns, encoding="utf-8") as handle:
        for number, line in enumerate(handle, 1):
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 5:
                continue
            try:
                entry = int(parts[0])
                x, y, z = float(parts[1]), float(parts[2]), float(parts[3])
            except ValueError:
                if number > 1:
                    print(f"line {number}: not numeric, skipped", file=sys.stderr)
                continue
            kind = parts[5].strip() if len(parts) > 5 else "pack"
            points.append((entry, x, y, z, parts[4].strip().replace(",", " "), kind == "boss"))

    route = []
    for entry, x, y, z, name, is_boss in points:
        if not is_boss and route and not route[-1][5] and math.dist((x, y, z), route[-1][1:4]) < args.merge:
            continue
        route.append((entry, x, y, z, name, is_boss))
    if not any(p[5] for p in route):
        print("warning: no boss lines in the input; the run will be complete at the last waypoint", file=sys.stderr)

    out = sys.stdout
    out.write(f"dungeon {args.id}\n")
    if args.name:
        out.write(f"name {args.name}\n")
    out.write(f"levels {args.levels[0]} {args.levels[1]}\nroster {args.roster}\n")
    if args.staging:
        out.write("staging " + " ".join(f"{v:g}" for v in args.staging) + "\n")
    if args.portal:
        out.write("portal " + " ".join(f"{v:g}" for v in args.portal) + "\n")
    out.write("entry " + " ".join(f"{v:g}" for v in args.entry) + "\n")
    out.write(f"duration {args.duration}\n")
    if args.wipe_recovery:
        out.write(f"wipe_recovery {args.wipe_recovery}\n")
    for entry, x, y, z, name, is_boss in route:
        if is_boss:
            out.write(f"boss {entry} {x:.1f} {y:.1f} {z:.1f} {name}\n")
        else:
            out.write(f"go {x:.1f} {y:.1f} {z:.1f}\n")
    out.write("end\n")


if __name__ == "__main__":
    main()
