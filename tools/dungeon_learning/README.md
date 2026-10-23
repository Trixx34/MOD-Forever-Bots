# Offline dungeon policy learning

This is a reproducible, offline policy-search loop, not live self-modification. It uses the sim's ordered combat trace and per-party outcomes to compare bounded policy candidates. A candidate changes one or more config knobs; it does not edit code or merge itself.

## Scripts

Run these from the bot repository root:

- `tools/dungeon_learning/experiment.sh` — recommended entry point. Starts a named test run, checks status, collects/analyzes the report, and ranks or proposes candidates.
- `tools/sim_dungeon_run.sh` — lower-level sim start/report helper. It SSHes to `forever-test`, writes the scenario config, and restarts the sim. Prefer `experiment.sh` because it records candidate settings and protects local run state.
- `tools/dungeon_learning/analyze.py` — standard-library-only parser/ranker. Use directly only when analyzing logs collected another way; `experiment.sh report` calls it for you.

The scripts require SSH access to `forever-test` and passwordless `sudo` there. They only target the test sim. The experiment output is local and defaults to `/tmp/dm-learning/`.

## What varies

The module validates these candidate parameters and uses the current policy values as defaults:

| Parameter | Default | Allowed range | Search step |
|---|---:|---:|---:|
| `DungeonHealBelowPct` | 55 | 30–85 | 5 |
| `DungeonTankHealBelowPct` | 75 | 40–95 | 5 |
| `HealerDpsManaPct` | 40 | 10–80 | 10 |
| `HealerLosMoveYards` | 6 | 3–12 | 1 |

Each run uses one candidate across all parties. The runner supports up to eight simultaneous five-bot parties, which gives repeated observations for that candidate. Per-party A/B variants are a later extension; don't compare different candidates in one batch until that is implemented.

## Run a candidate

Build/deploy the telegraph-enabled test core and bot module with tuning support first. Then run `start`; its positional arguments are **name, parties, seed, heal threshold, tank threshold, DPS mana threshold, LoS move distance, duration**:

```bash
tools/dungeon_learning/experiment.sh start baseline 8 1 55 75 40 6 2400
tools/dungeon_learning/experiment.sh status baseline
# Once all 8 parties have an outcome:
tools/dungeon_learning/experiment.sh report baseline
```

The script refuses to restart the sim while the current scenario has unfinished parties, so starting a new candidate cannot silently interrupt a run. Default output is `/tmp/dm-learning/baseline`; set `RUN_ROOT=/path` to move it. Use a new name for every run, including repeats with a different seed:

```bash
tools/dungeon_learning/experiment.sh start baseline-seed-2 8 2 55 75 40 6 2400
tools/dungeon_learning/experiment.sh report baseline-seed-2
```

`status` is a snapshot; run it again later to see progress. `report` may be run again after more parties finish. The launcher keeps the active-run marker until the expected number of party outcomes is present, then permits the next run.

Each run folder contains `candidate.env`, `dm-run.start`, `dm-run.log`, `dm-run.telegraph.log`, `events.jsonl`, and `metrics.json`. The JSONL trace is in journal order (core sequence numbers are retained); metrics include party outcomes, bot cast results, effective damage/healing, and LoS refusals. Raw logs are kept for replay and later feature extraction.

## Review a fight

`timeline` prints one party's events in order, split into blocks when nothing happens for `--gap` seconds (default 30). Each block header lists the clock window, event count, party bots, NPCs seen, and any boss killed inside it; the kill is also marked inline. Cast starts, damage, and heals come from the core telegraph; `--results` adds the module's cast results (`applied`, `no line of sight`, `not enough power`, ...):

```bash
tools/dungeon_learning/experiment.sh timeline baseline 3 --results
# -> /tmp/dm-learning/baseline/party3.timeline.txt
```

The capture is journal-based, so timestamps have one-second resolution and journald can split a small number of long lines; the analyzer drops those fragments from metrics (they stay in `events.jsonl` as `_truncated`) and `metrics.json` counts them under `truncated_trace_lines`. Ordering inside one second follows journal order.

## Compare and choose the next experiment

Keep one metrics file per candidate and seed. Rank uses an explicit lexicographic objective—not an opaque weighted score:

1. Mean bosses killed (higher)
2. Completion rate (higher)
3. Mean deaths (lower)
4. Wipe rate (lower)
5. LoS refusals (lower)

```bash
tools/dungeon_learning/experiment.sh rank
tools/dungeon_learning/experiment.sh propose
```

`propose` suggests one-knob neighbors of the best candidate within the validated bounds. Re-run the top candidates on at least one held-out seed before adopting anything. Treat the result as a recommendation; review the trace and merge changes manually. Run output stays under `/tmp`, not in the source tree.

## Safety and interpretation

- This harness targets the test sim only; it does not change production or push policy automatically.
- Compare batches with the same party count, duration, roster, dungeon data, and seed. Repeat top candidates on different seeds to reduce luck bias.
- The trace's event order comes from the single journal capture. Core `seq` orders core events; module cast results include `time_ms` and remain in journal order.
- A candidate is incomplete if its `metrics.json` warns that the expected party outcome count was not observed.
- Current batch mode runs one candidate at a time. Eight parties are eight replicates, not eight different policies.
