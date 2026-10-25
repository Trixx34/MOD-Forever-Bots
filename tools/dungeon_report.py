#!/usr/bin/python3 -I
"""Report of a dungeon scenario run from the module's log (the sim worldserver journal).

    sudo journalctl -u forever-sim --since "18:00" --no-pager -q | python3 -I tools/dungeon_report.py [--names names.tsv] [--bosses 7]
    python3 -I tools/dungeon_report.py journal.txt            # or a file; use - for stdin

Per party (ScenarioParties runs several at once; the log names them dm#1, dm#2, ...; a single party is just "dm") it prints: when the run started, each boss with its kill
time since the start, the deaths (bot, time, the creature entry that killed it, named with --names: a tab separated "entry<TAB>name" file), the wipes and ghosts, the
outcome line, and the counts of "no route" and other refusals. Then an aggregate over all parties: runs, completions, mean bosses killed, deaths per run, wipes, mean time
per boss. A bot belongs to the party its rank in guid order falls in (the scenario's bots, in groups of the roster size: 5), the same cut the module makes.

Lines it reads (module log): "scenario <name>: phase <p>", "scenario <name>: boss killed: <boss>", "scenario <name>: outcome=...", "scenario bot <guid> race <r> class <c>",
"event died: bot=<g> ... killer_entry=<e>", "event ghost: bot=<g>", "event revived: bot=<g>", "bot <g> <action> not done: <reason>".
"""
import argparse
import collections
import re
import sys
from datetime import datetime

STAMP = re.compile(r"^(?P<mon>[A-Z][a-z]{2}) +(?P<day>\d+) (?P<h>\d\d):(?P<m>\d\d):(?P<s>\d\d)\b")
ISO = re.compile(r"^(?P<y>\d{4})-(?P<mo>\d\d)-(?P<d>\d\d)[ T](?P<h>\d\d):(?P<m>\d\d):(?P<s>\d\d)")
MONTHS = {m: i + 1 for i, m in enumerate("Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec".split())}


def stamp_of(line: str):
    """Seconds since an arbitrary origin (a day count and the time of day), or None."""
    m = STAMP.match(line)
    if m:
        return (MONTHS[m["mon"]] * 31 + int(m["day"])) * 86400 + int(m["h"]) * 3600 + int(m["m"]) * 60 + int(m["s"])
    m = ISO.match(line)
    if m:
        return (int(m["mo"]) * 31 + int(m["d"])) * 86400 + int(m["h"]) * 3600 + int(m["m"]) * 60 + int(m["s"])
    return None


def clock(seconds: float) -> str:
    seconds = int(seconds)
    return f"{seconds // 60}:{seconds % 60:02d}"


SCEN = re.compile(r"forever-bots: scenario (?P<name>[\w.-]+?)(?:#(?P<party>\d+))?: (?P<rest>.*)$")
SCEN_BOT = re.compile(r"forever-bots: scenario bot (?P<guid>\d+) race (?P<race>\d+) class (?P<cls>\d+)")
RESULT = re.compile(r"forever-bots: scenario (?P<name>[\w.-]+?)(?:#(?P<party>\d+))?: outcome=(?P<outcome>\S+) furthest=(?P<furthest>\d+)/(?P<route>\d+) deaths=(?P<deaths>\d+) "
                    r"alive=(?P<alive>\d+)/(?P<roster>\d+) events=(?P<events>\d+) revived=(?P<revived>\d+) wipes=(?P<wipes>\d+) ghosts=(?P<ghosts>\d+) run=(?P<run>\d+)s"
                    r"(?: party=(?P<party_formed>\w+))?(?: bosses=(?P<killed>\d+)/(?P<total>\d+) killed=(?P<names>.*))?")
DIED = re.compile(r"forever-bots: event died: bot=(?P<bot>\d+) life=(?P<life>\d+) map=(?P<map>\d+)\s+(?:pos=(?P<pos>[\d-]+,[\d-]+)\s+)?last_decision=(?P<action>\S+).* killer_entry=(?P<killer>\S+)\s+in_combat=(?P<combat>\d)")
PULL = re.compile(r"forever-bots: event pull: bot=(?P<bot>\d+) map=(?P<map>\d+) pos=(?P<pos>[\d-]+,[\d-]+) hp=(?P<hp>\d+)/(?P<hpmax>\d+) power=(?P<power>\d+)/(?P<powermax>\d+) target=(?P<target>\d+)")
GHOST = re.compile(r"forever-bots: event ghost: bot=(?P<bot>\d+)")
REVIVED = re.compile(r"forever-bots: event revived: bot=(?P<bot>\d+)")
TACTIC = re.compile(r"forever-bots: event (?P<kind>focus|interrupt_target|heal): (?:bot|healer)=(?P<bot>\d+)")
NOTDONE = re.compile(r"forever-bots: bot (?P<bot>\d+) (?P<what>.+?) not done: (?P<why>.+)$")


