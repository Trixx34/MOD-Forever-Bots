#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reads the run telemetry of the forever-bots scenarios and prints, per party: the time per step, the route point reached over time (5 minute buckets), the stalls
with their cause, and the boss attempts.

Input: run-telemetry.log (the file written when Module.forever-bots.RunTelemetryDir is set) or a module journal/log: any line that holds "party_tl:", "party_stalled:"
or "boss_attempt:" is read, whatever stands in front of it ("2026-11-12T10:00:00Z ", "forever-bots: event ").

    python3 tools/run_telemetry.py run-telemetry.log
    journalctl -u forever-sim | python3 tools/run_telemetry.py -
    python3 tools/run_telemetry.py run-telemetry.log --party dm#2
    journalctl -u forever-sim | python3 tools/run_telemetry.py deaths - [--spells spells.txt] [--top 30]

The `deaths` subcommand reads the died: lines (protocol 29, the core's damage events) and prints one row per (attacker entry, spell id): deaths, the summed damage and overkill of
the killing blows, and the average health percent the victims had before the blow. killer_src=guess rows (no event came: the module's old guess) are marked "guess" and have no
spell. The world DB is not available here, so spell names come from the optional --spells file (one "<id> <name>" or "<id>,<name>" per line; 0 is melee).

Each party_tl: line stands for the 30 s before it, so the time per step is the number of lines of a step times 30 s (the timeline is sampled, not exact).
Python 3, standard library only."""
import argparse
import re
import sys
from collections import Counter, OrderedDict

KINDS = ("party_tl", "party_stalled", "boss_attempt")
LINE = re.compile(r"(?:^|\s)(?P<kind>party_tl|party_stalled|boss_attempt): (?P<rest>.*)$")
BUCKET_S = 300
SAMPLE_S = 30


def parse_line(text):
    """-> (kind, {key: value}) or None. Values are strings; a field with an empty value (variant=) is kept as ''."""
    m = LINE.search(text)
    if not m:
        return None
    fields = OrderedDict()
    for part in m.group("rest").split():
        if "=" in part:
            key, value = part.split("=", 1)
            fields[key] = value
    if "scenario" not in fields or "t_s" not in fields:
        return None
    return m.group("kind"), fields


def party_key(fields):
    variant = fields.get("variant", "")
    return f"{fields['scenario']}#{fields.get('party', '1')}" + (f" [{variant}]" if variant else "")


def read(stream):
    parties = OrderedDict()
    for text in stream:
        parsed = parse_line(text)
        if not parsed:
            continue
        kind, fields = parsed
        p = parties.setdefault(party_key(fields), {"tl": [], "stalls": [], "boss": []})
        p[{"party_tl": "tl", "party_stalled": "stalls", "boss_attempt": "boss"}[kind]].append(fields)
    return parties


def party_damage(fields):
    """The dmg_by_ fields of a party_tl or boss_attempt line as one short text, or ''."""
    return " ".join(f"{key}={fields[key]}" for key in ("dmg_by_spell", "dmg_by_attacker", "death_by_spell", "death_by_attacker") if key in fields)


def route_point(fields):
    try:
        return int(fields.get("route", "0/0").split("/")[0])
    except ValueError:
        return 0


def summarize(name, p):
    out = [f"== {name} =="]
    tl = p["tl"]
    if tl:
        last = tl[-1]
        out.append(f"   timeline   {len(tl)} samples, up to t={last['t_s']}s, route {last.get('route', '?')}, alive {last.get('alive', '?')}, map {last.get('map', '?')}")
        steps = Counter(f.get("step", "?") for f in tl)
        total = sum(steps.values()) * SAMPLE_S
        out.append("   time/step  " + "  ".join(f"{step}:{count * SAMPLE_S}s ({100 * count * SAMPLE_S // max(total, 1)}%)" for step, count in steps.most_common()))
        buckets = OrderedDict()
        for f in tl:
            b = int(f["t_s"]) // BUCKET_S
            buckets[b] = max(buckets.get(b, 0), route_point(f))
        out.append("   route/5min " + "  ".join(f"{b * BUCKET_S // 60}m:{point}" for b, point in buckets.items()))
        out.append(f"   last mark  {last.get('last', '?')}")
        spells = Counter()
        attackers = Counter()
        for f in tl:
            for key, target in (("dmg_by_spell", spells), ("dmg_by_attacker", attackers)):
                for item in f.get(key, "").split(","):
                    parts = item.split(":")
                    if len(parts) == 4 and parts[1].isdigit():
                        target[parts[0]] += int(parts[1])
        if spells:
            out.append("   dmg taken  by spell " + " ".join(f"{k}:{v}" for k, v in spells.most_common(5)) + "  by attacker " + " ".join(f"{k}:{v}" for k, v in attackers.most_common(5)) +
                       "  (big hits only; 0 = melee)")
    else:
        out.append("   timeline   (no party_tl lines)")
    if p["stalls"]:
        for s in p["stalls"]:
            out.append(f"   STALL      t={s['t_s']}s cause={s.get('cause', '?')} no_progress={s.get('no_progress_s', '?')}s pulls={s.get('pulls', '?')} "
                       f"refusals={s.get('route_refusals', '?')} rest={s.get('rest_s', '?')}s dead={s.get('dead_s', '?')}s fight={s.get('fight_s', '?')}s "
                       f"route={s.get('route', '?')} last={s.get('last', '?')}")
    else:
        out.append("   stalls     none")
    open_attempts = {}
    for b in p["boss"]:
        boss = b.get("boss", "?")
        if b.get("stage") == "start":
            open_attempts[boss] = b
            continue
        start = open_attempts.pop(boss, None)
        began = f"t={start['t_s']}s" if start else "t=?"
        out.append(f"   boss       {boss.replace('_', ' ')} {began} dur={b.get('dur_s', '?')}s deaths={b.get('deaths', '?')} outcome={b.get('outcome', '?')}")
        if party_damage(b):
            out.append(f"   boss dmg   {party_damage(b)}   (spell:damage:hits:deaths, attacker entry:damage:hits:deaths, death_by: key:deaths:overkill)")
    for boss, b in open_attempts.items():
        out.append(f"   boss       {boss.replace('_', ' ')} t={b['t_s']}s started, no end line (run ended or the log stops)")
    return "\n".join(out)


DIED = re.compile(r"(?:^|\s)died: (?P<rest>.*)$")


def parse_died(text):
    """-> {key: value} of a died: line, or None."""
    m = DIED.search(text)
    if not m:
        return None
    fields = {}
    for part in m.group("rest").split():
        if "=" in part:
            key, value = part.split("=", 1)
            fields[key] = value
    return fields if "bot" in fields else None


def load_spells(path):
    names = {}
    with open(path, errors="replace") as handle:
        for text in handle:
            text = text.strip()
            if not text or text.startswith("#"):
                continue
            parts = re.split(r"[,\t ]+", text, maxsplit=1)
            if len(parts) == 2 and parts[0].isdigit():
                names[parts[0]] = parts[1].strip().strip('"')
    return names


def number(value):
    return int(value) if value is not None and value.isdigit() else None


def death_rows(stream):
    """-> {(entry, spell): {deaths, damage, overkill, pct_sum, pct_n, guess}} from the died: lines."""
    rows = OrderedDict()
    for text in stream:
        fields = parse_died(text)
        if fields is None:
            continue
        guess = fields.get("killer_src", "guess") != "event"
        entry = fields.get("killer_entry", "none")
        spell = "?" if guess else fields.get("killer_spell", "?")
        row = rows.setdefault((entry, spell), {"deaths": 0, "damage": 0, "overkill": 0, "pct_sum": 0, "pct_n": 0, "guess": guess})
        row["deaths"] += 1
        row["damage"] += number(fields.get("hit_amount")) or 0
        row["overkill"] += number(fields.get("overkill")) or 0
        pct = number(fields.get("hit_hp_pct"))
        if pct is not None:
            row["pct_sum"] += pct
            row["pct_n"] += 1
    return rows


def deaths_table(rows, names, top):
    header = ("attacker", "spell", "spell name", "deaths", "damage", "overkill", "avg hp%")
    lines = []
    for (entry, spell), row in sorted(rows.items(), key=lambda item: (-item[1]["deaths"], -item[1]["damage"], item[0]))[:top]:
        if row["guess"]:
            name = "guess (no damage event)"
        elif spell == "0":
            name = names.get("0", "melee")
        else:
            name = names.get(spell, "")
        avg = f"{row['pct_sum'] / row['pct_n']:.0f}" if row["pct_n"] else "-"
        lines.append((entry, spell, name, str(row["deaths"]), str(row["damage"]), str(row["overkill"]), avg))
    widths = [max(len(header[i]), *(len(line[i]) for line in lines)) if lines else len(header[i]) for i in range(len(header))]
    out = ["  ".join(cell.ljust(widths[i]) if i in (2,) else cell.rjust(widths[i]) for i, cell in enumerate(header))]
    for line in lines:
        out.append("  ".join(cell.ljust(widths[i]) if i == 2 else cell.rjust(widths[i]) for i, cell in enumerate(line)))
    return "\n".join(out)


def deaths_main(argv):
    ap = argparse.ArgumentParser(prog="run_telemetry.py deaths", description="table of the died: lines by attacker and spell")
    ap.add_argument("file", help="a journal or log file with died: lines; - for stdin")
    ap.add_argument("--spells", help="optional file of spell names, one '<id> <name>' or '<id>,<name>' per line")
    ap.add_argument("--top", type=int, default=30, help="rows to print (default 30)")
    args = ap.parse_args(argv)
    names = load_spells(args.spells) if args.spells else {}
    if args.file == "-":
        rows = death_rows(sys.stdin)
    else:
        with open(args.file, errors="replace") as handle:
            rows = death_rows(handle)
    if not rows:
        print("no died: lines found")
        return 1
    total = sum(row["deaths"] for row in rows.values())
    guessed = sum(row["deaths"] for row in rows.values() if row["guess"])
    print(deaths_table(rows, names, args.top))
    print(f"{total} deaths, {guessed} without a damage event (guess)")
    return 0


def main(argv=None):
    if argv is None:
        argv = sys.argv[1:]
    if argv and argv[0] == "deaths":
        return deaths_main(argv[1:])
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("file", help="run-telemetry.log or a journal file; - for stdin")
    ap.add_argument("--party", help="only this party, as dm#2")
    args = ap.parse_args(argv)
    if args.file == "-":
        parties = read(sys.stdin)
    else:
        with open(args.file, errors="replace") as handle:
            parties = read(handle)
    shown = 0
    for name, p in parties.items():
        if args.party and not name.startswith(args.party):
            continue
        print(summarize(name, p))
        shown += 1
    if not shown:
        print("no party_tl, party_stalled or boss_attempt lines found")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
