#!/usr/bin/python3 -I
"""Turn the old text files (data/dungeons.txt, data/dungeon_events.txt) into the per-dungeon JSON files (data/dungeons/<map id>-<name>.json, schema 1).

    python3 -I tools/convert_dungeons.py                       # reads data/dungeons.txt and data/dungeon_events.txt, writes data/dungeons/
    python3 -I tools/convert_dungeons.py --dungeons FILE --events FILE --out DIR [--map ID ...] [--check] [--force]

The conversion is lossless: numbers are copied as the characters they were written with (so the module reads exactly the same floats), names and
orders are kept, and the events of a map go into that map's file. tests/dungeon_files_test.cpp loads the result and compares it with what the module
reads from the old files. A dungeon file is written once by this script and edited by hand afterwards; do not run it over files you changed (it asks
for --force to replace an existing file). --check only prints what it would write.

The bosses[] block of each file lists the route's boss points (entry and name) with an empty strategy: fill it in by hand (doc/dungeon-strategy.md).
"""
import argparse
import os
import re
import sys

NUMBER = re.compile(r"^-?(0|[1-9][0-9]*)(\.[0-9]+)?$")

# File names are <map id>-<slug>.json. The slug is for people; the module reads the map id.
SLUGS = {
    33: "shadowfang-keep",
    34: "the-stockade",
    36: "the-deadmines",
    43: "wailing-caverns",
    47: "razorfen-kraul",
    48: "blackfathom-deeps",
    90: "gnomeregan",
    129: "razorfen-downs",
    389: "ragefire-chasm",
}


class Raw(str):
    """A number written exactly as in the source."""


def fail(message):
    sys.exit("convert_dungeons: " + message)


def number(token, where):
    if not NUMBER.match(token):
        fail(f"{where}: '{token}' is not a plain number")
    return Raw(token)


def strip(line):
    hash_at = line.find("#")
    if hash_at != -1:
        line = line[:hash_at]
    return line.strip()


def parse_dungeons(text):
    dungeons, current = [], None
    for number_, raw in enumerate(text.splitlines(), 1):
        line = strip(raw)
        if not line:
            continue
        where = f"dungeons line {number_}"
        words = line.split(None, 1)
        key = words[0]
        rest = words[1] if len(words) > 1 else ""
        args = rest.split()
        if key == "dungeon":
            if current is not None:
                fail(f"{where}: 'dungeon' inside another dungeon")
            current = {"id": args[0], "route": [], "bosses": []}
        elif current is None:
            fail(f"{where}: '{key}' outside a dungeon")
        elif key == "end":
            dungeons.append(current)
            current = None
        elif key == "name":
            current["name"] = " ".join(args)
        elif key == "levels":
            current["levels"] = [number(args[0], where), number(args[1], where)]
        elif key == "roster":
            current["roster"] = args[0]
        elif key in ("staging", "portal", "entry"):
            current[key] = {"map": number(args[0], where), "x": number(args[1], where), "y": number(args[2], where), "z": number(args[3], where)}
        elif key in ("duration", "wipe_recovery", "min_alive", "leader_dead_abandon"):
            current[key] = number(args[0], where)
        elif key == "go":
            if len(args) != 3:
                fail(f"{where}: 'go' needs x y z")
            current["route"].append({"x": number(args[0], where), "y": number(args[1], where), "z": number(args[2], where)})
        elif key == "boss":
            entry = number(args[0], where)
            name = " ".join(args[4:])
            current["route"].append({"x": number(args[1], where), "y": number(args[2], where), "z": number(args[3], where), "boss": entry})
            # a boss can stand on several route points (the same entry again): the boss block lists it once
            known = [b for b in current["bosses"] if b["entry"] == entry]
            if not known:
                current["bosses"].append({"entry": entry, "name": name})
            elif known[0]["name"] != name:
                fail(f"{where}: boss {entry} is called '{name}' here and '{known[0]['name']}' before")
        else:
            fail(f"{where}: unknown key '{key}'")
    if current is not None:
        fail("dungeons: the last dungeon has no 'end'")
    return dungeons


def parse_events(text):
    events, current = [], None
    for number_, raw in enumerate(text.splitlines(), 1):
        line = strip(raw)
        if not line:
            continue
        where = f"events line {number_}"
        w = line.split()
        if w[0] == "event":
            if current is not None:
                fail(f"{where}: 'event' inside an event")
            current = {"map": int(w[1]), "id": w[2], "name": " ".join(w[3:]), "steps": []}
        elif current is None:
            fail(f"{where}: '{w[0]}' outside an event")
        elif w[0] == "end":
            events.append(current)
            current = None
        elif w[0] == "at_start":
            current["trigger"] = {"type": "at_start"}
        elif w[0] == "after_boss":
            current["trigger"] = {"type": "after_boss", "boss": number(w[1], where)}
        elif w[0] == "near":
            current["trigger"] = {"type": "near", "x": number(w[1], where), "y": number(w[2], where), "z": number(w[3], where), "radius": number(w[4], where)}
        elif w[0] == "use":
            step = {"type": "use", "object": number(w[1], where), "x": number(w[2], where), "y": number(w[3], where), "z": number(w[4], where)}
            if len(w) == 7 and w[5] == "item":
                step["item"] = number(w[6], where)
            elif len(w) != 5:
                fail(f"{where}: use needs <object> <x> <y> <z> [item <entry>]")
            current["steps"].append(step)
        elif w[0] == "wait":
            current["steps"].append({"type": "wait", "seconds": number(w[1], where)})
        elif w[0] == "timeout":
            current["timeout_sec"] = number(w[1], where)
        else:
            fail(f"{where}: unknown line '{w[0]}'")
    if current is not None:
        fail("events: the last event has no 'end'")
    return events


