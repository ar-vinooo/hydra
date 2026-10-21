import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("check_gpu_trace", Path(__file__).resolve().parents[1] / "tools/check_gpu_trace.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def event(kind, channel, sequence):
    return {"event": f"pfifo_{kind}", "fields": {"channel": str(channel), "sequence": str(sequence)}}


class TraceTests(unittest.TestCase):
    def test_producer_runs_while_consumer_is_blocked(self):
        trace = [event("method", 1, 5), event("blocked", 1, 5),
                 event("method", 2, 1), event("method", 2, 2),
                 event("resumed", 1, 5), event("method", 1, 6)]
        self.assertEqual(checker.check_events(trace, True)["methods"], 4)

    def test_blocked_channel_cannot_be_overtaken(self):
        trace = [event("method", 1, 1), event("blocked", 1, 1), event("method", 1, 2)]
        with self.assertRaisesRegex(ValueError, "while blocked"):
            checker.check_events(trace)

    def test_duplicate_and_reversed_sequences_fail(self):
        for sequence in [1, 2]:
            with self.assertRaisesRegex(ValueError, "reordered/duplicated"):
                checker.check_events([event("method", 1, 2), event("method", 1, sequence)])

    def test_capture_boundaries_are_explicit(self):
        trace = [event("resumed", 1, 4), event("method", 1, 5), event("blocked", 1, 5)]
        result = checker.check_events(trace)
        self.assertEqual((result["unmatched_resumes"], result["pending_blocks"]), (1, 1))
        with self.assertRaises(ValueError):
            checker.check_events(trace, True)
        with self.assertRaisesRegex(ValueError, "ended with blocked"):
            checker.check_events([event("method", 1, 1), event("blocked", 1, 1)], True)

    def test_wrong_wakeup_fails(self):
        with self.assertRaisesRegex(ValueError, "wrong sequence"):
            checker.check_events([event("method", 1, 1), event("blocked", 1, 1), event("resumed", 1, 2)])

    def test_bad_or_missing_evidence_fails(self):
        for trace in [[], [{"event": "draw"}], [event("method", 0, 1)],
                      [event("method", 1, -1)], [{"event": "pfifo_method", "fields": {}}]]:
            with self.assertRaises(ValueError):
                checker.check_events(trace)
        budget = [event("method", 1, 1), {"event": "event_budget_exhausted"}]
        self.assertTrue(checker.check_events(budget)["truncated"])
        with self.assertRaisesRegex(ValueError, "event budget"):
            checker.check_events(budget, True)

    def test_independent_sequences_allow_filtered_gaps(self):
        result = checker.check_events([event("method", 1, 1), event("method", 2, 1), event("method", 1, 9)])
        self.assertEqual(result["channels"], 2)


if __name__ == "__main__":
    unittest.main()
