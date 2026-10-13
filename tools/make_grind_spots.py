#!/usr/bin/env python3
"""Builds data/grind_spots.txt from an export of creature spawns (see doc/grind-spots.md).

Input: a tab-separated file, one creature spawn per line, with these columns (a header line is skipped):
    map  x  y  z  level   [side]
`level` is the creature's level (one number: the average of its min and max level is fine). `side` is optional: 0 both (default), 1 Alliance, 2 Horde,
for spawns that only one faction should grind (for example a faction's own starting area).

The spawns are cut into square cells (--cell yards, default 120). A cell with at least --min-count spawns becomes one spot at the average position of
its spawns, with the bot levels that suit it: from the 10th percentile of the creature levels to the 90th percentile plus 2 (a bot does not pull
more than two levels above itself). Neighbouring cells with the same band are not merged: the bot picks by distance and weight.

Usage: make_grind_spots.py spawns.tsv > data/grind_spots.txt
"""
import argparse
import math
import sys
from collections import defaultdict


def percentile(sorted_values, fraction):
    if not sorted_values:
        return 0
    index = min(len(sorted_values) - 1, max(0, int(round(fraction * (len(sorted_values) - 1)))))
    return sorted_values[index]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("spawns")
    parser.add_argument("--cell", type=float, default=120.0)
    parser.add_argument("--min-count", type=int, default=6)
    parser.add_argument("--max-level", type=int, default=60)
    args = parser.parse_args()

    cells = defaultdict(list)
    with open(args.spawns, encoding="utf-8") as handle:
        for number, line in enumerate(handle, 1):
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 5:
                continue
            try:
                map_id = int(parts[0])
                x, y, z = float(parts[1]), float(parts[2]), float(parts[3])
                level = int(round(float(parts[4])))
                side = int(parts[5]) if len(parts) > 5 and parts[5] != "" else 0
            except ValueError:
                if number > 1:
                    print(f"line {number}: not numeric, skipped", file=sys.stderr)
                continue
            if level < 1 or level > args.max_level + 3:
                continue
            key = (map_id, side, math.floor(x / args.cell), math.floor(y / args.cell))
            cells[key].append((x, y, z, level))

    spots = []
    for (map_id, side, _cx, _cy), spawns in cells.items():
        if len(spawns) < args.min_count:
            continue
        levels = sorted(s[3] for s in spawns)
        low = max(1, percentile(levels, 0.10))
        high = min(args.max_level, percentile(levels, 0.90) + 2)
        x = sum(s[0] for s in spawns) / len(spawns)
        y = sum(s[1] for s in spawns) / len(spawns)
        z = sum(s[2] for s in spawns) / len(spawns)
        spots.append((map_id, low, x, y, z, high, side, len(spawns)))

    spots.sort()
    print("# Grind spots (leveling). Made by tools/make_grind_spots.py from a creature spawn export. Format: src/grind_data.h")
    for map_id, low, x, y, z, high, side, count in spots:
        print(f"spot {map_id} {x:.1f} {y:.1f} {z:.1f} {low} {high} {side} {count}")
    print(f"# {len(spots)} spots", file=sys.stderr)


if __name__ == "__main__":
    main()