class Party:
    def __init__(self, key):
        self.key = key
        self.started = None
        self.phases = []                  # (time, phase)
        self.bosses = []                  # (time, name)
        self.result = None
        self.result_at = None
        self.deaths = []                  # (time, bot, killer)
        self.ghosts = 0
        self.revives = 0
        self.no_route = 0
        self.refusals = collections.Counter()
        self.tactics = collections.Counter()
        self.pulls = []
        self.bots = []


def read(lines, since=None):
    parties = collections.OrderedDict()
    spawned = []                           # scenario bots in spawn order
    events = []                            # (time, kind, data) for per-bot lines, assigned to parties at the end
    origin = None
    for line in lines:
        t = stamp_of(line)
        if t is not None and origin is None:
            origin = t
        if since is not None and t is not None and t < since:
            continue
        m = SCEN_BOT.search(line)
        if m:
            spawned.append(int(m["guid"]))
            continue
        m = RESULT.search(line)
        if m:
            key = m["party"] or "1"
            party = parties.setdefault(key, Party(key))
            party.result = m.groupdict()
            party.result_at = t
            continue
        m = SCEN.search(line)
        if m:
            key = m["party"] or "1"
            party = parties.setdefault(key, Party(key))
            rest = m["rest"]
            if rest.startswith("phase running") and party.started is None:
                party.started = t
            elif rest.startswith("phase "):
                party.phases.append((t, rest.split()[1] if len(rest.split()) > 1 else rest))
            elif rest.startswith("boss killed: "):
                party.bosses.append((t, rest[len("boss killed: "):].strip()))
            continue
        m = PULL.search(line)
        if m:
            events.append((t, "pull", (int(m["bot"]), int(m["map"]), m["pos"], int(m["hp"]), int(m["hpmax"]),
                                       int(m["power"]), int(m["powermax"]), int(m["target"]))))
            continue
        m = DIED.search(line)
        if m:
            events.append((t, "died", (int(m["bot"]), m["killer"], int(m["map"]), m["pos"], m["action"], int(m["combat"]))))
            continue
        if GHOST.search(line):
            events.append((t, "ghost", (int(GHOST.search(line)["bot"]),)))
            continue
        m = REVIVED.search(line)
        if m:
            events.append((t, "revived", (int(m["bot"]),)))
            continue
        m = TACTIC.search(line)
        if m:
            events.append((t, "tactic", (int(m["bot"]), m["kind"])))
            continue
        m = NOTDONE.search(line)
        if m:
            events.append((t, "notdone", (int(m["bot"]), m["what"], m["why"])))
    return parties, spawned, events, origin