def scalar(value):
    if isinstance(value, Raw):
        return str(value)
    if isinstance(value, str):
        out = ['"']
        for c in value:
            if c == '"':
                out.append('\\"')
            elif c == "\\":
                out.append("\\\\")
            elif ord(c) < 0x20:
                out.append("\\u%04x" % ord(c))
            else:
                out.append(c)
        out.append('"')
        return "".join(out)
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value)


def inline(value):
    if isinstance(value, (dict, list)) and not value:
        return "{}" if isinstance(value, dict) else "[]"
    if isinstance(value, dict):
        return "{ " + ", ".join(f'"{k}": {inline(v)}' for k, v in value.items()) + " }"
    if isinstance(value, list):
        return "[" + ", ".join(inline(v) for v in value) + "]"
    return scalar(value)


def is_scalar(value):
    return not isinstance(value, (dict, list))


def is_flat(value):
    """A value that reads well on one line: a scalar, an empty container, or a container of scalars."""
    if isinstance(value, dict):
        return all(is_scalar(v) for v in value.values())
    if isinstance(value, list):
        return all(is_scalar(v) for v in value)
    return True


def render(value, depth=0):
    pad = "  " * depth
    if is_scalar(value):
        return scalar(value)
    if is_flat(value):
        return inline(value)
    if isinstance(value, dict):
        parts = [f'{pad}  "{k}": {render(v, depth + 1)}' for k, v in value.items()]
        return "{\n" + ",\n".join(parts) + "\n" + pad + "}"
    return "[\n" + ",\n".join(pad + "  " + render(v, depth + 1) for v in value) + "\n" + pad + "]"


def build(dungeon, events):
    entry_map = int(dungeon["entry"]["map"])
    out = {"schema": Raw("1"), "id": dungeon["id"], "map": Raw(str(entry_map)), "name": dungeon.get("name", "")}
    if "levels" in dungeon:
        out["levels"] = dungeon["levels"]
    out["roster"] = dungeon["roster"]
    entrance = {}
    for key in ("staging", "portal", "entry"):
        if key in dungeon:
            entrance[key] = dungeon[key]
    out["entrance"] = entrance
    run = {}
    for old, new in (("duration", "duration_sec"), ("wipe_recovery", "wipe_recovery_sec"), ("min_alive", "min_alive"), ("leader_dead_abandon", "leader_dead_abandon_sec")):
        if old in dungeon:
            run[new] = dungeon[old]
    if run:
        out["run"] = run
    out["route"] = dungeon["route"]
    if dungeon["bosses"]:
        out["bosses"] = [{"entry": b["entry"], "name": b["name"], "strategy": {}} for b in dungeon["bosses"]]
    mine = []
    for e in events:
        if e["map"] != entry_map:
            continue
        item = {"id": e["id"], "name": e["name"], "trigger": e["trigger"], "steps": e["steps"]}
        if "timeout_sec" in e:
            item["timeout_sec"] = e["timeout_sec"]
        mine.append(item)
    if mine:
        out["events"] = mine
    return out


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dungeons", default=os.path.join(here, "data", "dungeons.txt"))
    ap.add_argument("--events", default=os.path.join(here, "data", "dungeon_events.txt"))
    ap.add_argument("--out", default=os.path.join(here, "data", "dungeons"))
    ap.add_argument("--map", type=int, action="append", help="convert only the dungeon of this map id (repeat for several)")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    with open(args.dungeons, encoding="utf-8") as f:
        dungeons = parse_dungeons(f.read())
    with open(args.events, encoding="utf-8") as f:
        events = parse_events(f.read())
    used_events = 0
    wanted_events = len(events)
    os.makedirs(args.out, exist_ok=True) if not args.check else None
    for dungeon in dungeons:
        data = build(dungeon, events)
        map_id = int(data["map"])
        if args.map and map_id not in args.map:
            continue
        used_events += len(data.get("events", []))
        slug = SLUGS.get(map_id) or re.sub(r"[^a-z0-9]+", "-", dungeon.get("name", dungeon["id"]).lower()).strip("-")
        path = os.path.join(args.out, f"{map_id}-{slug}.json")
        text = render(data) + "\n"
        if args.check:
            print(f"would write {path} ({len(text)} bytes)")
            continue
        if os.path.exists(path) and not args.force:
            fail(f"{path} exists; use --force to replace it")
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        print(f"wrote {path}")
    if not args.map and used_events != wanted_events:
        orphan = [e["id"] for e in events if e["map"] not in {int(d["entry"]["map"]) for d in dungeons}]
        print(f"note: {len(events) - used_events} event(s) are for maps with no dungeon block ({', '.join(orphan)}); they stay in the old events file", file=sys.stderr)


if __name__ == "__main__":
    main()
