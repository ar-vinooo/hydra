#!/usr/bin/env python3
"""Build/watch Hydra live GPU engines, renderer and shader compiler (stdlib only)."""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
HOT_SOURCES = (ROOT / "src/core/hot/table.cpp",
               ROOT / "src/core/hot/texture_cache_hot.cpp")


def hot_sources(build_dir):
    manifest = build_dir / "hot_sources.txt"
    if manifest.is_file():
        return tuple(Path(line) for line in manifest.read_text().splitlines() if line)
    return HOT_SOURCES


def module_path(build_dir):
    return build_dir / "libhydra-hot.so"


def read_status(build_dir):
    path = Path(str(module_path(build_dir)) + ".status")
    try:
        return dict(line.split("=", 1) for line in path.read_text().splitlines()
                    if "=" in line)
    except FileNotFoundError:
        return {}


def snapshot(paths):
    result = {}
    for path in paths:
        try:
            stat = path.stat()
            result[str(path)] = (stat.st_mtime_ns, stat.st_size)
        except FileNotFoundError:
            pass
    return result


def header_snapshot():
    return snapshot(path for path in (ROOT / "src").rglob("*")
                    if path.suffix in (".h", ".hpp", ".inc"))


def build(build_dir, *targets):
    return subprocess.run(["cmake", "--build", str(build_dir), "--target",
                           *targets, "-j", "4"], check=False).returncode == 0


def watch(build_dir, session, once=False, process=None):
    overrides = session / "override"
    overrides.mkdir(parents=True, exist_ok=True)
    sources = snapshot(hot_sources(build_dir))
    headers = header_snapshot()
    shaders = snapshot(overrides.glob("*.metal"))
    headers_changed = False
    print(f"Watching GPU sources and {overrides}", flush=True)
    print("Header/layout changes require a full build and app restart.", flush=True)
    while True:
        if process is not None and process.poll() is not None:
            return
        new_sources = snapshot(hot_sources(build_dir))
        new_headers = header_snapshot()
        new_shaders = snapshot(overrides.glob("*.metal"))
        if new_headers != headers and not headers_changed:
            print("Headers changed: restart Hydra after a full build; "
                  "live module build skipped.", flush=True)
            # Keep refusing until this watcher is restarted with the new app.
            headers_changed = True
        if not headers_changed and new_sources != sources:
            time.sleep(0.3)  # Coalesce editor writes before invoking the linker.
            pending = snapshot(hot_sources(build_dir))
            if build(build_dir, "hydra-hot"):
                print("Module rebuilt; check run.log for 'loaded module'.", flush=True)
            else:
                print("Build failed; running game keeps its previous module.", flush=True)
            sources = pending
        if new_shaders != shaders:
            time.sleep(0.3)
            shaders = snapshot(overrides.glob("*.metal"))
            (overrides / "reload").touch()
            print("Shader reload requested (including removed overrides).", flush=True)
        if once:
            return
        time.sleep(0.5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("run", "watch", "build", "reload", "rollback", "status", "trace",
                                                "gpucapture"))
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/swiftui-release")
    parser.add_argument("--session", type=Path, help="Defaults to <build-dir>/hot-debug")
    parser.add_argument("--once", action="store_true", help="One watcher pass, for setup checks")
    parser.add_argument("--perf-stats", action="store_true", help="Log aggregate CPU/GPU frame timings without per-draw waits")
    parser.add_argument("--dump-shaders", action="store_true", help="Save generated shaders (adds disk I/O)")
    parser.add_argument("--trace-options", default="", help="e.g. 'textures gbuffer=1 post=1'")
    parser.add_argument("--frames", type=int, default=2, help="gpucapture: frames to capture (1..8)")
    parser.add_argument("--gpu-capture", action="store_true", help="run: enable Metal GPU capture (needed by gpucapture; uses much more memory)")
    args = parser.parse_args()
    args.build_dir = args.build_dir.resolve()
    args.session = (args.session or args.build_dir / "hot-debug").resolve()
    for directory in ("override", "trace", "shaders", "gputrace"):
        (args.session / directory).mkdir(parents=True, exist_ok=True)
    if args.command == "status":
        current = read_status(args.build_dir)
        print("\n".join(f"{k}={v}" for k, v in current.items()) or
              "No hot reload host has reported status yet.")
    elif args.command == "rollback":
        Path(str(module_path(args.build_dir)) + ".rollback").touch()
        print("Rollback requested; inspect status/run.log for the result.")
    elif args.command == "trace":
        (args.session / "trace/trigger").write_text(args.trace_options + "\n")
        print(f"Next frame requested in {args.session / 'trace'}")
    elif args.command == "gpucapture":
        # Consumed by the renderer at the next presented frame (needs `run`)
        (args.session / "gputrace/trigger").write_text(f"{args.frames}\n")
        print(f"GPU capture of {args.frames} frame(s) requested; "
              f"see run.log for the .gputrace path in {args.session / 'gputrace'}")
    elif args.command == "reload":
        (args.session / "override/reload").touch()
        Path(str(module_path(args.build_dir)) + ".reload").touch()
    elif args.command == "build":
        return 0 if build(args.build_dir, "hydra-hot") else 1
    elif args.command == "watch":
        watch(args.build_dir, args.session, args.once)
    else:
        if not build(args.build_dir, "hydra", "hydra-hot"):
            return 1
        module = args.build_dir / "libhydra-hot.so"
        if not module.is_file():
            parser.error("Configure the build with -DHYDRA_HOT_RELOAD=ON first")
        env = dict(os.environ, HYDRA_HOT_MODULE=str(module),
                   HYDRA_TRACE_DIR=str(args.session / "trace"),
                   HYDRA_SHADER_OVERRIDE_DIR=str(args.session / "override"),
                   HYDRA_GPU_CAPTURE_DIR=str(args.session / "gputrace"))
        if args.gpu_capture:
            # .gputrace documents outside Xcode (tools/hot_debug.py gpucapture).
            # Metal's capture layer retains every resource: high memory use.
            env["MTL_CAPTURE_ENABLED"] = "1"
        if args.perf_stats:
            env["HYDRA_PERF_STATS"] = "1"
        if args.dump_shaders:
            env["HYDRA_DUMP_SHADERS"] = str(args.session / "shaders")
        executable = args.build_dir / "bin/Hydra.app/Contents/MacOS/Hydra"
        with (args.session / "run.log").open("a") as log:
            process = subprocess.Popen([str(executable)], env=env, stdout=log,
                                       stderr=subprocess.STDOUT,
                                       start_new_session=True)
            print(f"Hydra PID {process.pid}; log: {args.session / 'run.log'}", flush=True)
            try:
                watch(args.build_dir, args.session, process=process)
                print(f"Hydra exited with code {process.returncode}", flush=True)
            except KeyboardInterrupt:
                pass
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)
