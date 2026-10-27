#!/usr/bin/env python3
"""Bounded Hydra visual captures and evidence reports; never builds or launches games."""
import argparse
from collections import Counter
import json
from pathlib import Path
import time

ROOT = Path(__file__).resolve().parents[1]
SOURCE_FILES = {
    "decoder": "src/core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/decoder.cpp",
    "ir": "src/core/hw/tegra_x1/gpu/renderer/shader_decompiler/decompiler.cpp",
    "msl": "src/core/hw/tegra_x1/gpu/renderer/shader_decompiler/codegen/lang/msl/emitter.cpp",
    "bindings": "src/core/hw/tegra_x1/gpu/engines/3d.cpp",
    "textures": "src/core/hw/tegra_x1/gpu/renderer/texture_cache.cpp",
    "pipeline": "src/core/hw/tegra_x1/gpu/renderer/metal/pipeline.cpp",
}


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2) + "\n")


def read_events(capture):
    result = []
    with (capture / "events.jsonl").open() as source:
        for index, line in enumerate(source, 1):
            try:
                result.append(json.loads(line))
            except json.JSONDecodeError as error:
                raise ValueError(f"invalid/incomplete JSON at line {index}; wait for capture to finish") from error
    # The first entries are ring history, potentially including earlier captures.
    starts = [i for i, item in enumerate(result) if item.get("event") == "capture_begin"]
    result = result[starts[-1]:] if starts else result
    submissions = {e.get("submission") for e in result if e.get("event") in ("draw_begin", "gpu_submit")}
    result = [e for e in result if e.get("event") != "gpu_completed" or e.get("submission") in submissions]
    completions = capture / "completions.jsonl"
    if completions.exists():
        existing = {e.get("submission") for e in result if e.get("event") == "gpu_completed"}
        for line in completions.read_text().splitlines():
            event = json.loads(line)
            if event.get("submission") not in existing:
                result.append(event)
                existing.add(event.get("submission"))
    return result


def request(directory, options):
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / "request"
    if path.exists():
        raise ValueError("a request is still pending; wait until Hydra consumes it")
    temporary = directory / f".request-{time.time_ns()}"
    temporary.write_text(options + "\n")
    temporary.replace(path)
    print(f"Queued: {options}\nOutput: {directory}/capture-*/events.jsonl")


def diagnose(capture, symptom):
    events = read_events(capture)
    counts = Counter(item.get("event") for item in events)
    hypotheses = []
    evidence = []
    def hypothesis(subsystem, statement, support, experiment):
        if len(hypotheses) < 3:
            hypotheses.append({"subsystem": subsystem, "hypothesis": statement,
                               "supporting_evidence": support, "diagnostic_experiment": experiment,
                               "result": "not_run", "confidence": "low"})
    unknown = []
    for path in capture.glob("*.decoded.txt"):
        unknown += [f"{path.name}: {line}" for line in path.read_text().splitlines() if "UNKNOWN" in line][:8]
    if unknown:
        evidence += unknown[:8]
        hypothesis("decoder", "Unsupported guest instructions may affect the selected draw.", unknown[:8],
                   "Inspect matching PC and IR; implement one instruction only after confirming its Maxwell semantics.")
    for item in events:
        kind, fields = item.get("event"), item.get("fields", {})
        if kind in ("metal_draw_rejected", "shader_replay_failed", "texture_binding_unavailable", "texture_descriptor_unmapped", "unsupported_texture_alias"):
            evidence.append(item)
        if kind == "gpu_completed" and fields.get("status") != "completed":
            evidence.append(item)
        if kind == "shader_replay" and fields.get("matches_bound_source") == "0":
            evidence.append(item)
            hypothesis("msl", "Bound MSL differs from a replay of the current shader inputs.", [item],
                       "Check active MSL overrides and guest/specialization changes before changing the decoder.")
    unavailable = [e for e in evidence if isinstance(e, dict) and e.get("event") in
                   ("texture_binding_unavailable", "texture_descriptor_unmapped")]
    if unavailable:
        hypothesis("bindings", "An unavailable descriptor may explain missing sampled data.", unavailable[:4],
                   "Capture the same draw with shaders=1 and inspect the reported guest/host binding and TIC/TSC.")
    rejected = [e for e in evidence if isinstance(e, dict) and e.get("event") == "metal_draw_rejected"]
    if rejected:
        hypothesis("pipeline", "The selected draw could not be encoded with its bound pipeline/pass.", rejected[:4],
                   "Inspect metal_library_result, metal_entry_point and metal_pipeline_result for the same source identity.")
    alias_failures = [e for e in evidence if isinstance(e, dict) and e.get("event") == "unsupported_texture_alias"]
    if alias_failures:
        hypothesis("textures", "A sampled resource has an unsupported GPU alias synchronization path.", alias_failures[:4],
                   "Identify its contribution with one selected-draw experiment; warnings alone do not prove the visible root cause.")
    draws = [{k: e.get(k) for k in ("submission", "present_epoch", "draw")} for e in events
             if e.get("event") == "draw_state"]
    images = [{"path": e["fields"]["path"], "exists": Path(e["fields"]["path"]).is_file()}
              for e in events if e.get("event") == "image_requested"]
    report = {
        "schema": 1, "kind": "diagnostic_evidence", "visual_symptom": symptom,
        "capture": str(capture.resolve()), "capture_finished": bool(counts["capture_end"]),
        "affected_draws": draws, "relevant_shaders_or_resources": [e for e in events if e.get("event") in
            ("shader_snapshot", "bound_shader", "texture_binding", "render_target", "draw_pipeline")],
        "suspected_subsystems": sorted({h["subsystem"] for h in hypotheses}),
        "root_cause_hypotheses": hypotheses, "supporting_evidence": evidence,
        "diagnostic_experiments": [h["diagnostic_experiment"] for h in hypotheses],
        "experiment_results": [], "candidate_source_files": {h["subsystem"]: SOURCE_FILES[h["subsystem"]] for h in hypotheses},
        "recommended_correction": None, "confidence": "insufficient_evidence", "images": images,
        "uncertainty": ["No visual reference or same-game-state equivalence has been established.",
                        "Successful Metal compilation/encoding does not validate guest shader semantics.",
                        "Present epochs are CPU presentation observations, not exact guest frame boundaries.",
                        "Image requests may be omitted by format/size limits; existence is checked separately."],
        "next_targeted_capture": "Select one reported draw or shader, capture images=1 shaders=1, then run one intervention against a baseline at the same game state.",
        "event_counts": dict(counts),
    }
    write_json(capture / "diagnosis.json", report)
    return report


