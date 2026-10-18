#!/usr/bin/env python3
"""Writes data/dungeons.txt blocks for the classic five-man dungeons from a SQLite file made by tools/dump_to_sqlite.py (doc/dungeons.md).

    python3 -I tools/dump_to_sqlite.py world.sql.gz world.sqlite creature creature_template areatrigger_teleport world_safe_locs
    python3 -I tools/make_dungeons_from_db.py world.sqlite [--only rfc,wc] > data/dungeons.txt

What comes from the database: the door (the "Classic: <name> - Entrance" and "- Exit" teleport targets: the party is teleported to the entrance, and a ghost
released outside walks to the exit's place), the hostile spawns on the dungeon's map, and the boss creatures (found by NAME, since this database carries
no boss flags or scripts; a name that finds no spawn on the map is reported on stderr). What is a guess: the ORDER of the route. Spawns are folded into packs
(creatures within --pack yards of each other), and the route visits the pack or boss nearest to where the party stands next, starting at the entrance, with a
penalty for a different floor. That ignores walls and one-way routes; the core moves bots along its navmesh between waypoints, so it works as an order, but a
dungeon with a dead end will be walked into and back. Check each route on the sim and edit the `go` lines that are wrong.
"""
import argparse
import math
import sqlite3
import subprocess
import sys
import os

# id: (map, name, min level, max level, boss names). Levels are the usual vanilla ranges. Zul'Farrak is left out: five of its nine bosses are summoned by an
# event and have no spawn, so a "kill every boss" rule could never be met. The Temple of Atal'Hakkar has no "Classic:" door in this database.
DUNGEONS = {
    "rfc": (389, "Ragefire Chasm", 13, 18, ["Oggleflint", "Taragaman the Hungerer", "Jergosh the Invoker", "Bazzalan"]),
    "wc": (43, "Wailing Caverns", 15, 25, ["Lady Anacondra", "Lord Cobrahn", "Kresh", "Lord Pythas", "Skum", "Lord Serpentis", "Verdan the Everliving", "Mutanus the Devourer"]),
    "dm": (36, "Deadmines", 18, 23, ["Rhahk'Zor", "Sneed's Shredder", "Gilnid", "Mr. Smite", "Captain Greenskin", "Edwin VanCleef"]),
    "sfk": (33, "Shadowfang Keep", 22, 30, ["Rethilgore", "Razorclaw the Butcher", "Baron Silverlaine", "Commander Springvale", "Odo the Blindwatcher", "Fenrus the Devourer", "Wolf Master Nandos", "Archmage Arugal"]),
    "stockade": (34, "Stormwind Stockades", 24, 32, ["Targorr the Dread", "Kam Deepfury", "Hamhock", "Bazil Thredd", "Dextren Ward", "Bruegal Ironknuckle"]),
    "bfd": (48, "Blackfathom Deeps", 24, 32, ["Ghamoo-ra", "Lady Sarevess", "Gelihast", "Baron Aquanis", "Twilight Lord Kelris", "Old Serra'kis", "Aku'mai"]),
    "gnomer": (90, "Gnomeregan", 29, 38, ["Grubbis", "Viscous Fallout", "Electrocutioner 6000", "Crowd Pummeler 9-60", "Dark Iron Ambassador", "Mekgineer Thermaplugg"]),
    "rfk": (47, "Razorfen Kraul", 29, 38, ["Aggem Thorncurse", "Death Speaker Jargba", "Overlord Ramtusk", "Agathelos the Raging", "Charlga Razorflank"]),
    "rfd": (129, "Razorfen Downs", 37, 46, ["Tuten'kash", "Mordresh Fire Eye", "Glutton", "Ragglesnout", "Amnennar the Coldbringer"]),
}
ROSTER = "2:1,5:5,8:8,2:9,5:4"              # tank first, healer second: orc warrior, undead priest, troll mage, orc warlock, undead rogue
STAGING = ("1", "1629", "-4373", "31")      # Orgrimmar, Valley of Strength


def door(db, name):
    """(entrance map, x, y, z) and (exit map, x, y, z) of the teleports named '<name> - Entrance' / '- Exit'."""
    found = {}
    for kind in ("Entrance", "Exit"):
        row = db.execute("""SELECT s.MapID, s.LocX, s.LocY, s.LocZ FROM areatrigger_teleport a JOIN world_safe_locs s ON s.ID = a.PortLocID
                            WHERE s.Comment LIKE ? ORDER BY s.ID DESC LIMIT 1""", (f"Classic: {name} - {kind}",)).fetchone()
        found[kind] = row
    return found["Entrance"], found["Exit"]


