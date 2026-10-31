#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Turn one sim journal capture into ordered events and candidate metrics."""
import argparse
import json
import re
from collections import Counter
from pathlib import Path


OUTCOME = re.compile(
    r"forever-bots: scenario (?P<scenario>[\w.-]+?)(?:#(?P<party>\d+))?: outcome=(?P<outcome>\S+) "
    r"furthest=(?P<furthest>\d+)/(?P<route>\d+) deaths=(?P<deaths>\d+) alive=(?P<alive>\d+)/(?P<roster>\d+) "
    r"events=(?P<events>\d+) revived=(?P<revived>\d+) wipes=(?P<wipes>\d+) ghosts=(?P<ghosts>\d+) run=(?P<run>\d+)s"
    r"(?: party=(?P<party_formed>\w+))?(?: bosses=(?P<bosses>\d+)/(?P<boss_total>\d+) killed=(?P<boss_names>.*))?"
)
BOT = re.compile(r"forever-bots: scenario bot (?P<guid>\d+) race (?P<race>\d+) class (?P<class>\d+)")
BOSS_KILL = re.compile(r"scenario [\w.-]+?#(?P<party>\d+): boss killed: (?P<name>.+?)\s*$")
KV = re.compile(r"\b(?P<key>[a-z_]+)=(?P<value>[^\s]+)")
STAMP = re.compile(r"^(?P<stamp>[A-Z][a-z]{2} +\d+ \d\d:\d\d:\d\d)")

# Bot rotation spells and the dungeon spells that show up in the trace. Unknown ids stay numeric.
SPELL_NAMES = {
    17: "Power Word: Shield", 78: "Heroic Strike", 100: "Charge", 116: "Frostbolt", 122: "Frost Nova",
    133: "Fireball", 172: "Corruption", 331: "Healing Wave", 403: "Lightning Bolt", 585: "Smite",
    589: "Shadow Word: Pain", 635: "Holy Light", 686: "Shadow Bolt", 707: "Immolate", 772: "Rend",
    845: "Cleave", 853: "Hammer of Justice", 1160: "Demoralizing Shout", 1752: "Sinister Strike",
    1766: "Kick", 1978: "Serpent Sting", 2050: "Lesser Heal", 2054: "Heal", 2061: "Flash Heal",
    2098: "Eviscerate", 2136: "Fire Blast", 2687: "Bloodrage", 3044: "Arcane Shot", 5176: "Wrath",
    5185: "Healing Touch", 5308: "Execute", 6343: "Thunder Clap", 7386: "Sunder Armor", 8042: "Earth Shock",
    8050: "Flame Shock", 8092: "Mind Blast", 8921: "Moonfire",
}
# Deadmines bosses and their guards, used to label activity blocks.
BOSS_ROSTER = {"Rhahk'Zor", "Sneed", "Sneed's Shredder", "Gilnid", "Mr. Smite", "Captain Greenskin",
               "Edwin VanCleef", "Cookie"}


def ints(values, *keys):
    for key in keys:
        if key in values:
            try:
                values[key] = int(values[key])
            except ValueError:
                pass
    return values


def parse_trace_line(line, ordinal):
    marker = "dungeon_telegraph "
    if marker in line:
        body = line.split(marker, 1)[1]
        values = {item["key"]: item["value"] for item in KV.finditer(body)}
        names = ("caster", "target", "attacker", "victim", "healer")
        for actor in names:
            match = re.search(rf"\b{actor}_name=(.*?)\s+{actor}_entry=", body)
            if match:
                values[f"{actor}_name"] = match.group(1)
        ints(values, "seq", "time_ms", "instance", "caster", "caster_entry", "target", "target_entry",
             "attacker", "attacker_entry", "victim", "victim_entry", "healer", "healer_entry", "spell",
             "amount", "effective", "attempted", "victim_hp_before", "target_hp_before")
        if values.get("event") not in {"cast_start", "damage", "heal"}:
            # journalctl splits a few long lines; a missing or partial event name means the rest
            # of the record is unreliable, so keep it out of metrics.
            values.setdefault("event", None)
            values["_truncated"] = True
        return {"ordinal": ordinal, "source": "core", **values}

    marker = "forever-bots: event cast_result: "
    if marker in line:
        body = line.split(marker, 1)[1]
        values = {item["key"]: item["value"] for item in KV.finditer(body)}
        status = re.search(r"\bstatus=(.*?)\s+hp=", body)
        if status:
            values["status"] = status.group(1)
        ints(values, "time_ms", "bot", "spell", "target", "hp", "power")
        return {"ordinal": ordinal, "source": "module", "event": "cast_result", **values}

    marker = "forever-bots: event healer_dps_los"
    if marker in line:
        body = line.split(marker + (":" if line.startswith(marker + ":") else ""), 1)[-1]
        values = {item["key"]: item["value"] for item in KV.finditer(body)}
        ints(values, "healer", "spell", "target")
        return {"ordinal": ordinal, "source": "module", "event": "healer_dps_los", **values}
    return None


