#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Filters data/grind_spots.txt of spots the core finds no route to. Writes a new file; the input is never changed.

Rules (a row is dropped when either holds):
  1. zero:      its position is 0,0,0 (x, y and z all within 0.5): a spawn without a real position.
  2. isolated:  it has neighbours and every one of them differs in z by more than --max-dz yards (default 100). Neighbours are the other spots on the
                same map within --radius yards in x/y (default 250, about twice the 120-yard cell of make_grind_spots.py, so the ring of cells around a
                spot is covered, diagonals included). A spot whose nearest-in-height neighbour is within 100 yards of z sits on the same terrain as the
                others and is kept; a spot far above or below everything around it is typically on a plateau or in a pit with no walkable way to it.
                A spot with no neighbours within the radius is kept (nothing to compare with). Neighbours are taken from the unfiltered file, so
                the result does not depend on the order of the rows.
Comment lines and other lines are copied unchanged.

Usage: filter_grind_spots.py data/grind_spots.txt data/grind_spots.filtered.txt [--radius 250] [--max-dz 100]
"""
import argparse
import math
import sys
from collections import defaultdict


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source")
    parser.add_argument("target")
    parser.add_argument("--radius", type=float, default=250.0)
    parser.add_argument("--max-dz", type=float, default=100.0)
    args = parser.parse_args()
    if args.source == args.target:
        sys.exit("the target must not be the source")

    lines = open(args.source, encoding="utf-8").read().splitlines()
    spots = []  # (line index, map, x, y, z)
    for number, line in enumerate(lines):
        fields = line.split()
        if len(fields) >= 8 and fields[0] == "spot":
            spots.append((number, fields[1], float(fields[2]), float(fields[3]), float(fields[4])))

    # a grid of radius-sized buckets per map, so the neighbour search is not quadratic
    grid = defaultdict(list)
    for spot in spots:
        grid[(spot[1], int(spot[2] // args.radius), int(spot[3] // args.radius))].append(spot)

    dropped = {}
    for number, map_id, x, y, z in spots:
        if abs(x) < 0.5 and abs(y) < 0.5 and abs(z) < 0.5:
            dropped[number] = "zero"
            continue
        cx, cy = int(x // args.radius), int(y // args.radius)
        gaps = [abs(o[4] - z) for dx in (-1, 0, 1) for dy in (-1, 0, 1) for o in grid[(map_id, cx + dx, cy + dy)]
                if o[0] != number and math.hypot(o[2] - x, o[3] - y) <= args.radius]
        if gaps and min(gaps) > args.max_dz:
            dropped[number] = "isolated"

    with open(args.target, "w", encoding="utf-8") as out:
        for number, line in enumerate(lines):
            if number not in dropped:
                out.write(line + "\n")

    zero = sum(1 for v in dropped.values() if v == "zero")
    print(f"rows before: {len(spots)}  after: {len(spots) - len(dropped)}  dropped: {len(dropped)} (zero position: {zero}, isolated in height: {len(dropped) - zero})",
          file=sys.stderr)


if __name__ == "__main__":
    main()
