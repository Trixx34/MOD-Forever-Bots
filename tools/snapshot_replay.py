#!/usr/bin/env python3
"""Replays a telemetry run and answers the open findings of the sim handoff, one section per finding.

    snapshot_replay.py --telemetry forever-bots-telemetry.csv [--stderr ws-err.txt] [--hours 5]

The CSV is src/bot_telemetry.cpp's output (header: time_ms,kind,bot,level,health_pct,power_pct,in_fight,fights,targets_gone,deaths,
casts_applied,refusals,stuck_episodes,longest_fight_ms,health_lost,detail). Every line carries a bot's running totals, so the last
line of a bot tells its whole story.

Rows are grouped into runs the same way soak_report.py does (the clock steps back at a reboot, and a five-minute hole with no row
splits two stretches), because the report from a mixed chain of runs is what makes these findings hard to read.

Output is Markdown on stdout, one section per open question:
  F1 leveling  - bots still on level 1 at the end, and their last quest step (needs --stderr for the quest_state lines)
  F2 deaths    - deaths per level, and the level of the bots that died most
  F3 stuck     - stuck episodes against fights, so a route problem and a combat loop can be told apart
  F4 fights    - the longest fight, and every fight_start / target_switch / fight_end around it
  F5 refusals  - applied casts against refused ones, grouped by status (needs --stderr)
  F6 targets   - fights that ended with the target gone, split by level and map
"""
import argparse
import csv
import glob
import os
import re
import sys
from collections import Counter, defaultdict

# time_ms is the monotonic clock, which starts again after a reboot: rows of different runs must not be sorted together.
RUN_BREAK_MS = 5000
GAP_BREAK_MS = 5 * 60 * 1000
# A fight is called stalled by the policy after kFightStallMs (bot_policy.cpp); anything far beyond it is the bug of F4.
FIGHT_STALL_MS = 45000
# Rows shown around the longest fight: a fight of this length needs this much context to be read.
FIGHT_CONTEXT_MS = 180000


def telemetry_files(path):
    """The telemetry file and its rotations, oldest first: named archives, then the module's own .N rotations, then the live file."""
    siblings = [f for f in glob.glob(path + ".[0-9]*") if os.path.exists(f)]
    suffix = lambda f: f.rsplit(".", 1)[1]
    numbered = sorted((f for f in siblings if suffix(f).isdigit()), key=lambda f: -int(suffix(f)))
    named = sorted(f for f in siblings if not suffix(f).isdigit())
    return [f for f in named + numbered + [path] if os.path.exists(f)]


def read_telemetry(path):
    """The rows in the order they were written, each with "run" and "active_ms" (time since the run's first row, gaps left out)."""
    rows = []
    for name in telemetry_files(path):
        with open(name, newline="", encoding="utf-8", errors="replace") as handle:
            for row in csv.DictReader(handle):
                if row.get("kind") and row.get("time_ms", "").isdigit():
                    rows.append(row)
    if not rows:
        return []
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


def detail_field(row, name):
    """One "name value" pair out of a row's detail column, or None."""
    for match in re.finditer(r"(\w+) ([\w.\-]+)", row.get("detail") or ""):
        if match.group(1) == name:
            return match.group(2)
    return None


# --- stderr: the quest_state and refusal lines the module writes ---

QUEST_STATE = re.compile(r"quest_state: bot=(\d+) .*?step=(\w+) quest=(\d+)")
NOT_DONE = re.compile(r"(\d+) ([\w ]+?) not done: ([\w ]+?)(?:\n|$)")


def read_stderr(path):
    """The last quest_state per bot and the refusal counts by action and status, from the module's stderr."""
    last_step, cast_not_done = {}, Counter()
    if not path or not os.path.exists(path):
        return last_step, cast_not_done
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            quest = QUEST_STATE.search(line)
            if quest:
                last_step[int(quest.group(1))] = (quest.group(2), int(quest.group(3)))
            else:
                done = NOT_DONE.search(line)
                if done:
                    cast_not_done[(done.group(2).strip(), done.group(3).strip())] += 1
    return last_step, cast_not_done


# --- one section per open finding ---


