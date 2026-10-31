#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Replaces the route of every dungeon in a dungeons file with one planned over the navmesh (tools/mmap_check --plan): boss to boss, along the corridors.

    tools/mmap_check/build.sh /tmp/mmap_check
    python3 -I tools/replan_dungeons.py /tmp/mmap_check /srv/forever/data/mmaps data/dungeons.txt > data/dungeons_planned.txt

A dungeon whose map has no navmesh, or whose entry is off it, keeps its old route (and the reason is printed on stderr). The comment lines at the top of the file
are kept. The input needs bosses to plan between: run tools/make_dungeons_from_db.py first.
"""
import subprocess
import sys


def main(argv):
    if len(argv) != 4:
        print(__doc__)
        return 2
    tool, mmaps, path = argv[1], argv[2], argv[3]
    text = open(path, encoding="utf-8").read()
    blocks, head, current = [], [], None
    for line in text.splitlines():
        words = line.split()
        if words and words[0] == "dungeon":
            current = [line]
            blocks.append((words[1], current))
        elif current is not None:
            current.append(line)
            if words and words[0] == "end":
                current = None
        elif not blocks:
            head.append(line)
    out = list(head)
    out.append("# The routes below were planned over the navmesh by tools/replan_dungeons.py (tools/mmap_check --plan): boss to boss along the walkable path, nearest boss first.")
    out.append("# The order of the bosses is a guess (nearest first); the corners of the path are on the mesh. None of it has been walked by a bot party on a sim yet.")
    for dungeon_id, lines in blocks:
        block = "\n".join(lines) + "\n"
        result = subprocess.run([tool, mmaps, path, dungeon_id, "--plan"], capture_output=True, text=True)
        if result.returncode == 0 and "boss" in result.stdout:
            print(f"{dungeon_id}: {result.stderr.strip().splitlines()[-1]}", file=sys.stderr)
            out.append("")
            out.append(result.stdout.rstrip("\n"))
        else:
            print(f"{dungeon_id}: kept the old route ({(result.stderr.strip().splitlines() or ['no output'])[-1]})", file=sys.stderr)
            out.append("")
            out.append(block.rstrip("\n"))
    print("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
