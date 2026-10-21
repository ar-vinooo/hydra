"""Replay Hydra's guest event timeline, memory observations and ownership.

This is a semantic/event replay, not a Maxwell shader interpreter or Metal
renderer. Use .hcap replay for rendered commands. Unknown GPU-produced bytes
stay unknown until a recorded writeback/publication supplies them.
"""
import argparse
from collections import Counter
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import sys
import os
import tempfile

from guest_event_format import MAGIC, write_record

U64 = (1 << 64) - 1


@dataclass
class Segment:
    begin: int
    end: int
    version: int
    writer: str
    data: bytes | None

    def slice(self, begin, end):
        data = None if self.data is None else self.data[begin - self.begin:end - self.begin]
        return Segment(begin, end, self.version, self.writer, data)


class Memory:
    """Disjoint physical ranges: all GPU mappings share these same bytes."""
    def __init__(self):
        self.segments = []

    def overlay(self, begin, size, version=0, writer="cpu", data=None, initial=False, observe=False):
        end = begin + size
        if size == 0:
            return
        next_segments = []
        cursor = begin
        for segment in self.segments:
            if segment.end <= begin or segment.begin >= end:
                next_segments.append(segment)
                continue
            left = max(begin, segment.begin)
            right = min(end, segment.end)
            if segment.begin < left:
                next_segments.append(segment.slice(segment.begin, left))
            if cursor < left:
                chunk = None if data is None else data[cursor - begin:left - begin]
                next_segments.append(Segment(cursor, left, version, writer, chunk))
            if initial or (not observe and segment.version > version):
                next_segments.append(segment.slice(left, right))
            else:
                chunk = None if data is None else data[left - begin:right - begin]
                next_segments.append(Segment(left, right,
                    segment.version if observe else version,
                    segment.writer if observe else writer, chunk))
            cursor = right
            if right < segment.end:
                next_segments.append(segment.slice(right, segment.end))
        if cursor < end:
            chunk = None if data is None else data[cursor - begin:end - begin]
            next_segments.append(Segment(cursor, end, version, writer, chunk))
        self.segments = sorted(next_segments, key=lambda segment: segment.begin)

    def intersect(self, begin, size):
        end = begin + size
        return [segment for segment in self.segments if segment.begin < end and begin < segment.end]

    def read(self, begin, size):
        cursor = begin
        end = begin + size
        result = bytearray()
        for segment in self.intersect(begin, size):
            if segment.begin > cursor or segment.data is None:
                return None
            right = min(end, segment.end)
            result.extend(segment.data[cursor - segment.begin:right - segment.begin])
            cursor = right
        return bytes(result) if cursor == end else None

    def summary(self):
        # Hash includes addresses and lengths; holes/unknown bytes cannot be
        # confused with known zero-filled memory or concatenated aliases.
        digest = hashlib.sha256()
        known = unknown = 0
        runs = []
        for segment in self.segments:
            if runs and runs[-1][-1].end == segment.begin and (
                    runs[-1][-1].data is None) == (segment.data is None):
                runs[-1].append(segment)
            else:
                runs.append([segment])
        for run in runs:
            size = run[-1].end - run[0].begin
            digest.update(run[0].begin.to_bytes(8, "little"))
            digest.update(size.to_bytes(8, "little"))
            digest.update(b"?" if run[0].data is None else b"=")
            if run[0].data is None:
                unknown += size
            else:
                known += size
                for segment in run:
                    digest.update(segment.data)
        return {"known_bytes": known, "unknown_bytes": unknown,
                "memory_observation_sha256": digest.hexdigest()}