def section_leveling(rows, last_step, out):
    """F1: bots that are still on level 1 at the end, and what each of them was last told to do."""
    last_level, level_ups = {}, Counter()
    for row in rows:
        bot, level = int(row["bot"]), as_int(row["level"])
        last_level[bot] = level
        if row["kind"] == "level":
            level_ups[bot] += 1
    level_one = sorted(bot for bot, level in last_level.items() if level <= 1)
    out.append(f"## F1 leveling\n\n{len(level_one)} of {len(last_level)} bots end on level 1 or below; "
               f"{sum(level_ups.values())} level-ups in all.")
    if not level_one:
        return
    steps = Counter(last_step.get(bot, ("?", 0))[0] for bot in level_one)
    walked = [bot for bot in level_one if last_step.get(bot, ("?", 0))[0] == "walk_to"]
    out.append(f"Bots still on level 1 by their last quest step: "
               + ", ".join(f"{step} {n}" for step, n in steps.most_common()) + ".")
    out.append(f"{len(walked)} of them last sat in `walk_to`: they are the level-1 bots the 2000-yard cap, the no-route block "
               "and the dropped teleport are meant to move. Read their last `quest_state` line for the start and the quest.")
    for bot in walked[:10]:
        step, quest = last_step[bot]
        out.append(f"- bot {bot}: step={step} quest={quest}")


def section_deaths(rows, out):
    """F2: deaths per level, and the bots that died the most, so a cluster by level can be seen."""
    by_level, per_bot = Counter(), Counter()
    levels = {}
    for row in rows:
        bot, level = int(row["bot"]), as_int(row["level"])
        if level:
            levels[bot] = level
        if row["kind"] == "death":
            by_level[level] += 1
            per_bot[bot] += 1
    total = sum(by_level.values())
    out.append(f"## F2 deaths\n\n{total} deaths. By level: "
               + ", ".join(f"level {level}: {n}" for level, n in sorted(by_level.items())[:15]) + ".")
    out.append("Most deaths: "
               + ", ".join(f"bot {bot} {n} (level {levels.get(bot, '?')})" for bot, n in per_bot.most_common(8)) + ".")


def section_stuck(rows, out):
    """F3: stuck episodes against fights. Many episodes with few fights is a route problem; many with many fights is a combat loop."""
    last = {}
    for row in rows:
        last[int(row["bot"])] = row
    stuck = [(as_int(row["stuck_episodes"]), as_int(row["fights"]), int(row["bot"]), as_int(row["level"]))
             for row in last.values() if as_int(row["stuck_episodes"])]
    stuck.sort(reverse=True)
    out.append(f"## F3 stuck\n\n{len(stuck)} bots had at least one stuck episode; {sum(s[0] for s in stuck)} episodes in all.")
    out.append("Bot: episodes / fights (level)\n\nThe highest first, with the fights beside them:")
    for episodes, fights, bot, level in stuck[:12]:
        out.append(f"- bot {bot}: {episodes} / {fights} (level {level})")


def section_fights(rows, out):
    """F4: the longest fight and its rows. The watchdog gives up at kFightStallMs, so a far longer one is the bug."""
    longest, per_bot = 0, {}
    for row in rows:
        bot = int(row["bot"])
        per_bot[bot] = max(per_bot.get(bot, 0), as_int(row["longest_fight_ms"]))
        longest = max(longest, as_int(row["longest_fight_ms"]))
    out.append(f"## F4 fights\n\nThe longest fight is {longest} ms ({round(longest / 60000, 1)} minutes); "
               f"the policy gives a stalled one up after {FIGHT_STALL_MS} ms.")
    if longest <= FIGHT_STALL_MS * 2:
        out.append("Nothing beyond twice the watchdog: no fight to explain.")
        return
    out.append("Longest per bot: "
               + ", ".join(f"bot {bot} {ms}" for bot, ms in sorted(per_bot.items(), key=lambda e: -e[1])[:5]) + ".")
    # The rows of that bot around the end of that fight: how the health moved, and what the target did. longest_fight_ms only
    # reaches its final value on the fight_end row, so the window has to reach past it.
    worst = max(per_bot, key=per_bot.get)
    around = [row for row in rows
              if int(row["bot"]) == worst and abs(as_int(row["longest_fight_ms"]) - per_bot[worst]) <= 1]
    when = [as_int(row["active_ms"]) for row in around]
    window = rows if not when else [
        row for row in rows
        if int(row["bot"]) == worst and min(when) - FIGHT_CONTEXT_MS <= as_int(row["active_ms"]) <= min(when) + FIGHT_CONTEXT_MS]
    window.sort(key=lambda row: as_int(row["active_ms"]))
    out.append(f"Rows of bot {worst} around its longest fight:")
    for row in window:
        detail = (row.get("detail") or "").strip()
        out.append(f"- {as_int(row['active_ms']):>10} ms {row['kind']:<13} hp {row['health_pct']}% fights {row['fights']}"
                   + (f"  {detail}" if detail else ""))


