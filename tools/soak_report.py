#!/usr/bin/env python3
"""Summarises a long run from the module's telemetry CSV and, optionally, the module's stderr log (ws-err.txt) and the core's Server.log.

    soak_report.py --telemetry forever-bots-telemetry.csv [--stderr ws-err.txt] [--server-log Server.log] [--hours 8]

Telemetry: the CSV of src/bot_telemetry.cpp (header: time_ms,kind,bot,level,health_pct,power_pct,in_fight,fights,targets_gone,deaths,casts_applied,
refusals,stuck_episodes,longest_fight_ms,health_lost,detail). Every line carries the bot's running totals, so the last line of a bot tells its story. Rotated
files (name.1, name.2 ...) are read too when they exist. Output is Markdown on stdout.
"""
import argparse
import csv
import glob
import os
import re
import sys
from collections import Counter, defaultdict


def telemetry_files(path):
    """The telemetry file and its rotations, oldest first: files archived by name with a timestamp, then the module's own rotations
    (.N, the highest number is the oldest), then the live file."""
    siblings = [f for f in glob.glob(path + ".[0-9]*") if os.path.exists(f)]
    suffix = lambda f: f.rsplit(".", 1)[1]
    numbered = sorted((f for f in siblings if suffix(f).isdigit()), key=lambda f: -int(suffix(f)))
    named = sorted(f for f in siblings if not suffix(f).isdigit())
    return [f for f in named + numbered + [path] if os.path.exists(f)]


# time_ms is the module's monotonic clock (time since the machine booted): it starts again from a small value after a reboot, so rows of
# different runs must not be sorted together. A step back of more than this ends a run.
RUN_BREAK_MS = 5000
# The module writes a summary line per bot every minute: no row for this long means it was not running, and the time is left out too.
GAP_BREAK_MS = 5 * 60 * 1000


def read_telemetry(path):
    """The rows in the order they were written. Each gets "run" (0, 1, ...) and "active_ms": the time since the first row, with the gaps between
    runs (the machine was off, the module was down) left out."""
    rows = []
    for name in telemetry_files(path):
        with open(name, newline="", encoding="utf-8", errors="replace") as handle:
            for row in csv.DictReader(handle):
                if row.get("kind") and row.get("time_ms", "").isdigit():
                    rows.append(row)
    run, run_start, offset, previous = 0, None, 0, None
    for row in rows:
        now = int(row["time_ms"])
        if previous is None:
            run_start = previous = now
        elif now < previous - RUN_BREAK_MS or now - previous > GAP_BREAK_MS:
            offset += previous - run_start
            run += 1
            run_start = previous = now
        else:
            previous = max(previous, now)
        row["run"] = run
        row["active_ms"] = offset + now - run_start
    return rows