def assign(parties, spawned, events, roster):
    """Cut the scenario bots, in guid order, into parties of `roster` and give each party its per-bot events."""
    guids = sorted(spawned)
    party_of = {}
    keys = list(parties.keys()) or ["1"]
    for i, guid in enumerate(guids):
        index = min(i // roster, len(keys) - 1)
        party_of[guid] = keys[index] if len(keys) > 1 else keys[0]
        parties.setdefault(party_of[guid], Party(party_of[guid])).bots.append(guid)
    for t, kind, data in events:
        guid = data[0]
        key = party_of.get(guid)
        if key is None:
            continue
        party = parties[key]
        if kind == "died":
            if party.started is None or (t is not None and t >= party.started):
                party.deaths.append((t, guid, data[1], data[2], data[3], data[4], data[5]))
        elif kind == "ghost":
            party.ghosts += 1
        elif kind == "revived":
            party.revives += 1
        elif kind == "tactic":
            party.tactics[data[1]] += 1
        elif kind == "pull":
            if party.started is None or (t is not None and t >= party.started):
                party.pulls.append((t, data))
        elif kind == "notdone":
            if "no route" in data[2]:
                party.no_route += 1
            else:
                party.refusals[f"{data[1]}: {data[2]}"] += 1


def load_names(path):
    names = {}
    if path:
        with open(path, encoding="utf-8") as f:
            for line in f:
                parts = line.rstrip("\n").split("\t")
                if len(parts) >= 2 and parts[0].isdigit():
                    names[parts[0]] = parts[1]
    return names


# the boss names that make up one encounter: Sneed's Shredder and Sneed are counted once (the review's
# decision of 2026-10-22), so the encounter is complete only when both are dead
ENCOUNTERS = (("Sneed's Shredder", "Sneed"),)


def encounters_of(killed):
    """The names of the encounters that completed: a pair counts when both of its parts are dead."""
    done = []
    for parts in ENCOUNTERS:
        if all(part in killed for part in parts):
            done.append(" + ".join(parts))
    return done


def report(parties, names, total_bosses):
    rows = []
    for key, party in parties.items():
        print(f"== party {key}  bots {party.bots}")
        start = party.started
        if start is None:
            print("   never started running")
        else:
            print(f"   running from {clock(start % 86400)} (clock time); times below are minutes:seconds into the run")
        for t, name in party.bosses:
            print(f"   boss killed  {name:<22} at {clock(t - start) if (t is not None and start is not None) else '?'}")
        for death in party.deaths:
            t, bot, killer, place, pos, action, combat = (death + ("?",))[:7] if len(death) == 6 else death
            who = names.get(killer, killer)
            when = clock(t - start) if (t is not None and start is not None) else '?'
            print(f"   death        bot {bot:<6} at {when:<7} killed by {who:<22} map {place} at {pos or '?'} action {action}{' in combat' if combat else ''}")
        r = party.result
        if r:
            print(f"   result       outcome={r['outcome']} furthest={r['furthest']}/{r['route']} deaths={r['deaths']} death_events={r['events']} revived={r['revived']} "
                  f"wipes={r['wipes']} ghosts={r['ghosts']} run={r['run']}s bosses={r['killed'] or 0}/{r['total'] or total_bosses}")
        else:
            print("   result       (none yet: still running, or the run did not end in this log)")
        refusals = ", ".join(f"{count}x {what}" for what, count in party.refusals.most_common(4)) or "none"
        print(f"   no route {party.no_route}; other refusals: {refusals}")
        if party.tactics:
            print("   tactics      " + ", ".join(f"{count}x {kind}" for kind, count in sorted(party.tactics.items())))
        if party.pulls:
            health = [100 * data[3] / data[4] for _t, data in party.pulls if data[4]]
            power = [100 * data[5] / data[6] for _t, data in party.pulls if data[6]]
            print(f"   pulls        {len(party.pulls)}  the party's health at the pull {sum(health) / len(health):.0f} % (the lowest {min(health):.0f} %)" if health else
                  f"   pulls        {len(party.pulls)}")
        killed = int(r["killed"]) if r and r["killed"] else len(party.bosses)
        rows.append({
            "key": key, "complete": bool(r and r["outcome"] == "complete"), "killed": killed, "deaths": int(r["events"]) if r else len(party.deaths),
            "wipes": int(r["wipes"]) if r else 0, "run": int(r["run"]) if r else 0, "ended": bool(r),
            "boss_times": [(t - start) for t, _ in party.bosses if t is not None and start is not None],
            "encounters": encounters_of([name for _, name in party.bosses]),
        })
    print("== aggregate")
    n = len(rows)
    if not n:
        print("   no party in the log")
        return
    ended = [row for row in rows if row["ended"]]
    print(f"   parties {n} (ended {len(ended)}); completed {sum(row['complete'] for row in rows)}/{n}")
    print(f"   bosses killed: mean {sum(row['killed'] for row in rows) / n:.2f} of {total_bosses}; best {max(row['killed'] for row in rows)}; per party {[row['killed'] for row in rows]}")
    print(f"   death events: mean {sum(row['deaths'] for row in rows) / n:.1f} per party; wipes: total {sum(row['wipes'] for row in rows)}")
    times = [t for row in rows for t in row["boss_times"]]
    if times:
        print(f"   first-boss kill times: {[clock(min(row['boss_times'])) for row in rows if row['boss_times']]}")
    # the Shredder and Sneed are one encounter (the review's decision: complete only when both are dead)
    encounters = [name for row in rows for name in row.get("encounters", [])]
    if encounters:
        print("   encounters: " + ", ".join(sorted(set(encounters))))
    killers = collections.Counter(k for party in parties.values() for death in party.deaths for k in [death[2]])
    if killers:
        print("   killers: " + ", ".join(f"{names.get(k, k)} x{c}" for k, c in killers.most_common(6)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", nargs="?", default="-", help="the journal text; - or none for stdin")
    ap.add_argument("--names", help="tab separated entry<TAB>name file for the killers")
    ap.add_argument("--bosses", type=int, default=7, help="bosses of the dungeon (Deadmines 7)")
    ap.add_argument("--roster", type=int, default=5, help="bots per party")
    ap.add_argument("--since", help="HH:MM:SS: ignore lines before this time of day")
    args = ap.parse_args()
    lines = sys.stdin if args.log == "-" else open(args.log, encoding="utf-8", errors="replace")
    since = None
    if args.since:
        h, m, *s = [int(x) for x in args.since.split(":")]
        since = None  # compared by time of day below
        wanted = h * 3600 + m * 60 + (s[0] if s else 0)
        lines = [l for l in lines if (lambda t: t is None or t % 86400 >= wanted)(stamp_of(l))]
    parties, spawned, events, _ = read(lines, since)
    assign(parties, spawned, events, args.roster)
    report(parties, load_names(args.names), args.bosses)


if __name__ == "__main__":
    main()
