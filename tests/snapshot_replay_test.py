"""Tests for tools/snapshot_replay.py: each open finding of the sim handoff is reported from a hand-built telemetry file."""
import os
import subprocess
import sys
import tempfile

HEADER = ("time_ms,kind,bot,level,health_pct,power_pct,in_fight,fights,targets_gone,deaths,"
          "casts_applied,refusals,stuck_episodes,longest_fight_ms,health_lost,detail\n")
TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "snapshot_replay.py")

MIN = 60000

failures = 0


def check(condition, message):
    global failures
    if not condition:
        failures += 1
        print("FAIL", message)


def row(time_ms, kind, bot, level, fights=0, targets_gone=0, deaths=0, episodes=0,
        longest=0, applied=0, refusals=0, detail=""):
    return (f"{time_ms},{kind},{bot},{level},100,100,0,{fights},{targets_gone},{deaths},"
            f"{applied},{refusals},{episodes},{longest},0,{detail}\n")


def report(text, stderr=None):
    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, "t.csv")
        with open(path, "w") as handle:
            handle.write(HEADER + text)
        command = [sys.executable, "-I", TOOL, "--telemetry", path]
        if stderr is not None:
            stderr_path = os.path.join(directory, "ws-err.txt")
            with open(stderr_path, "w") as handle:
                handle.write(stderr)
            command += ["--stderr", stderr_path]
        done = subprocess.run(command, capture_output=True, text=True)
        assert done.returncode == 0, done.stderr
        return done.stdout


# F1: three bots on level 1 at the end, two of them last told to walk. The last line of a bot carries its level.
f1 = "".join(row(i * MIN, "summary", bot, 1, fights=2) for bot in (1, 2, 3) for i in range(3))
f1 += "".join(row(10 * MIN, "level", 4, 5, detail="from 4 to 5") for _ in range(2))
step = ("forever-bots: event quest_state: bot=1 level=1 map=0 allowed=1 enabled=1 party=0 "
        "low_health=0 in_combat=0 chat_stay=0 chat_follow=0 step=walk_to quest=783 log=0 "
        "start_index=0 memory_done=0 memory_log=0\n")
step += "forever-bots: event quest_state: bot=2 level=1 map=0 allowed=1 enabled=1 party=0 step=hunt quest=7 log=1\n"
text = report(f1, stderr=step)
check("3 of 4 bots end on level 1 or below" in text, "level-1 count: " + text)
check("1 of them last sat in `walk_to`" in text, "walk_to count: " + text)
check("bot 1: step=walk_to quest=783" in text, "the bot and its last quest: " + text)

# F2: deaths are counted per level, not only in total.
f2 = "".join(row(i * MIN, "summary", 1, 3, fights=4) for i in range(2))
f2 += row(2 * MIN, "death", 1, 3, detail="killer 55")
f2 += row(3 * MIN, "death", 1, 3, detail="killer 55")
f2 += row(4 * MIN, "death", 2, 12, detail="killer 77")
text = report(f2)
check("3 deaths" in text, "death total: " + text)
check("level 3: 2" in text, "deaths at one level: " + text)
check("level 12: 1" in text, "deaths at another level: " + text)

# F3: episodes are reported beside the fights, so a route problem and a combat loop can be told apart.
f3 = "".join(
    f"{i * MIN},summary,7,4,100,100,0,{3 if i else 0},1,0,10,20,{5 if i else 0},0,0,\n" for i in range(3))
text = report(f3)
check("bot 7: 5 / 3 (level 4)" in text, "episodes beside fights: " + text)

# F4: a fight far beyond the watchdog is reported with the rows around it, in time order.
f4 = "".join(f"{i * MIN},summary,9,2,100,100,1,{1 if i else 0},0,0,4,6,{1 if i else 0},0,0,\n" for i in range(5))
f4 += f"{5 * MIN},fight_start,9,2,100,100,1,1,0,0,0,0,1,0,0,target 55\n"
f4 += f"{7 * MIN},summary,9,2,80,100,1,1,0,0,4,6,1,11083000,200,\n"
f4 += (f"{8 * MIN},fight_end,9,2,50,100,0,1,0,0,4,6,1,11083000,500,"
       "reason target_gone target 55 duration_ms 11083000 health_lost 500\n")
text = report(f4)
check("11083000 ms (184.7 minutes)" in text, "the longest fight in minutes: " + text)
check("beyond twice the watchdog" not in text, "a fight beyond the watchdog is not waved away: " + text)
check("fight_start" in text and "fight_end" in text, "the rows around the fight: " + text)

# F5: applied casts against refusals, and the reason from the stderr.
refused = "forever-bots: bot 9 cast spell 1978 not done: out of range\n" * 3
f5 = "".join(row(i * MIN, "summary", 3, 1, fights=1, applied=100, refusals=30) for i in range(2))
text = report(f5, stderr=refused)
check("100 casts applied, 30 actions answered otherwise" in text, "applied against refused: " + text)
check("cast spell 1978 -> out of range: 3" in text, "the refusal reason: " + text)

# F6: targets gone is split by level, and the reason it ended.
f6 = "".join(row(i * MIN, "summary", 5, 20, fights=10, targets_gone=6) for i in range(3))
f6 += f"{3 * MIN},fight_end,5,20,80,100,1,10,6,0,4,6,1,0,20,reason target_gone target 55 duration_ms 2000 health_lost 20\n"
f6 += "".join(row(i * MIN, "summary", 6, 33, fights=10, targets_gone=1) for i in range(3))
text = report(f6)
check("20 fights, 7 ending with the target gone" in text, "fights against targets gone: " + text)
check("level 20: 10 / 6" in text, "by level: " + text)
check("target_gone 1" in text, "the reason a fight ended: " + text)

# A file with no rows is refused rather than reported as an empty run.
done = subprocess.run([sys.executable, "-I", TOOL, "--telemetry", os.devnull], capture_output=True, text=True)
check(done.returncode != 0 and "no telemetry rows" in done.stderr, "an empty file is refused")

if failures == 0:
    print("snapshot_replay_test: all checks passed")
    sys.exit(0)
sys.exit(1)