def as_int(value, default=0):
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return default


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--telemetry", required=True)
    parser.add_argument("--stderr")
    parser.add_argument("--server-log")
    parser.add_argument("--hours", type=float, help="run length in hours (default: from the telemetry)")
    args = parser.parse_args()

    rows = read_telemetry(args.telemetry)
    if not rows:
        sys.exit("no telemetry rows found")
    runs = max(int(r["run"]) for r in rows) + 1
    active_ms = max(int(r["active_ms"]) for r in rows)
    hours = args.hours or max(active_ms / 3600000.0, 1e-9)

    # The counters in the rows are running totals since the module started, so they start again at a restart (the bots stay, with their levels).
    # A bot's total is the sum of the last value of each stretch between resets.
    counters = ("fights", "targets_gone", "deaths", "casts_applied", "refusals", "stuck_episodes")
    banked = defaultdict(Counter)      # bot -> counter -> total of the stretches already ended
    seen = defaultdict(Counter)        # bot -> counter -> last value
    longest = Counter()
    last = {}
    first_level = {}
    kinds = Counter()
    level_events_by_hour = Counter()
    deaths_by_hour = Counter()
    for row in rows:
        bot = row["bot"]
        kinds[row["kind"]] += 1
        last[bot] = row
        for key in counters:
            value = as_int(row[key])
            if value < seen[bot][key]:
                banked[bot][key] += seen[bot][key]
            seen[bot][key] = value
        longest[bot] = max(longest[bot], as_int(row["longest_fight_ms"]))
        first_level.setdefault(bot, as_int(row["level"], 1))
        hour = int(row["active_ms"]) // 3600000
        if row["kind"] == "level":
            level_events_by_hour[hour] += 1
        if row["kind"] == "death":
            deaths_by_hour[hour] += 1

    levels = Counter(as_int(r["level"], 1) for r in last.values())
    brackets = Counter((l - 1) // 10 * 10 + 1 for l in levels.elements())
    bot_total = lambda bot, key: banked[bot][key] + seen[bot][key]
    total = lambda key: sum(bot_total(b, key) for b in last)

    print(f"# Soak report\n")
    print(f"- Telemetry: {len(rows)} rows, {len(last)} bots, {hours:.2f} h of runs ({active_ms / 1000:.0f} s)")
    if runs > 1:
        print(f"- {runs} separate runs (a stretch with no rows or a restart of the clock; the time between runs is left out):")
        for number in range(runs):
            part = [r for r in rows if int(r["run"]) == number]
            span = (int(part[-1]["active_ms"]) - int(part[0]["active_ms"])) / 3600000.0
            bots = len({r["bot"] for r in part})
            levels_in_run = sum(1 for r in part if r["kind"] == "level")
            how = ""
            if number > 0:
                before = [r for r in rows if int(r["run"]) == number - 1][-1]
                step = int(part[0]["time_ms"]) - int(before["time_ms"])
                how = f", after a {step / 60000:.0f} min stretch with no rows" if step > 0 else ", after a restart of the machine (the clock started again)"
            print(f"  - run {number + 1}: {span:.2f} h, {bots} bots, {levels_in_run} level-ups{how}")
    print(f"- Bots by line kind: " + ", ".join(f"{k} {n}" for k, n in sorted(kinds.items())))
    print(f"- Fights {total('fights')}, targets gone {total('targets_gone')}, casts applied {total('casts_applied')}, refusals {total('refusals')}")
    print(f"- Deaths {total('deaths')} ({total('deaths') / hours:.1f} per hour), stuck episodes {total('stuck_episodes')}, longest fight {max(longest.values(), default=0) / 1000:.0f} s")
    gained = sum(max(0, as_int(r['level'], 1) - first_level[b]) for b, r in last.items())
    print(f"- Levels gained {gained} ({gained / hours:.1f} per hour; {gained / max(len(last), 1):.1f} per bot)")

    print("\n## Final levels")
    print("| level bracket | bots |\n|---|---|")
    for low in sorted(brackets):
        print(f"| {low}-{low + 9} | {brackets[low]} |")
    print("\n| level | bots |\n|---|---|")
    for level in sorted(levels):
        print(f"| {level} | {levels[level]} |")

    print("\n## Per hour")
    print("| hour | level-ups | deaths |\n|---|---|---|")
    for hour in range(int(hours) + 1):
        print(f"| {hour} | {level_events_by_hour[hour]} | {deaths_by_hour[hour]} |")

    flat = [b for b in last if bot_total(b, "stuck_episodes") > 0]
    if flat:
        worst = sorted(flat, key=lambda b: -bot_total(b, "stuck_episodes"))[:10]
        print("\n## Most stuck bots")
        print("| bot | level | stuck episodes | fights |\n|---|---|---|---|")
        for b in worst:
            print(f"| {b} | {last[b]['level']} | {bot_total(b, 'stuck_episodes')} | {bot_total(b, 'fights')} |")

    if args.stderr and os.path.exists(args.stderr):
        events = Counter()
        refused = Counter()
        pattern = re.compile(r"forever-bots: event ([a-z_ ]+?):")
        with open(args.stderr, encoding="utf-8", errors="replace") as handle:
            for line in handle:
                m = pattern.search(line)
                if m:
                    events[m.group(1)] += 1
                if "death_action_refused" in line or "rez_refused" in line:
                    refused[line.split("forever-bots:")[-1].strip()[:120]] += 1
        print("\n## Module events (stderr)")
        if events:
            print("| event | count |\n|---|---|")
            for name, n in events.most_common():
                print(f"| {name} | {n} |")
        else:
            print("(none)")
        if refused:
            print("\nDeath and rez refusals (first 10 distinct):")
            for text, n in refused.most_common(10):
                print(f"- {n} x {text}")

    if args.server_log and os.path.exists(args.server_log):
        reasons = Counter()
        cast_results = Counter()
        cast_spells = Counter()
        gear_failed = 0
        gear_lines = 0
        summary = re.compile(r"^.*Modules:\s+(\d+) x type (\d+) status (\d+): (.*)$")
        gear = re.compile(r"autogear .*summary .*failed=(\d+)")
        with open(args.server_log, encoding="utf-8", errors="replace") as handle:
            for line in handle:
                m = summary.match(line.rstrip("\n"))
                if m:
                    reasons[re.sub(r"\d+", "N", m.group(4))] += int(m.group(1))
                    cast = re.search(r"cast of spell (\d+).*?result\D*(\d+)", m.group(4), re.I)
                    if cast:
                        cast_results[cast.group(2)] += int(m.group(1))
                        cast_spells[(cast.group(1), cast.group(2))] += int(m.group(1))
                g = gear.search(line)
                if g:
                    gear_lines += 1
                    gear_failed += 1 if int(g.group(1)) > 0 else 0
        print("\n## Core log")
        print(f"- autogear summaries {gear_lines}, with failed picks {gear_failed}")
        if reasons:
            print("\nTop refusal reasons (sum of the once-a-minute summaries):")
            for text, n in reasons.most_common(12):
                print(f"- {n} x {text}")
        if cast_results:
            print("\nCast refusals by result code (SpellCastResult values):")
            for code, n in cast_results.most_common(10):
                print(f"- result {code}: {n}")
            print("\nCast refusals by spell and result (top 15):")
            for (spell, code), n in cast_spells.most_common(15):
                print(f"- spell {spell} result {code}: {n}")


if __name__ == "__main__":
    main()