def read_summary(path):
    parties, bots = [], {}
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        match = BOT.search(line)
        if match:
            bots[int(match["guid"])] = {"race": int(match["race"]), "class": int(match["class"])}
            continue
        match = OUTCOME.search(line)
        if match:
            values = match.groupdict()
            for key in ("party", "furthest", "route", "deaths", "alive", "roster", "events", "revived", "wipes", "ghosts", "run", "bosses", "boss_total"):
                if values.get(key) is not None:
                    values[key] = int(values[key])
            values["party"] = values["party"] or 1
            values["bosses"] = values.get("bosses") or 0
            values["boss_total"] = values.get("boss_total") or 0
            parties.append(values)
    return parties, bots


def read_kills(path):
    """Boss kills as (party, clock seconds, name) from a dm-run.log summary capture."""
    kills = []
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        match = BOSS_KILL.search(line)
        stamp = STAMP.match(line)
        if match and stamp:
            kills.append((int(match["party"]), clock_seconds(stamp["stamp"]), match["name"]))
    return kills


def clock_seconds(stamp):
    match = re.search(r"(\d\d):(\d\d):(\d\d)$", stamp)
    if not match:
        return 0
    hours, minutes, seconds = (int(part) for part in match.groups())
    return hours * 3600 + minutes * 60 + seconds


def clock_label(seconds):
    seconds %= 24 * 3600
    return f"{seconds // 3600:02d}:{seconds % 3600 // 60:02d}:{seconds % 60:02d}"


def relative_label(seconds):
    sign = "-" if seconds < 0 else "+"
    seconds = abs(seconds)
    return f"{sign}{seconds // 60:02d}:{seconds % 60:02d}"


def spell_label(spell):
    try:
        spell = int(spell)
    except (TypeError, ValueError):
        return "spell ?"
    if spell == 0:
        return "melee"
    name = SPELL_NAMES.get(spell)
    return f"{name} ({spell})" if name else f"spell {spell}"


def owner_map(events):
    """Map every unit id seen in a core event to the party instance that observed it."""
    owners = {}
    for event in events:
        if event.get("source") != "core" or not event.get("instance"):
            continue
        for actor in ("caster", "healer", "attacker", "victim", "target"):
            unit = event.get(actor)
            if unit:
                owners.setdefault(unit, event["instance"])
    return owners


def event_actor(event):
    if event.get("source") == "module":
        return event.get("bot") or event.get("healer")
    for actor in ("caster", "healer", "attacker", "victim", "target"):
        if event.get(actor) is not None:
            return event[actor]
    return None


def event_name(event, actor):
    return event.get(f"{actor}_name") or "?"


def render_event(event):
    """One human-readable line for an ordered fight trace, without the timestamp prefix."""
    kind = event.get("event")
    if kind == "cast_start":
        return (f"cast   {event_name(event, 'caster')}({event.get('caster')}) starts "
                f"{spell_label(event.get('spell'))}" + (" [triggered]" if event.get("triggered") == "true" else ""))
    if kind == "damage":
        return (f"damage {event_name(event, 'attacker')}({event.get('attacker')}) -> "
                f"{event_name(event, 'victim')}({event.get('victim')}) {event.get('amount', 0)} "
                f"({event.get('effective', 0)} effective, {spell_label(event.get('spell'))}, "
                f"victim hp {event.get('victim_hp_before', '?')})")
    if kind == "heal":
        return (f"heal   {event_name(event, 'healer')}({event.get('healer')}) -> "
                f"{event_name(event, 'target')}({event.get('target')}) +{event.get('effective', 0)}"
                f"/{event.get('attempted', 0)} ({spell_label(event.get('spell'))}, "
                f"target hp {event.get('target_hp_before', '?')})")
    if kind == "cast_result":
        return (f"result bot {event.get('bot')} {spell_label(event.get('spell'))} -> {event.get('target')} "
                f"status={event.get('status')} hp={event.get('hp')} power={event.get('power')}")
    details = " ".join(f"{key}={value}" for key, value in sorted(event.items())
                       if key not in {"ordinal", "source", "event", "journal_time", "stamp_seconds"})
    return f"{kind:<6} {details}"


