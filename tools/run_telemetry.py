#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reads the run telemetry of the forever-bots scenarios and prints, per party: the time per step, the route point reached over time (5 minute buckets), the stalls
with their cause, and the boss attempts.

Input: run-telemetry.log (the file written when Module.forever-bots.RunTelemetryDir is set) or a module journal/log: any line that holds "party_tl:", "party_stalled:"
or "boss_attempt:" is read, whatever stands in front of it ("2026-11-12T10:00:00Z ", "forever-bots: event ").

    python3 tools/run_telemetry.py run-telemetry.log
    journalctl -u forever-sim | python3 tools/run_telemetry.py -
    python3 tools/run_telemetry.py run-telemetry.log --party dm#2

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
    for boss, b in open_attempts.items():
        out.append(f"   boss       {boss.replace('_', ' ')} t={b['t_s']}s started, no end line (run ended or the log stops)")
    return "\n".join(out)


def main(argv=None):
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