class Replay:
    def __init__(self):
        self.memory = Memory()
        self.order = 0
        self.started = self.ended = self.truncated = False
        self.spaces = {}
        self.channels = {}
        self.closed_channels = set()
        self.destroyed_spaces = set()
        self.last_sequence = {}
        self.waits = {}
        self.gpu = {}
        self.reports = {}
        self.counts = Counter()
        self.stale_reads = []
        self.dependencies = set()
        self.faults = {}
        self.unknown_semaphore_reads = 0
        self.syncpoints = {}
        self.native = {}

    @staticmethod
    def integer(event, key, default=0):
        value = event.get(key, default)
        if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= U64:
            raise ValueError(f"{key} is not a uint64")
        return value

    @staticmethod
    def satisfied(word, expected, operation):
        if word > 0xFFFFFFFF or expected > 0xFFFFFFFF:
            raise ValueError("semaphore operands exceed their 32-bit width")
        word &= 0xFFFFFFFF
        expected &= 0xFFFFFFFF
        if operation == 1:
            return word == expected
        if operation == 4:
            return ((word - expected) & 0xFFFFFFFF) < 0x80000000
        if operation == 8:
            return bool(word & expected)
        raise ValueError(f"unsupported semaphore comparison {operation}")

    def translate(self, channel, address, size):
        space = self.spaces.get(self.channels.get(channel), {})
        for base, (host, length, _guest) in sorted(space.items()):
            if base <= address and size <= length and address - base <= length - size:
                return host + address - base if host else None
        return None

    def apply(self, event):
        if not isinstance(event, dict) or type(event.get("schema")) is not int or event.get("schema") != 1:
            raise ValueError("unsupported guest-event schema")
        order = self.integer(event, "order")
        if order != self.order + 1:
            raise ValueError(f"event order {order} does not follow {self.order}")
        if self.ended or self.truncated:
            raise ValueError("event follows a terminal record")
        self.order = order
        kind = event.get("event")
        if not isinstance(kind, str):
            raise ValueError("event name is missing")
        if not self.started and kind != "session_begin":
            raise ValueError("capture must begin with session_begin")
        channel = self.integer(event, "channel")
        sequence = self.integer(event, "sequence")
        address = self.integer(event, "address")
        size = self.integer(event, "size")
        version = self.integer(event, "version")
        value = self.integer(event, "value")
        auxiliary = self.integer(event, "auxiliary")
        self.integer(event, "source")
        if size > U64 - address:
            raise ValueError("memory interval overflows uint64")
        self.counts[kind] += 1

        if kind == "session_begin":
            if self.started or value != 1:
                raise ValueError("duplicate or unsupported session header")
            self.started = True
        elif kind == "truncated":
            self.truncated = True
        elif kind == "session_end":
            pending = sum(status == "pending" for status in self.gpu.values())
            reports = sum(status == "pending" for status in self.reports.values())
            native = sum(status == "pending" for status in self.native.values())
            if size != pending or auxiliary != reports or version != native or value != int(pending == 0 and reports == 0 and native == 0):
                raise ValueError("footer does not match pending GPU epochs")
            self.ended = True
        elif kind == "native_submit":
            if version == 0 or version in self.native:
                raise ValueError("invalid or duplicate native submission")
            self.native[version] = "pending"
        elif kind == "native_complete":
            if self.native.get(version) != "pending" or value not in {0, 1}:
                raise ValueError("native completion has no pending submission")
            self.native[version] = "completed" if value else "failed"
        elif kind == "syncpoint_initial":
            if not 0 < address < 192 or value > 0xFFFFFFFF or address in self.syncpoints:
                raise ValueError("invalid or duplicate initial syncpoint")
            self.syncpoints[address] = value
        elif kind == "map":
            if version == 0 or version in self.destroyed_spaces or size == 0 or value > U64 - size:
                raise ValueError("invalid mapped backing range")
            self.spaces.setdefault(version, {})[address] = (value, size, auxiliary)
        elif kind == "unmap":
            mapping = self.spaces.setdefault(version, {}).pop(address, None)
            if mapping is None or mapping[1] != size:
                raise ValueError("unmap does not match a recorded mapping")
        elif kind == "channel_switch":
            if channel == 0 or channel in self.closed_channels or address == 0 or address in self.destroyed_spaces:
                raise ValueError("channel and address-space IDs must be nonzero")
            if channel in self.channels and self.channels[channel] != address:
                raise ValueError("channel changed its bound address space")
            self.channels[channel] = address
        elif kind == "channel_destroy":
            if channel == 0 or channel in self.closed_channels:
                raise ValueError("invalid or duplicate channel destruction")
            self.closed_channels.add(channel)
            self.waits = {key: wait for key, wait in self.waits.items() if key[0] != channel}
        elif kind == "address_space_destroy":
            if address == 0 or address in self.destroyed_spaces:
                raise ValueError("invalid or duplicate address-space destruction")
            if self.spaces.get(address):
                raise ValueError("address space was destroyed with live mappings")
            self.destroyed_spaces.add(address)
        elif kind == "method":
            if channel not in self.channels or sequence <= self.last_sequence.get(channel, 0):
                raise ValueError("method has no bound channel or reorders its sequence")
            if channel in self.closed_channels or channel in self.faults or any(key[0] == channel for key in self.waits):
                raise ValueError("method overtook a blocked or faulted channel")
            if auxiliary >= 8 or value > 0xFFFFFFFF:
                raise ValueError("invalid method subchannel or argument")
            self.last_sequence[channel] = sequence
        elif kind in {"semaphore_wait", "syncpoint_wait"}:
            key = (channel, kind)
            if sequence != self.last_sequence.get(channel) or key in self.waits:
                raise ValueError("wait does not identify the last dispatched method")
            self.waits[key] = (sequence, address, value, auxiliary)
        elif kind == "semaphore_acquired":
            if not self.satisfied(value, version, auxiliary):
                raise ValueError("acquire succeeded with an unsatisfied predicate")
            host = self.translate(channel, address, 4)
            observed = None if host is None else self.memory.read(host, 4)
            if observed is None:
                self.unknown_semaphore_reads += 1
            elif int.from_bytes(observed, "little") != value:
                raise ValueError("memory does not reproduce the successful acquire")
        elif kind in {"semaphore_resume", "syncpoint_resume"}:
            key = (channel, kind.replace("resume", "wait"))
            waited = self.waits.pop(key, None)
            if waited is None or waited[:3] != (sequence, address, version):
                raise ValueError("resume does not match a recorded dependency")
            if kind == "syncpoint_resume":
                if value > 0xFFFFFFFF or version > 0xFFFFFFFF:
                    raise ValueError("syncpoint operands exceed their 32-bit width")
                if ((value - version) & 0xFFFFFFFF) >= 0x80000000:
                    raise ValueError("syncpoint resumed before its threshold")
            else:
                if auxiliary != waited[3] or not self.satisfied(value, version, auxiliary):
                    raise ValueError("semaphore resumed with an unsatisfied predicate")
                host = self.translate(channel, address, 4)
                data = None if host is None else self.memory.read(host, 4)
                if data is None:
                    self.unknown_semaphore_reads += 1
                elif int.from_bytes(data, "little") != value:
                    raise ValueError("recorded memory does not reproduce the observed acquire value")
        elif kind in {"memory_initial", "memory_write", "memory_published", "memory_writeback"}:
            if size == 0 or size > 64 * 1024:
                raise ValueError("memory payload exceeds the recorder chunk size")
            encoded = event.get("data")
            if not isinstance(encoded, str) or len(encoded) != size * 2:
                raise ValueError("memory payload length differs from its range")
            data = bytes.fromhex(encoded)
            if len(data) != size:
                raise ValueError("decoded memory payload has the wrong byte length")
            if kind == "memory_write" and version == 0:
                raise ValueError("memory write lacks a content epoch")
            self.memory.overlay(address, size, version, "cpu", data,
                initial=kind == "memory_initial", observe=kind in {"memory_published", "memory_writeback"})
        elif kind == "gpu_write_issued":
            if version == 0 or size == 0 or self.gpu.get(version, "pending") != "pending":
                raise ValueError("GPU epoch was reused after completion")
            self.gpu[version] = "pending"
            self.memory.overlay(address, size, version, "gpu")
        elif kind == "gpu_complete":
            if self.gpu.get(version) != "pending" or value not in {0, 1}:
                raise ValueError("GPU completion has no unique pending producer")
            # Completion changes only the token. It never restamps an interval
            # or resurrects ownership after a newer CPU/GPU overwrite.
            self.gpu[version] = "completed" if value else "failed"
        elif kind == "report_queued":
            if version == 0 or version in self.reports:
                raise ValueError("duplicate or invalid report token")
            self.reports[version] = "pending"
        elif kind == "report_complete":
            if self.reports.get(version) != "pending" or value not in {0, 1}:
                raise ValueError("report completion has no pending publisher")
            self.reports[version] = "completed" if value else "failed"
        elif kind == "resource_read":
            for segment in self.memory.intersect(address, size):
                if segment.version > version:
                    self.stale_reads.append({"order": order, "channel": channel,
                        "sequence": sequence, "prepared": version, "current": segment.version})
                if segment.writer == "gpu":
                    self.dependencies.add((segment.version, channel, sequence))
        elif kind in {"draw", "clear", "blit", "compute", "dma_access", "dma_copy", "macro_flush"}:
            if channel and (channel in self.faults or any(key[0] == channel for key in self.waits)):
                raise ValueError("operation overtook a blocked/faulted channel")
        elif kind == "channel_fault":
            if channel == 0:
                raise ValueError("fault has no channel identity")
            self.faults[channel] = value
            self.waits = {key: wait for key, wait in self.waits.items() if key[0] != channel}
        elif kind in {"checkpoint", "resource_write", "resource_invalidate", "pfifo_entry", "pfifo_word"}:
            pass
        elif kind in {"semaphore_acquire", "semaphore_release", "semaphore_release_queued"}:
            if channel in self.faults or any(key[0] == channel for key in self.waits):
                raise ValueError("semaphore operation overtook a blocked/faulted channel")
        else:
            raise ValueError(f"unsupported event {kind!r}")

    def result(self):
        pending = sum(status == "pending" for status in self.gpu.values())
        reports = sum(status == "pending" for status in self.reports.values())
        native = sum(status == "pending" for status in self.native.values())
        complete = self.started and self.ended and not self.truncated and not pending and not reports and not native and not self.waits
        return {"schema": 1, "status": "replayed" if complete else "incomplete",
                "rendered": False, "events": self.order, "counts": dict(self.counts),
                "guest_methods": self.counts["method"],
                "channels": len(self.channels), "address_spaces": len(self.spaces),
                "gpu_pending": pending, "gpu_failed": sum(status == "failed" for status in self.gpu.values()),
                "reports_pending": reports, "reports_failed": sum(status == "failed" for status in self.reports.values()),
                "native_pending": native, "native_failed": sum(status == "failed" for status in self.native.values()),
                "blocked_channels": len({key[0] for key in self.waits}),
                "channel_faults": self.faults, "stale_reads": self.stale_reads,
                "dependency_edges": len(self.dependencies),
                "unknown_semaphore_reads": self.unknown_semaphore_reads,
                **self.memory.summary()}