def build_timeline(events, party, gap=30, include_results=False, kills=(), min_events=1, known_bots=None):
    """Ordered, block-per-fight view of one party's events; returns printable lines."""
    owners = owner_map(events)
    selected = []
    for event in events:
        if event.get("source") == "core":
            if event.get("instance") != party:
                continue
        else:
            unit = event_actor(event)
            if owners.get(unit) != party:
                continue
            if event.get("event") == "cast_result" and not include_results:
                continue
        selected.append(event)
    if not selected:
        return [f"party {party}: no events"]
    selected.sort(key=lambda event: (event.get("stamp_seconds", 0), event["ordinal"]))

    blocks, current = [], [selected[0]]
    for event in selected[1:]:
        if event.get("stamp_seconds", 0) - current[-1].get("stamp_seconds", 0) > gap:
            blocks.append(current)
            current = []
        current.append(event)
    blocks.append(current)

    party_kills = [kill for kill in kills if kill[0] == party]
    module_bots = {unit for event in events if event.get("source") == "module"
                   for unit in (event.get("bot"), event.get("healer")) if unit}
    bots = sorted(unit for unit in (known_bots or module_bots) if owners.get(unit) == party)
    lines = []
    for index, block in enumerate(blocks, start=1):
        if len(block) < min_events:
            continue
        start = block[0].get("stamp_seconds", 0)
        end = block[-1].get("stamp_seconds", 0)
        names = {event_name(event, actor) for event in block
                 for actor in ("caster", "healer", "attacker", "victim", "target")
                 if event.get(f"{actor}_name")}
        bosses = sorted(name for name in names if name in BOSS_ROSTER)
        killed = [f"{name} at {clock_label(stamp)}" for _, stamp, name in party_kills if start <= stamp <= end]
        header = (f"== party {party} block {index}  {clock_label(start)}-{clock_label(end)} "
                  f"({relative_label(end - start)[1:]})  events={len(block)}")
        if bots:
            header += f"  bots={','.join(str(bot) for bot in bots)}"
        if bosses:
            header += f"  npcs={','.join(bosses)}"
        if killed:
            header += f"  KILLED={','.join(killed)}"
        lines.append(header)
        pending = [kill for kill in party_kills if start <= kill[1] <= end]
        for event in block:
            stamp = event.get("stamp_seconds", 0)
            for kill in list(pending):
                if stamp >= kill[1]:
                    lines.append(f"{clock_label(kill[1])} {relative_label(kill[1] - start)}  "
                                 f"---- boss killed: {kill[2]} ----")
                    pending.remove(kill)
            lines.append(f"{clock_label(stamp)} {relative_label(stamp - start)}  {render_event(event)}")
        for kill in pending:
            lines.append(f"{clock_label(kill[1])} {relative_label(kill[1] - start)}  "
                         f"---- boss killed: {kill[2]} ----")
        lines.append("")
    return lines


def summarize_events(events, scenario_bots):
    counts = Counter()
    cast_results = Counter()
    by_spell = Counter()
    damage = heal = attempted_heal = 0
    bot_casts = bot_damage = bot_heals = 0
    los_refusals = 0
    for event in events:
        kind = event.get("event")
        if event["source"] == "core" and kind is not None:
            counts[kind] += 1
            if kind == "cast_start":
                by_spell[str(event.get("spell", 0))] += 1
                bot_casts += event.get("caster") in scenario_bots
            elif kind == "damage":
                damage += event.get("effective", 0)
                bot_damage += event.get("attacker") in scenario_bots
            elif kind == "heal":
                heal += event.get("effective", 0)
                attempted_heal += event.get("attempted", 0)
                bot_heals += event.get("healer") in scenario_bots
        elif kind == "cast_result":
            status = str(event.get("status", "unknown")).lower()
            cast_results[status] += 1
            los_refusals += "line_of_sight" in status or "line of sight" in status
    return {
        "event_counts": dict(counts),
        "cast_result_statuses": dict(cast_results),
        "cast_starts_by_spell": dict(by_spell),
        "bot_cast_starts": bot_casts,
        "effective_damage": damage,
        "bot_damage_events": bot_damage,
        "effective_healing": heal,
        "attempted_healing": attempted_heal,
        "bot_heal_events": bot_heals,
        "los_refusals": los_refusals,
    }


