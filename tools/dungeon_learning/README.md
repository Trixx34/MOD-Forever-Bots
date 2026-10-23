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

Build/deploy the bot module and test core first. Then run a bounded sim batch; parameters after the duration are seed, heal threshold, tank threshold, DPS mana threshold, and LoS move distance:

```bash
RUN_DIR=/tmp/dm-learning/candidate-55-75-40-6 \
  tools/sim_dungeon_run.sh start 8 none 2400 1 55 75 40 6
```

After the scenario parties finish:

```bash
RUN_DIR=/tmp/dm-learning/candidate-55-75-40-6 tools/sim_dungeon_run.sh report
python3 -I tools/dungeon_learning/analyze.py analyze \
  --summary /tmp/dm-learning/candidate-55-75-40-6/dm-run.log \
  --trace /tmp/dm-learning/candidate-55-75-40-6/dm-run.telegraph.log \
  --out /tmp/dm-learning/candidate-55-75-40-6/candidate-55-75-40-6 \
  --candidate candidate-55-75-40-6 --seed 1 --parties 8 \
  --heal 55 --tank-heal 75 --dps-mana 40 --los-yards 6
```

The analyzer writes `events.jsonl` in journal order (core sequence numbers are retained) plus `metrics.json` with party outcomes, bot cast results, effective damage/healing, and LoS refusals. The summary keeps the full per-party outcomes; the trace remains suitable for replay and later feature extraction.

## Compare and choose the next experiment

Keep one metrics file per candidate and seed. Rank uses an explicit lexicographic objective—not an opaque weighted score:

1. Mean bosses killed (higher)
2. Completion rate (higher)
3. Mean deaths (lower)
4. Wipe rate (lower)
5. LoS refusals (lower)

```bash
python3 -I tools/dungeon_learning/analyze.py rank /tmp/dm-learning/*/candidate-*/metrics.json
python3 -I tools/dungeon_learning/analyze.py propose /tmp/dm-learning/*/candidate-*/metrics.json
```

`propose` suggests one-knob neighbors of the best candidate within the validated bounds. Re-run the top candidates on at least one held-out seed before adopting anything. Treat the result as a recommendation; review the trace and merge changes manually. Run output stays under `/tmp`, not in the source tree.

## Safety and interpretation

- This harness targets the test sim only; it does not change production or push policy automatically.
- Compare batches with the same party count, duration, roster, dungeon data, and seed. Repeat top candidates on different seeds to reduce luck bias.
- The trace's event order comes from the single journal capture. Core `seq` orders core events; module cast results include `time_ms` and remain in journal order.
- A candidate is incomplete if its `metrics.json` warns that the expected party outcome count was not observed.
- Current batch mode runs one candidate at a time. Eight parties are eight replicates, not eight different policies.