def section_refusals(rows, cast_not_done, out):
    """F5: applied casts against refused ones, and what the core refused them for."""
    last = {}
    for row in rows:
        last[int(row["bot"])] = row
    applied = sum(as_int(row["casts_applied"]) for row in last.values())
    refused = sum(as_int(row["refusals"]) for row in last.values())
    out.append(f"## F5 refusals\n\n{applied} casts applied, {refused} actions answered otherwise "
               f"({round(100.0 * refused / max(applied + refused, 1), 1)}%).")
    if not cast_not_done:
        out.append("Pass --stderr to group the refusals by action and status; the CSV counts them without a reason.")
        return
    out.append("Refusals by action and status (from the module's stderr):")
    for (action, status), n in cast_not_done.most_common(12):
        out.append(f"- {action} -> {status}: {n}")


def section_targets(rows, out):
    """F6: how much of the fights ended with the target gone, split by level."""
    last, levels = {}, {}
    for row in rows:
        bot, level = int(row["bot"]), as_int(row["level"])
        last[bot] = row
        if level:
            levels[bot] = level
    fights = sum(as_int(row["fights"]) for row in last.values())
    gone = sum(as_int(row["targets_gone"]) for row in last.values())
    reasons = Counter()
    for row in rows:
        reason = detail_field(row, "reason")
        if row["kind"] == "fight_end" and reason:
            reasons[reason] += 1
    out.append(f"## F6 targets\n\n{fights} fights, {gone} ending with the target gone "
               f"({round(100.0 * gone / max(fights, 1), 1)}%).")
    out.append("`target_gone` also counts a kill: a creature that died is not in view when the threat drops.")
    if reasons:
        out.append("Fights ended by: " + ", ".join(f"{reason} {n}" for reason, n in reasons.most_common()) + ".")
    by_level = defaultdict(lambda: [0, 0])
    for row in last.values():
        level = levels.get(int(row["bot"]), 0)
        by_level[level][0] += as_int(row["fights"])
        by_level[level][1] += as_int(row["targets_gone"])
    out.append("By level (fights / targets gone):")
    for level in sorted(by_level):
        fights, gone = by_level[level]
        out.append(f"- level {level}: {fights} / {gone}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--telemetry", required=True)
    parser.add_argument("--stderr", help="the module's stderr, for quest_state and refusal lines")
    parser.add_argument("--hours", type=float, help="run length in hours (default: from the telemetry)")
    args = parser.parse_args()

    rows = read_telemetry(args.telemetry)
    if not rows:
        sys.exit("no telemetry rows found")
    runs = max(int(r["run"]) for r in rows) + 1
    last_step, cast_not_done = read_stderr(args.stderr)
    out = [f"# Replay of {os.path.basename(args.telemetry)}",
           f"{len(set(int(r['bot']) for r in rows))} bots, {runs} run(s), "
           f"{round(max(as_int(r['active_ms']) for r in rows) / 3600000, 2)} h of run time.\n"]
    section_leveling(rows, last_step, out)
    section_deaths(rows, out)
    section_stuck(rows, out)
    section_fights(rows, out)
    section_refusals(rows, cast_not_done, out)
    section_targets(rows, out)
    print("\n".join(out))


if __name__ == "__main__":
    main()
