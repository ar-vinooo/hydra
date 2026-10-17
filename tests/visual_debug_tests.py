"""Offline evidence/report regression cases. Run explicitly; does not launch Hydra."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("visual_debug", Path(__file__).resolve().parents[1] / "tools/visual_debug.py")
visual = importlib.util.module_from_spec(spec)
spec.loader.exec_module(visual)


class EvidenceTests(unittest.TestCase):
    def test_compilation_success_does_not_prove_fix(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory)
            (capture / "events.jsonl").write_text('\n'.join(json.dumps(e) for e in [
                {"event": "capture_begin"},
                {"event": "metal_library_result", "fields": {"library_available": "1"}},
                {"event": "metal_draw_encoded"}, {"event": "capture_end"}]))
            report = visual.diagnose(capture, "black object")
            self.assertEqual(report["confidence"], "insufficient_evidence")
            self.assertIsNone(report["recommended_correction"])
            self.assertEqual(report["root_cause_hypotheses"], [])

    def test_unknown_instruction_is_only_a_hypothesis(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory)
            (capture / "events.jsonl").write_text('{"event":"capture_begin"}\n{"event":"capture_end"}\n')
            (capture / "shader.decoded.txt").write_text('5 raw=123 op=UNKNOWN\n')
            report = visual.diagnose(capture, "broken lighting")
            self.assertEqual(report["root_cause_hypotheses"][0]["subsystem"], "decoder")
            self.assertEqual(report["root_cause_hypotheses"][0]["result"], "not_run")
            self.assertIsNone(report["recommended_correction"])

    def test_ring_history_does_not_contaminate_capture(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory)
            (capture / "events.jsonl").write_text('\n'.join(json.dumps(e) for e in [
                {"event": "capture_begin"}, {"event": "metal_draw_rejected"},
                {"event": "capture_end"}, {"event": "capture_begin"}, {"event": "capture_end"}]))
            report = visual.diagnose(capture, "missing texture")
            self.assertEqual(report["supporting_evidence"], [])

    def test_pending_request_is_not_overwritten(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "request").write_text("action=reset\n")
            with self.assertRaises(ValueError):
                visual.request(path, "action=capture")
            self.assertEqual((path / "request").read_text(), "action=reset\n")


if __name__ == "__main__":
    unittest.main()