def hostile_spawns(db, map_id):
    return db.execute("""SELECT c.id, t.name, c.position_x, c.position_y, c.position_z FROM creature c JOIN creature_template t ON t.entry = c.id
                         WHERE c.map = ? AND t.npcflag = 0 AND t.type NOT IN (8, 11, 12, 13) AND t.faction != 35 AND (COALESCE(c.unit_flags, 0) & 2) = 0""", (map_id,)).fetchall()


def cost(a, b):
    return math.hypot(a[0] - b[0], a[1] - b[1]) + 3.0 * abs(a[2] - b[2])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("database")
    parser.add_argument("--only", default="", help="comma-separated ids to write (default: all)")
    parser.add_argument("--pack", type=float, default=12.0, help="yards: creatures this close are one pack")
    parser.add_argument("--merge", type=float, default=30.0, help="passed to make_dungeon.py")
    args = parser.parse_args()
    db = sqlite3.connect(args.database)
    only = {part for part in args.only.split(",") if part}
    here = os.path.dirname(os.path.abspath(__file__))

    print("# Dungeon routes for the dungeon run (M9). Format: src/dungeon_data.h, doc/dungeons.md.")
    print("#")
    print("# Made by tools/make_dungeons_from_db.py from the world database: the doors, the spawns and the bosses are real; the ORDER of each route is a")
    print("# nearest-next guess (see the tool). None of these routes has been walked on a sim. Check each one and edit the `go` lines that are wrong.")
    print("")
    for dungeon_id, (map_id, name, low, high, bosses) in DUNGEONS.items():
        if only and dungeon_id not in only:
            continue
        entrance, exit_ = door(db, name)
        if not entrance:
            print(f"{dungeon_id}: no 'Classic: {name} - Entrance' teleport target; skipped", file=sys.stderr)
            continue
        spawns = hostile_spawns(db, entrance[0])
        boss_rows = [s for s in spawns if s[1] in bosses]
        missing = [b for b in bosses if b not in {s[1] for s in boss_rows}]
        if missing:
            print(f"{dungeon_id}: bosses without a spawn on map {entrance[0]}: {', '.join(missing)}", file=sys.stderr)
        if not boss_rows:
            print(f"{dungeon_id}: no boss found; skipped", file=sys.stderr)
            continue

        # Packs: single-linkage clusters of the other hostile spawns, each at its average position.
        others = [s for s in spawns if s[1] not in bosses]
        clusters, seen = [], [False] * len(others)
        for i in range(len(others)):
            if seen[i]:
                continue
            members, stack = [], [i]
            seen[i] = True
            while stack:
                k = stack.pop()
                members.append(k)
                for j in range(len(others)):
                    if not seen[j] and math.hypot(others[k][2] - others[j][2], others[k][3] - others[j][3]) <= args.pack and abs(others[k][4] - others[j][4]) <= 8:
                        seen[j] = True
                        stack.append(j)
            xs = [others[k][2] for k in members]; ys = [others[k][3] for k in members]; zs = [others[k][4] for k in members]
            clusters.append((others[members[0]][0], sum(xs) / len(xs), sum(ys) / len(ys), sum(zs) / len(zs), others[members[0]][1], "pack"))
        points = clusters + [(s[0], s[2], s[3], s[4], s[1], "boss") for s in boss_rows]

        # The route: from the entrance, always the nearest point not visited yet.
        position = (entrance[1], entrance[2], entrance[3])
        ordered = []
        while points:
            nearest = min(points, key=lambda p: cost(position, (p[1], p[2], p[3])))
            points.remove(nearest)
            ordered.append(nearest)
            position = (nearest[1], nearest[2], nearest[3])

        tsv = f"{os.environ.get('TMPDIR', '/tmp')}/dungeon_{dungeon_id}.tsv"
        with open(tsv, "w", encoding="utf-8") as out:
            for p in ordered:
                out.write(f"{p[0]}\t{p[1]:.1f}\t{p[2]:.1f}\t{p[3]:.1f}\t{p[4]}\t{p[5]}\n")
        command = [sys.executable, "-I", os.path.join(here, "make_dungeon.py"), "--id", dungeon_id, "--name", name, "--levels", str(low), str(high),
                   "--roster", ROSTER, "--entry", str(entrance[0]), f"{entrance[1]:.2f}", f"{entrance[2]:.2f}", f"{entrance[3]:.2f}",
                   "--staging", *STAGING, "--merge", str(args.merge)]
        if exit_:
            command += ["--portal", str(exit_[0]), f"{exit_[1]:.1f}", f"{exit_[2]:.1f}", f"{exit_[3]:.1f}"]
        command.append(tsv)
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"{dungeon_id}: make_dungeon.py failed: {result.stderr.strip()}", file=sys.stderr)
            continue
        sys.stdout.write(result.stdout)
        print(f"{dungeon_id}: {len(boss_rows)} bosses, {len(clusters)} packs, {result.stdout.count(chr(10) + 'go ')} waypoints", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