def export_native(capture, destination):
    """Export only a complete stream, using an atomic destination replacement.

    Re-read and check every record during export so a changed source cannot
    bypass structural checks made by the earlier inspection pass.
    """
    if capture.resolve() == destination.resolve():
        raise ValueError("native output must differ from the source capture")
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="wb", dir=destination.parent,
                                         prefix=destination.name + ".", delete=False) as target:
            temporary = Path(target.name)
            target.write(MAGIC)
            replay = Replay()
            with capture.open(encoding="utf-8") as source:
                for line in source:
                    if line.strip():
                        event = json.loads(line)
                        replay.apply(event)
                        write_record(target, event)
            result = replay.result()
            if result["status"] != "replayed" or result["channel_faults"] or result["gpu_failed"] or result["reports_failed"] or result["native_failed"]:
                raise ValueError("native replay requires a complete stream without recorded failures")
            if result["guest_methods"] == 0 or replay.counts["pfifo_word"] == 0:
                raise ValueError("native replay requires captured PFIFO words and methods")
            target.flush()
            os.fsync(target.fileno())
        os.replace(temporary, destination)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--output", type=Path, help="Write the reconstructed-state summary as JSON")
    parser.add_argument("--allow-incomplete", action="store_true", help="Allow inspection of a capture prefix")
    parser.add_argument("--strict", action="store_true", help="Reject stale reads, recorded failures and unresolved acquires")
    parser.add_argument("--native-output", type=Path, help="Export a complete stream for hydra-guest-replay")
    args = parser.parse_args()
    try:
        outputs = [path.resolve() for path in (args.output, args.native_output) if path]
        if args.capture.resolve() in outputs or len(set(outputs)) != len(outputs):
            raise ValueError("capture and output paths must all differ")
        replay = Replay()
        with args.capture.open(encoding="utf-8") as source:
            for line_number, line in enumerate(source, 1):
                if not line.strip():
                    continue
                try:
                    replay.apply(json.loads(line))
                except (ValueError, TypeError, KeyError) as error:
                    raise ValueError(f"line {line_number}: {error}") from error
        result = replay.result()
        strict_failed = bool(result["stale_reads"] or result["gpu_failed"] or
                             result["reports_failed"] or result["native_failed"] or
                             result["channel_faults"] or result["unknown_semaphore_reads"] or
                             result["guest_methods"] == 0)
        if args.native_output:
            if args.strict and strict_failed:
                raise ValueError("strict replay checks failed; native export was not written")
            export_native(args.capture, args.native_output)
        text = json.dumps(result, indent=2) + "\n"
        if args.output:
            args.output.write_text(text, encoding="utf-8")
        print(text, end="")
        if result["status"] != "replayed" and not args.allow_incomplete:
            return 1
        if args.strict and strict_failed:
            return 1
        return 0
    except (ValueError, OSError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
