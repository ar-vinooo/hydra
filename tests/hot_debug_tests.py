"""Exercise watcher events without compiling or launching Hydra."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import sys

sys.dont_write_bytecode = True

spec = importlib.util.spec_from_file_location(
    "hot_debug", Path(__file__).resolve().parents[1] / "tools/hot_debug.py")
debug = importlib.util.module_from_spec(spec)
spec.loader.exec_module(debug)


class WatchTests(unittest.TestCase):
    def test_manifest_watches_compiled_renderer_sources(self):
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = Path(tmp)
            source = build_dir / "depth_stencil_state_cache.cpp"
            source.write_text("old")
            (build_dir / "hot_sources.txt").write_text(str(source) + "\n")
            original = debug.snapshot
            calls = 0

            def snapshots(paths):
                nonlocal calls
                paths = list(paths)
                if paths == [source]:
                    calls += 1
                    if calls == 2:
                        source.write_text("new renderer implementation")
                return original(paths)

            with patch.object(debug, "snapshot", side_effect=snapshots), \
                 patch.object(debug, "header_snapshot", return_value={}), \
                 patch.object(debug.time, "sleep"), \
                 patch.object(debug, "build", return_value=True) as build:
                debug.watch(build_dir, build_dir, once=True)
            build.assert_called_once_with(build_dir, "hydra-hot")

    def test_status_reports_host_revision(self):
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = Path(tmp)
            self.assertEqual(debug.read_status(build_dir), {})
            Path(str(debug.module_path(build_dir)) + ".status").write_text(
                "pid=42\nrevision=7\nstate=loaded\n")
            self.assertEqual(debug.read_status(build_dir)["revision"], "7")

    def test_shader_edit_and_removal_request_reload(self):
        for remove in (False, True):
            with self.subTest(remove=remove), tempfile.TemporaryDirectory() as tmp:
                session = Path(tmp)
                override = session / "override"
                override.mkdir()
                shader = override / "0x123_Fragment.metal"
                shader.write_text("old")

                def change(paths):
                    if change.calls == 1:
                        if remove:
                            shader.unlink()
                        else:
                            shader.write_text("new source")
                    change.calls += 1
                    return original(paths)

                change.calls = 0
                original = debug.snapshot
                # snapshot is called for sources, headers, then shaders;
                # inject the edit immediately before the first polling pass.
                def snapshots(paths):
                    paths = list(paths)
                    if paths and paths[0].suffix == ".metal":
                        return change(paths)
                    return original(paths)

                with patch.object(debug, "snapshot", side_effect=snapshots), \
                     patch.object(debug.time, "sleep"), \
                     patch.object(debug, "build") as build:
                    debug.watch(session, session, once=True)
                self.assertTrue((override / "reload").exists())
                build.assert_not_called()

    def test_header_change_blocks_live_build(self):
        with tempfile.TemporaryDirectory() as tmp, \
             patch.object(debug, "header_snapshot", side_effect=[{}, {"new.hpp": (1, 1)}]), \
             patch.object(debug, "snapshot", side_effect=[{}, {}, {"changed.cpp": (1, 1)}, {}]), \
             patch.object(debug, "build") as build:
            debug.watch(Path(tmp), Path(tmp), once=True)
            build.assert_not_called()


if __name__ == "__main__":
    unittest.main()
