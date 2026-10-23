#!/usr/bin/env python3
"""Small standard-library checks for the offline dungeon learning analyzer."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

MODULE_PATH = Path(__file__).resolve().parents[1] / "tools" / "dungeon_learning" / "analyze.py"
spec = importlib.util.spec_from_file_location("dungeon_learning_analyze", MODULE_PATH)
learning = importlib.util.module_from_spec(spec)
spec.loader.exec_module(learning)


class TraceParserTest(unittest.TestCase):
    def test_core_event_preserves_sequence_and_multiword_names(self):
        line = ("Oct 23 14:00:00 forever-sim[12]: dungeon_telegraph seq=44 time_ms=9000 event=damage "
                "instance=7 attacker=120 attacker_name=Edwin VanCleef attacker_entry=639 victim=900 "
                "victim_name=Test Bot victim_entry=0 spell=0 amount=50 effective=40 victim_hp_before=100")
        event = learning.parse_trace_line(line, 3)
        self.assertEqual(event["ordinal"], 3)
        self.assertEqual(event["seq"], 44)
        self.assertEqual(event["attacker_name"], "Edwin VanCleef")
        self.assertEqual(event["effective"], 40)

    def test_cast_result_counts_line_of_sight_refusal(self):
        line = ("Oct 23 14:00:00 forever-sim[12]: forever-bots: event cast_result: time_ms=9001 "
                "bot=900 spell=8092 target=639 status=no line of sight hp=400/800 power=100/500")
        event = learning.parse_trace_line(line, 4)
        metrics = learning.summarize_events([event], {900})
        self.assertEqual(metrics["los_refusals"], 1)
        self.assertEqual(metrics["cast_result_statuses"], {"no line of sight": 1})

    def test_rank_is_outcome_first_and_proposal_stays_bounded(self):
        params = {"heal": 55, "tank_heal": 75, "dps_mana": 40, "los_yards": 6}
        base = {"candidate": "base", "params": params, "party_count": 8, "expected_parties": 8, "warnings": [],
                "aggregate": {"mean_bosses": 2.0, "complete_rate": 0.0, "mean_deaths": 4.0, "wipe_rate": 0.0},
                "combat": {"los_refusals": 5}}
        safer = {"candidate": "safer", "params": params, "party_count": 8, "expected_parties": 8, "warnings": [],
                 "aggregate": {"mean_bosses": 1.9, "complete_rate": 0.0, "mean_deaths": 0.0, "wipe_rate": 0.0},
                 "combat": {"los_refusals": 0}}
        self.assertGreater(learning.rank_key(base), learning.rank_key(safer))
        proposals = learning.propose([base])
        self.assertTrue(proposals)
        self.assertTrue(all(30 <= p["params"]["heal"] <= 85 for p in proposals))
        self.assertTrue(all(40 <= p["params"]["tank_heal"] <= 95 for p in proposals))

    def test_analyze_writes_party_metrics_and_ordered_trace(self):
        summary = ("Oct 23 14:00:00 forever-sim[12]: forever-bots: scenario bot 900 race 1 class 5\n"
                   "Oct 23 14:10:00 forever-sim[12]: forever-bots: scenario dm: outcome=timeout furthest=2/3 deaths=1 "
                   "alive=4/5 events=1 revived=0 wipes=0 ghosts=0 run=600s party=yes bosses=2/7 killed=One,Two\n")
        trace = ("Oct 23 14:01:00 forever-sim[12]: dungeon_telegraph seq=1 time_ms=100 event=heal instance=7 "
                 "healer=900 healer_name=Healer healer_entry=0 target=901 target_name=Tank target_entry=0 spell=2050 attempted=50 effective=40 target_hp_before=100\n")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "summary.log").write_text(summary, encoding="utf-8")
            (root / "trace.log").write_text(trace, encoding="utf-8")
            metric = learning.analyze(root / "summary.log", root / "trace.log", root / "out", "x", 1, 1,
                                      {"heal": 55, "tank_heal": 75, "dps_mana": 40, "los_yards": 6})
            self.assertEqual(metric["party_count"], 1)
            self.assertEqual(metric["aggregate"]["mean_bosses"], 2)
            self.assertEqual(metric["combat"]["effective_healing"], 40)
            self.assertEqual((root / "out" / "events.jsonl").read_text().count("\n"), 1)


if __name__ == "__main__":
    unittest.main()
