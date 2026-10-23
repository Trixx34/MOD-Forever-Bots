# Offline dungeon policy learning

This is a reproducible, offline policy-search loop, not live self-modification. It uses the sim's ordered combat trace and per-party outcomes to compare bounded policy candidates. A candidate changes one or more config knobs; it does not edit code or merge itself.

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

Build/deploy the bot module and test core first. `experiment.sh` is the self-service entry point; it records a manifest and keeps run output outside the source tree. Its `start` arguments are name, parties, seed, heal threshold, tank threshold, DPS mana threshold, LoS move distance, and duration:

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

The analyzer writes `events.jsonl` in journal order (core sequence numbers are retained) plus `metrics.json` with party outcomes, bot cast results, effective damage/healing, and LoS refusals. It also preserves the raw summary and telegraph logs for replay and later feature extraction.

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
