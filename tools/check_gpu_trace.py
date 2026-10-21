"""Check recorded PFIFO method order and cooperative blocking in events.jsonl.

This checks observed scheduling, not hardware compatibility or pixel correctness.
By default a bounded capture window may start after a block or end while blocked.
Use --require-complete for a complete stream that must contain every wakeup.
"""
import argparse
import json
from pathlib import Path
import sys


def check_events(events, require_complete=False):
    last = {}
    blocked = {}
    methods = resumes = blocks = unmatched = 0
    truncated = False

    def number(fields, key):
        raw = fields[key]
        if isinstance(raw, bool) or not isinstance(raw, (str, int)):
            raise ValueError(f"{key} must be an unsigned integer")
        value = int(raw)
        if value < 1 or value > (1 << 64) - 1:
            raise ValueError(f"{key} must be a nonzero uint64")
        return value

    for line, event in enumerate(events, start=1):
        try:
            if not isinstance(event, dict):
                raise ValueError("event must be an object")
            kind = event.get("event")
            if kind == "event_budget_exhausted":
                if require_complete:
                    raise ValueError("Trace exhausted its event budget")
                truncated = True
                continue
            if kind not in {"pfifo_method", "pfifo_blocked", "pfifo_resumed"}:
                continue
            fields = event["fields"]
            channel = number(fields, "channel")
            sequence = number(fields, "sequence")
            if kind == "pfifo_method":
                if channel in blocked:
                    raise ValueError(f"channel {channel} dispatched while blocked at {blocked[channel]}")
                if sequence <= last.get(channel, 0):
                    raise ValueError(f"channel {channel} reordered/duplicated {sequence} after {last[channel]}")
                last[channel] = sequence
                methods += 1
            elif kind == "pfifo_blocked":
                if channel in blocked:
                    raise ValueError(f"channel {channel} blocked twice without resuming")
                if channel in last and sequence != last[channel]:
                    raise ValueError(f"channel {channel} blocked at an unexpected sequence {sequence}")
                if require_complete and channel not in last:
                    raise ValueError(f"channel {channel} block has no preceding method evidence")
                blocked[channel] = sequence
                blocks += 1
            else:
                if channel not in blocked:
                    unmatched += 1
                    if require_complete or channel in last:
                        raise ValueError(f"channel {channel} resumed without a recorded block")
                elif blocked.pop(channel) != sequence:
                    raise ValueError(f"channel {channel} resumed at the wrong sequence")
                # A resumed blocking operation was already consumed. The next
                # method must advance beyond it even at a capture boundary.
                last[channel] = max(last.get(channel, 0), sequence)
                resumes += 1
        except (KeyError, TypeError, ValueError) as error:
            raise ValueError(f"event {line}: {error}") from error
    if methods == 0:
        raise ValueError("Trace contains no PFIFO method evidence")
    if require_complete and blocked:
        raise ValueError(f"Trace ended with blocked channels: {blocked}")
    return {"methods": methods, "channels": len(last), "blocks": blocks,
            "resumes": resumes, "pending_blocks": len(blocked), "unmatched_resumes": unmatched,
            "truncated": truncated}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--require-complete", action="store_true")
    args = parser.parse_args()
    try:
        with args.trace.open(encoding="utf-8") as stream:
            result = check_events((json.loads(line) for line in stream if line.strip()), args.require_complete)
        print(json.dumps({"status": "passed", **result}))
        return 0
    except (ValueError, OSError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
