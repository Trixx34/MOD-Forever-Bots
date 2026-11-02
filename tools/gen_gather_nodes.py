#!/usr/bin/python3 -I
# SPDX-License-Identifier: GPL-2.0-or-later
"""Make data/gather_nodes.txt: the herb, ore, skinning and fishing nodes of the continents, for the bots' gather walk (src/gather_nodes.h).

    mysql forever_test_world -N -B -e "select g.map,round(g.position_x,1),round(g.position_y,1),round(g.position_z,1),t.Data0,t.entry
        from gameobject g join gameobject_template t on t.entry=g.id
        where t.type=3 and g.map in (0,1)" > nodes.tsv
    python3 -I tools/gen_gather_nodes.py --nodes nodes.tsv --out data/gather_nodes.txt

A gathering node is a gameobject of type 3 (GAMEOBJECT_TYPE_CHEST): the herb and ore nodes are chests whose lock (Data0) names the skill that opens it
(LockType/skill). The mapping from lock id to skill is the table in LOCK_SKILL below; a lock with no skill is left out. Only the nodes of the two
continents are kept (the walk stays on the bot's own map).
"""
import argparse

# Lock id -> the gathering skill line that opens it (Herbalism 182, Mining 186, Skinning 393). UNVERIFIED: these lock ids were not checked against
# lock.dbc of this server; check them against the DB before the first generation.
LOCK_SKILL = {2: 186, 3: 186, 16: 182, 18: 182, 20: 393}


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--nodes", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    rows = []
    for line in open(args.nodes, encoding="utf-8"):
        f = line.rstrip("\n").split("\t")
        if len(f) < 6:
            continue
        m, x, y, z, lock, entry = int(f[0]), float(f[1]), float(f[2]), float(f[3]), int(f[4]), int(f[5])
        skill = LOCK_SKILL.get(lock)
        if skill is None:
            continue
        rows.append((skill, m, x, y, z, entry))

    rows.sort()
    with open(args.out, "w", encoding="utf-8") as f:
        f.write("# Gathering nodes of the continents (src/gather_nodes.h). One per line: skill map x y z entry\n")
        f.write("# skill: 182 Herbalism, 186 Mining, 393 Skinning. Made by tools/gen_gather_nodes.py from the scratch world DB (read only).\n")
        for skill, m, x, y, z, entry in rows:
            f.write(f"{skill} {m} {x:g} {y:g} {z:g} {entry}\n")
    print(f"{len(rows)} nodes -> {args.out}")


if __name__ == "__main__":
    main()