def load_events(trace_path):
    events, truncated = [], 0
    for ordinal, line in enumerate(Path(trace_path).read_text(encoding="utf-8", errors="replace").splitlines()):
        event = parse_trace_line(line, ordinal)
        if not event:
            continue
        if event.get("_truncated"):
            truncated += 1
            continue
        stamp = STAMP.match(line)
        if stamp:
            event["journal_time"] = stamp["stamp"]
            event["stamp_seconds"] = clock_seconds(stamp["stamp"])
        events.append(event)
    return events, truncated


def analyze(summary_path, trace_path, out_dir, candidate, seed, expected_parties, params):
    parties, bots = read_summary(summary_path)
    events, truncated = load_events(trace_path)
    n = len(parties)
    metric = {
        "schema_version": 1,
        "candidate": candidate,
        "seed": seed,
        "expected_parties": expected_parties,
        "params": params,
        "scenario_bots": len(bots),
        "parties": parties,
        "party_count": n,
        "aggregate": {
            "mean_bosses": sum(p["bosses"] for p in parties) / n if n else 0.0,
            "complete_rate": sum(p["outcome"] == "complete" for p in parties) / n if n else 0.0,
            "mean_deaths": sum(p["deaths"] for p in parties) / n if n else 0.0,
            "wipe_rate": sum(p["wipes"] > 0 or p["outcome"] == "wipe" for p in parties) / n if n else 0.0,
            "mean_run_sec": sum(p["run"] for p in parties) / n if n else 0.0,
        },
        "combat": summarize_events([e for e in events if e.get("source") == "module" or e.get("event") in ("cast_start", "damage", "heal")], set(bots)),
        "trace_events": len(events),
        "truncated_trace_lines": truncated,
        "warnings": [] if n == expected_parties else [f"expected {expected_parties} party outcomes, found {n}"],
    }
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    with (out / "events.jsonl").open("w", encoding="utf-8") as stream:
        for event in events:
            stream.write(json.dumps(event, sort_keys=True) + "\n")
    (out / "metrics.json").write_text(json.dumps(metric, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return metric


def rank_key(metric):
    aggregate = metric["aggregate"]
    combat = metric["combat"]
    return (aggregate["mean_bosses"], aggregate["complete_rate"], -aggregate["mean_deaths"],
            -aggregate["wipe_rate"], -combat["los_refusals"])


def campaign_metrics(paths):
    return [json.loads(Path(path).read_text(encoding="utf-8")) for path in paths]


def complete_metrics(metrics):
    return [m for m in metrics if m.get("party_count") == m.get("expected_parties") and not m.get("warnings")]


def propose(metrics):
    metrics = complete_metrics(metrics)
    if not metrics:
        return []
    best = max(metrics, key=rank_key)
    ranges = {"heal": (30, 85, 5), "tank_heal": (40, 95, 5), "dps_mana": (10, 80, 10), "los_yards": (3, 12, 1)}
    config_names = {"heal": "DungeonHealBelowPct", "tank_heal": "DungeonTankHealBelowPct",
                    "dps_mana": "HealerDpsManaPct", "los_yards": "HealerLosMoveYards"}
    tested = {tuple(sorted(m.get("params", {}).items())) for m in metrics}
    result = []
    params = dict(best.get("params", {}))
    for knob, (minimum, maximum, step) in ranges.items():
        current = int(params.get(knob, {"heal": 55, "tank_heal": 75, "dps_mana": 40, "los_yards": 6}[knob]))
        for value in (current - step, current + step):
            value = max(minimum, min(maximum, value))
            if value == current:
                continue
            candidate = dict(params, **{knob: value})
            if tuple(sorted(candidate.items())) not in tested:
                result.append({"based_on": best["candidate"], "candidate": f"{best['candidate']}-{knob}-{value}", "params": candidate,
                               "config": f"Module.forever-bots.{config_names[knob]} = {value}"})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    run = commands.add_parser("analyze", help="normalize one journal capture and calculate its metrics")
    run.add_argument("--summary", required=True, help="dm-run.log from sim_dungeon_run.sh report")
    run.add_argument("--trace", required=True, help="dm-run.telegraph.log from sim_dungeon_run.sh report")
    run.add_argument("--out", required=True, help="candidate output directory")
    run.add_argument("--candidate", required=True)
    run.add_argument("--seed", type=int, default=1)
    run.add_argument("--parties", type=int, default=8)
    run.add_argument("--heal", type=int, default=55)
    run.add_argument("--tank-heal", type=int, default=75)
    run.add_argument("--dps-mana", type=int, default=40)
    run.add_argument("--los-yards", type=int, default=6)
    run.add_argument("--tactics", default="none", help="comma list of focus,interrupt,heal,rest, or all/none (default: none)")
    rank = commands.add_parser("rank", help="rank completed candidates lexicographically")
    rank.add_argument("metrics", nargs="+", help="candidate metrics.json files")
    suggest = commands.add_parser("propose", help="suggest bounded one-knob neighbors of the best candidate")
    suggest.add_argument("metrics", nargs="+", help="candidate metrics.json files")
    timeline = commands.add_parser("timeline", help="print one party's ordered fight trace, one block per pull")
    timeline.add_argument("--trace", required=True, help="dm-run.telegraph.log from sim_dungeon_run.sh report")
    timeline.add_argument("--summary", help="dm-run.log, used to annotate boss kills")
    timeline.add_argument("--party", type=int, required=True, help="party instance number (1-based, as in deadmines_a#N)")
    timeline.add_argument("--gap", type=int, default=30, help="seconds of silence that start a new block (default 30)")
    timeline.add_argument("--min-events", type=int, default=1, help="skip blocks with fewer events (default 1)")
    timeline.add_argument("--results", action="store_true", help="also show module cast_result lines with cast status")
    timeline.add_argument("--out", help="write the timeline here instead of stdout")
    args = parser.parse_args()
    if args.command == "analyze":
        metric = analyze(args.summary, args.trace, args.out, args.candidate, args.seed, args.parties,
                         {"heal": args.heal, "tank_heal": args.tank_heal, "dps_mana": args.dps_mana,
                          "los_yards": args.los_yards, "tactics": args.tactics})
        print(json.dumps({"metrics": str(Path(args.out) / "metrics.json"), "events": str(Path(args.out) / "events.jsonl"),
                          "party_count": metric["party_count"], "warnings": metric["warnings"]}, indent=2))
    elif args.command == "rank":
        metrics = campaign_metrics(args.metrics)
        complete = complete_metrics(metrics)
        print(f"included {len(complete)}/{len(metrics)} complete candidate batches")
        for index, metric in enumerate(sorted(complete, key=rank_key, reverse=True), start=1):
            a, c = metric["aggregate"], metric["combat"]
            print(f"{index}. {metric['candidate']}: bosses={a['mean_bosses']:.2f}, complete={a['complete_rate']:.0%}, "
                  f"deaths={a['mean_deaths']:.2f}, wipes={a['wipe_rate']:.0%}, los_refusals={c['los_refusals']}, "
                  f"seed={metric['seed']} parties={metric['party_count']} params={metric['params']}")
    elif args.command == "timeline":
        events, _ = load_events(args.trace)
        kills = read_kills(args.summary) if args.summary else []
        known_bots = set(read_summary(args.summary)[1]) if args.summary else None
        lines = build_timeline(events, args.party, gap=args.gap, include_results=args.results,
                               kills=kills, min_events=args.min_events, known_bots=known_bots)
        text = "\n".join(lines) + "\n"
        if args.out:
            Path(args.out).write_text(text, encoding="utf-8")
            print(f"timeline: {args.out} ({len(lines)} lines)")
        else:
            print(text, end="")
    else:
        metrics = campaign_metrics(args.metrics)
        complete = complete_metrics(metrics)
        print(f"# proposing from {len(complete)}/{len(metrics)} complete candidate batches")
        for item in propose(complete):
            print(json.dumps(item, sort_keys=True))


if __name__ == "__main__":
    main()
