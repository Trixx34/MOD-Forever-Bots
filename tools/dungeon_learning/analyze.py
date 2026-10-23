#!/usr/bin/env python3
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
KV = re.compile(r"\b(?P<key>[a-z_]+)=(?P<value>[^\s]+)")
STAMP = re.compile(r"^(?P<stamp>[A-Z][a-z]{2} +\d+ \d\d:\d\d:\d\d)")


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
        if "event" not in values:
            # journalctl wraps long lines; keep the partial event out of metrics while retaining it in the trace.
            values["event"] = None
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


def analyze(summary_path, trace_path, out_dir, candidate, seed, expected_parties, params):
    parties, bots = read_summary(summary_path)
    events = []
    for ordinal, line in enumerate(Path(trace_path).read_text(encoding="utf-8", errors="replace").splitlines()):
        event = parse_trace_line(line, ordinal)
        if event and not event.get("_truncated"):
            stamp = STAMP.match(line)
            if stamp:
                event["journal_time"] = stamp["stamp"]
            events.append(event)
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
        "truncated_trace_lines": 0,
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
    rank = commands.add_parser("rank", help="rank completed candidates lexicographically")
    rank.add_argument("metrics", nargs="+", help="candidate metrics.json files")
    suggest = commands.add_parser("propose", help="suggest bounded one-knob neighbors of the best candidate")
    suggest.add_argument("metrics", nargs="+", help="candidate metrics.json files")
    args = parser.parse_args()
    if args.command == "analyze":
        metric = analyze(args.summary, args.trace, args.out, args.candidate, args.seed, args.parties,
                         {"heal": args.heal, "tank_heal": args.tank_heal, "dps_mana": args.dps_mana, "los_yards": args.los_yards})
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
    else:
        metrics = campaign_metrics(args.metrics)
        complete = complete_metrics(metrics)
        print(f"# proposing from {len(complete)}/{len(metrics)} complete candidate batches")
        for item in propose(complete):
            print(json.dumps(item, sort_keys=True))


if __name__ == "__main__":
    main()
