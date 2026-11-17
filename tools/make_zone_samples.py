#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Builds data/grind_zones.txt (the PathZoneFile of path variety, doc/path-variety.md) from a character export.

Input: a tab-separated file, one character per line: map zone x y   (SELECT map, zone, position_x, position_y FROM characters WHERE zone > 0).
The positions are thinned to one per --thin yards (default 120) per zone and written as `z <zone> <map> <x> <y>`.
Usage: make_zone_samples.py characters.tsv [more.tsv ...] > data/grind_zones.txt
"""
import argparse
import math
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("files", nargs="+")
    parser.add_argument("--thin", type=float, default=120.0)
    args = parser.parse_args()
    kept = []
    for name in args.files:
        with open(name, encoding="utf-8") as handle:
            for line in handle:
                parts = line.split()
                if len(parts) < 4:
                    continue
                try:
                    m, z, x, y = int(parts[0]), int(parts[1]), float(parts[2]), float(parts[3])
                except ValueError:
                    continue
                if z <= 0 or any(k[0] == m and k[1] == z and math.hypot(k[2] - x, k[3] - y) < args.thin for k in kept):
                    continue
                kept.append((m, z, x, y))
    print("# Zone samples for path variety (PathZoneFile). Format: z <zone> <map> <x> <y>; see src/path_variety.h.")
    for m, z, x, y in sorted(kept):
        print("z %d %d %.1f %.1f" % (z, m, x, y))


if __name__ == "__main__":
    sys.exit(main())