def compare_images(before, after, output):
    try:
        from PIL import Image, ImageChops, ImageStat
    except ImportError as error:
        raise ValueError("Image comparison requires Pillow; install it in your chosen Python environment") from error
    with Image.open(before) as a, Image.open(after) as b:
        a, b = a.convert("RGBA"), b.convert("RGBA")
        if a.size != b.size:
            raise ValueError("image sizes differ; no resizing is performed")
        delta = ImageChops.difference(a, b)
        delta.save(output)
        stats = ImageStat.Stat(delta)
        # Count all channels; alpha-only differences remain visible in the report.
        changed = sum(any(pixel) for pixel in delta.getdata())
        return {"before": str(before), "after": str(after), "diff": str(output),
                "changed_pixels": changed, "total_pixels": a.width * a.height,
                "mean_absolute_channel_delta": stats.mean,
                "conclusion": "difference_measured_only", "production_fix_validated": False}


def compare_captures(baseline, candidate, output, same_game_state):
    before, after = read_events(baseline), read_events(candidate)
    kinds = {"draw_state", "depth_stencil_raster", "index_state", "viewport_scissor",
             "blend_state", "render_target", "bound_shader", "uniform_binding", "texture_binding", "sampler_binding"}
    def states(events):
        result = []
        for event in events:
            if event.get("event") in kinds:
                fields = {k: v for k, v in event.get("fields", {}).items() if k not in ("object", "host_address")}
                result.append({"event": event["event"], "fields": fields})
        return result
    a, b = states(before), states(after)
    differences = [{"index": i, "before": a[i] if i < len(a) else None,
                    "after": b[i] if i < len(b) else None}
                   for i in range(max(len(a), len(b)))
                   if (a[i] if i < len(a) else None) != (b[i] if i < len(b) else None)]
    report = {"schema": 1, "kind": "capture_comparison", "baseline": str(baseline), "candidate": str(candidate),
              "same_game_state_acknowledged_by_operator": same_game_state,
              "baseline_finished": any(e.get("event") == "capture_end" for e in before),
              "candidate_finished": any(e.get("event") == "capture_end" for e in after),
              "state_differences": differences,
              "candidate_gpu_failures": [e for e in after if e.get("event") == "gpu_completed" and e.get("fields", {}).get("status") != "completed"],
              "candidate_draw_rejections": [e for e in after if e.get("event") == "metal_draw_rejected"],
              "candidate_gpu_seconds": [e.get("fields", {}).get("gpu_seconds") for e in after if e.get("event") == "gpu_completed"],
              "production_fix_validated": False,
              "uncertainty": ["State entries are compared in capture order; verify draw signatures if submission order changes.",
                              "GPU completion can arrive after capture_end; rerun after completion files stop changing.",
                              "GPU timings include diagnostic overhead; missing completion is not success.",
                              "Visual reference, shader behavior and cross-game regression review remain required."]}
    write_json(output, report)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, default=ROOT / "build/swiftui-release/hot-debug/trace/visual")
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("discover", "capture", "experiment", "reset", "status"):
        cmd = sub.add_parser(name)
        if name in ("capture", "experiment"):
            cmd.add_argument("--draw", type=lambda s: int(s, 0))
            cmd.add_argument("--shader", type=lambda s: int(s, 0))
            cmd.add_argument("--resource", type=lambda s: int(s, 0))
            cmd.add_argument("--frames", type=int, choices=range(1, 9), default=2)
        if name == "experiment":
            cmd.add_argument("--change", required=True, choices=("skip", "only", "solid", "uv", "normal", "checker", "no_depth", "no_cull", "no_blend"))
            cmd.add_argument("--input", type=int, choices=range(32), default=0)
            cmd.add_argument("--baseline", type=Path, required=True)
            cmd.add_argument("--hypothesis", action="append", required=True)
            cmd.add_argument("--symptom", required=True)
    cmd = sub.add_parser("diagnose")
    cmd.add_argument("capture", type=Path)
    cmd.add_argument("--symptom", required=True)
    cmd = sub.add_parser("compare")
    cmd.add_argument("before", type=Path)
    cmd.add_argument("after", type=Path)
    cmd.add_argument("--diff", type=Path, required=True)
    cmd = sub.add_parser("compare-captures")
    cmd.add_argument("baseline", type=Path)
    cmd.add_argument("candidate", type=Path)
    cmd.add_argument("--output", type=Path, required=True)
    cmd.add_argument("--same-game-state", action="store_true")
    args = parser.parse_args()
    if args.command == "compare-captures":
        compare_captures(args.baseline, args.candidate, args.output, args.same_game_state)
        print(f"Wrote {args.output}; production correction still requires review")
    elif args.command == "diagnose":
        report = diagnose(args.capture, args.symptom)
        print(f"Wrote {args.capture / 'diagnosis.json'}; confidence={report['confidence']}")
    elif args.command == "compare":
        report = compare_images(args.before, args.after, args.diff)
        write_json(args.diff.with_suffix(".json"), report)
        print(json.dumps(report, indent=2))
    elif args.command == "status":
        captures = sorted(args.directory.glob("capture-*"))
        print(f"Pending request: {(args.directory / 'request').exists()}")
        print(f"Latest capture: {captures[-1] if captures else 'none'}")
    elif args.command in ("discover", "reset"):
        request(args.directory, "action=reset" if args.command == "reset" else "action=capture frames=2")
    else:
        selectors = {key: getattr(args, key) for key in ("draw", "shader", "resource") if getattr(args, key) is not None}
        if not selectors or any(n < 0 for n in selectors.values()):
            parser.error("select a nonnegative draw, guest shader address, or guest resource address")
        options = [f"{k}={v}" for k, v in selectors.items()] + [f"frames={args.frames}", "images=1", "shaders=1"]
        action = "capture"
        if args.command == "experiment":
            if len(args.hypothesis) > 3:
                parser.error("provide at most three evidence-based hypotheses")
            if not any(e.get("event") == "capture_end" for e in read_events(args.baseline)):
                parser.error("baseline capture has not finished")
            if "resource" in selectors and args.change in ("solid", "uv", "normal", "checker", "no_blend"):
                parser.error("select a draw or shader for shader/blending interventions")
            if args.change in ("skip", "only"):
                action = args.change
            elif args.change.startswith("no_"):
                options.append(f"{args.change}=1")
            else:
                options += [f"output={args.change}", f"input={args.input}"]
            args.directory.mkdir(parents=True, exist_ok=True)
            write_json(args.directory / f"experiment-{time.time_ns()}.json", {
                "schema": 1, "kind": "diagnostic_experiment", "visual_symptom": args.symptom,
                "baseline": str(args.baseline.resolve()), "root_cause_hypotheses": args.hypothesis,
                "intervention": args.change, "selectors": selectors, "result": "pending_capture_and_review",
                "rollback": "automatic expiry; visual_debug.py reset restores diagnostic settings at the next GPU boundary",
                "hot_patch_rollback": "Use existing tools/hot_debug.py rollback for code patches; diagnostic reset does not roll back code or guest memory.",
                "production_fix_validated": False})
        request(args.directory, " ".join([f"action={action}"] + options))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError) as error:
        raise SystemExit(str(error)) from error
