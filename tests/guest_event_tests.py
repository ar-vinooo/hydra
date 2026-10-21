"""Guest replay failure cases and ownership invariants. Not a device oracle."""
import copy
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from replay_guest_events import Replay, Memory, export_native
from guest_event_format import MAGIC, KINDS, HEADER, write_record
from compare_gpu_replays import compare


def stream(*events):
    return [{"schema": 1, "order": index + 1, **event}
            for index, event in enumerate(events)]


def minimal():
    return stream({"event": "session_begin", "value": 1},
                  {"event": "channel_switch", "channel": 1, "address": 1},
                  {"event": "pfifo_word", "channel": 1, "address": 4096,
                   "size": 4, "value": (3 << 29) | (1 << 16)},
                  {"event": "pfifo_word", "channel": 1, "address": 4100,
                   "size": 4, "value": 0xb0b5, "auxiliary": 1},
                  {"event": "method", "channel": 1, "sequence": 1,
                   "address": 0, "value": 0xb0b5, "source": 4100, "auxiliary": 0},
                  {"event": "session_end", "value": 1})


def summary():
    return {"schema": 1, "status": "replayed", "rendered": True,
            "decoder": "pfifo", "guest_methods": 1, "pfifo_words": 2,
            "events": 8, "mismatches": 0,
            "observations": [{"order": 7, "address": 4096, "size": 4,
                              "fnv1a64": 12345, "matches": True}]}


class GuestEventTests(unittest.TestCase):
    def test_late_gpu_completion_does_not_replace_partial_cpu_write(self):
        replay = Replay()
        for event in stream(
            {"event": "session_begin", "value": 1},
            {"event": "gpu_write_issued", "address": 4096, "size": 8, "version": 1},
            {"event": "memory_write", "address": 4098, "size": 2, "version": 2, "data": "aabb"},
            {"event": "gpu_complete", "version": 1, "value": 1},
            {"event": "session_end", "value": 1}):
            replay.apply(event)
        self.assertEqual(replay.memory.read(4098, 2), b"\xaa\xbb")
        self.assertIsNone(replay.memory.read(4096, 8))
        self.assertEqual(replay.result()["unknown_bytes"], 6)
        self.assertEqual(replay.memory.intersect(4098, 2)[0].version, 2)

    def test_initial_alias_snapshot_cannot_resurrect_old_data(self):
        memory = Memory()
        memory.overlay(4096, 8, 4, "gpu")
        memory.overlay(4098, 2, 5, "cpu", b"ab")
        memory.overlay(4094, 12, data=b"0" * 12, initial=True)
        self.assertEqual(memory.read(4098, 2), b"ab")
        self.assertIsNone(memory.read(4096, 2))
        self.assertEqual(memory.read(4094, 2), b"00")

    def test_memory_hash_is_independent_of_interval_splitting(self):
        left, right = Memory(), Memory()
        left.overlay(100, 8, data=b"abcdefgh")
        right.overlay(100, 3, data=b"abc")
        right.overlay(103, 5, data=b"defgh")
        self.assertEqual(left.summary(), right.summary())

    def test_native_completion_footer_cannot_hide_pending_submission(self):
        replay = Replay()
        replay.apply({"schema": 1, "order": 1, "event": "session_begin", "value": 1})
        replay.apply({"schema": 1, "order": 2, "event": "native_submit", "version": 1})
        with self.assertRaisesRegex(ValueError, "footer"):
            replay.apply({"schema": 1, "order": 3, "event": "session_end", "value": 1})

    def test_duplicate_native_completion_is_rejected(self):
        replay = Replay()
        for event in stream({"event": "session_begin", "value": 1},
                            {"event": "native_submit", "version": 1},
                            {"event": "native_complete", "version": 1, "value": 1}):
            replay.apply(event)
        with self.assertRaisesRegex(ValueError, "no pending"):
            replay.apply({"schema": 1, "order": 4, "event": "native_complete", "version": 1, "value": 1})

    def test_semaphore_wrap_and_mask(self):
        self.assertTrue(Replay.satisfied(0, 0xFFFFFFFF, 4))
        self.assertFalse(Replay.satisfied(0xFFFFFFFF, 0, 4))
        self.assertTrue(Replay.satisfied(0x10, 0x30, 8))
        self.assertFalse(Replay.satisfied(0x10, 0x20, 8))

    def test_method_cannot_overtake_wait(self):
        replay = Replay()
        for event in stream({"event": "session_begin", "value": 1},
                            {"event": "channel_switch", "channel": 1, "address": 1},
                            {"event": "method", "channel": 1, "sequence": 1},
                            {"event": "semaphore_wait", "channel": 1, "sequence": 1,
                             "address": 4096, "value": 1, "auxiliary": 1}):
            replay.apply(event)
        with self.assertRaisesRegex(ValueError, "blocked"):
            replay.apply({"schema": 1, "order": 5, "event": "method", "channel": 1, "sequence": 2})

    def test_binary_transport_preserves_unsigned_fields_and_payload(self):
        target = io.BytesIO()
        write_record(target, {"event": "memory_write", "order": 9, "address": (1 << 63) + 3,
                              "size": 2, "version": 7, "data": "abff"})
        raw = target.getvalue()
        fields = HEADER.unpack(raw[:HEADER.size])
        self.assertEqual(fields[:2], (KINDS["memory_write"], 2))
        self.assertEqual(fields[6], (1 << 63) + 3)
        self.assertEqual(raw[HEADER.size:], b"\xab\xff")

    def test_native_export_rejects_incomplete_source_without_replacing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            source, target = Path(directory) / "events.jsonl", Path(directory) / "events.hge"
            source.write_text(json.dumps({"schema": 1, "order": 1, "event": "session_begin", "value": 1}) + "\n")
            target.write_bytes(b"existing")
            with self.assertRaisesRegex(ValueError, "complete"):
                export_native(source, target)
            self.assertEqual(target.read_bytes(), b"existing")
            self.assertEqual(len(list(Path(directory).iterdir())), 2)

    def test_export_writes_versioned_binary_header(self):
        with tempfile.TemporaryDirectory() as directory:
            source, target = Path(directory) / "events.jsonl", Path(directory) / "events.hge"
            source.write_text("".join(json.dumps(event) + "\n" for event in minimal()))
            export_native(source, target)
            self.assertTrue(target.read_bytes().startswith(MAGIC))

    def test_differential_reports_one_byte_hash_change_and_missing_observation(self):
        reference, candidate = summary(), summary()
        self.assertEqual(compare(reference, candidate)["status"], "equal")
        candidate["observations"][0]["fnv1a64"] ^= 1
        self.assertEqual(len(compare(reference, candidate)["differences"]), 1)
        candidate["observations"] = []
        with self.assertRaisesRegex(ValueError, "no output"):
            compare(reference, candidate)

    def test_differential_rejects_recorded_failure(self):
        candidate = copy.deepcopy(summary())
        candidate["observations"][0]["matches"] = False
        with self.assertRaisesRegex(ValueError, "failed"):
            compare(summary(), candidate)


if __name__ == "__main__":
    unittest.main()
