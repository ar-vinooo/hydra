"""Compare native replay observation hashes against an explicit reference.

Both inputs use hydra-guest-replay's summary schema. Hardware reference data
must be collected separately in that schema; naming it hardware does not
authenticate its origin. No image similarity threshold hides byte differences.
"""
import argparse
import json
from pathlib import Path
import sys


def compare(reference, candidate):
    for name, summary in (("reference", reference), ("candidate", candidate)):
        if not isinstance(summary, dict) or summary.get("schema") != 1:
            raise ValueError(f"{name}: unsupported summary schema")
        if summary.get("status") != "replayed" or summary.get("rendered") is not True:
            raise ValueError(f"{name}: native replay did not succeed")
        if summary.get("mismatches") != 0 or summary.get("guest_methods", 0) <= 0:
            raise ValueError(f"{name}: failed or empty native execution")
        observations = summary.get("observations")
        if not isinstance(observations, list) or not observations:
            raise ValueError(f"{name}: no output observations were compared")
        keys = set()
        for observation in observations:
            if not isinstance(observation, dict) or observation.get("matches") is not True:
                raise ValueError(f"{name}: an observation failed")
            for field in ("order", "address", "size", "fnv1a64"):
                value = observation.get(field)
                if type(value) is not int or not 0 <= value < (1 << 64):
                    raise ValueError(f"{name}: invalid observation {field}")
            key = tuple(observation[field] for field in ("order", "address", "size"))
            if key in keys or observation["size"] == 0:
                raise ValueError(f"{name}: duplicate or empty observation")
            keys.add(key)
    differences = []
    for field in ("guest_methods", "pfifo_words", "events", "decoder"):
        if reference.get(field) != candidate.get(field):
            differences.append({"field": field, "reference": reference.get(field),
                                "candidate": candidate.get(field)})
    def indexed(summary):
        return {tuple(item[key] for key in ("order", "address", "size")): item["fnv1a64"]
                for item in summary["observations"]}
    left, right = indexed(reference), indexed(candidate)
    for key in sorted(left.keys() | right.keys()):
        if left.get(key) != right.get(key):
            differences.append({"order": key[0], "address": key[1], "size": key[2],
                                "reference": left.get(key), "candidate": right.get(key)})
    return {"schema": 1, "status": "different" if differences else "equal",
            "observations": len(left), "differences": differences}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--reference-kind", choices=("native", "hardware"), default="native")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        result = compare(json.loads(args.reference.read_text(encoding="utf-8")),
                         json.loads(args.candidate.read_text(encoding="utf-8")))
        result["reference_kind"] = args.reference_kind
        result["reference_path"] = str(args.reference)
        text = json.dumps(result, indent=2) + "\n"
        if args.output:
            if args.output.resolve() in {args.reference.resolve(), args.candidate.resolve()}:
                raise ValueError("comparison output must differ from its inputs")
            args.output.write_text(text, encoding="utf-8")
        print(text, end="")
        return int(result["status"] != "equal")
    except (OSError, ValueError, TypeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
