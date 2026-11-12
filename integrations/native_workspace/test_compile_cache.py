"""Actual content and path-boundary regressions for resumable native compilation."""
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest

import compile_cache as cache


def hashes(path):
    data = path.read_bytes()
    return {"sha256": hashlib.sha256(data).hexdigest(),
            "sha256_lf": hashlib.sha256(data.replace(b"\r\n", b"\n")).hexdigest()}


class CompileCacheTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.root = Path(self.folder.name).resolve()
        self.source_root = self.root / "source"
        self.source_root.mkdir()
        self.source = self.source_root / "unit.cpp"
        self.header = self.source_root / "header.hpp"
        self.source.write_text('#include "header.hpp"\n')
        self.header.write_text("#define VALUE 1\n")
        self.output = self.root / "output"
        (self.output / "objects").mkdir(parents=True)
        self.object = self.output / "objects/unit.o"
        self.command = ["clang++", "-c", str(self.source), "-o", str(self.object)]
        self.unit = ("kicad/unit.cpp", {"directory": str(self.source_root)}, self.command,
                     self.object, self.output / "unit.d")
        self.roots = [("kicad", self.source_root)]
        self.calls = 0

    def compile(self, unit, roots, generated, output):
        self.calls += 1
        source = Path(unit[2][unit[2].index("-c") + 1])
        self.object.write_bytes(b"object:" + source.read_bytes() + self.header.read_bytes())
        dependencies = {"kicad/" + source.name: hashes(source), "kicad/header.hpp": hashes(self.header)}
        return {"source": unit[0], "object": {"path": str(self.object), "sha256": hashes(self.object)["sha256"]},
                "command": unit[2], "dependencies": sorted(dependencies)}, dependencies

    def cached(self, callback=None, unit=None):
        return cache.compile_cached(unit or self.unit, self.roots, self.output / "generated", self.output,
                                    callback or self.compile)

    def checkpoint(self):
        return next((self.output / "checkpoints").glob("*.json"))

    def test_successful_real_compile_is_reused_with_unchanged_return_format(self):
        first = self.cached()
        self.assertEqual(self.cached(), first)
        self.assertEqual(self.calls, 1)
        self.assertEqual(set(first[0]), {"source", "object", "command", "dependencies"})

    def test_object_without_a_sidecar_is_never_imported(self):
        self.object.write_bytes(b"old attempt")
        self.cached()
        self.assertEqual(self.calls, 1)

    def test_source_and_header_changes_invalidate_even_with_original_mtime(self):
        self.cached()
        for path in (self.source, self.header):
            before = path.stat()
            path.write_bytes(path.read_bytes() + b"/* changed */\n")
            os.utime(path, ns=(before.st_atime_ns, before.st_mtime_ns))
            self.cached()
        self.assertEqual(self.calls, 3)

    def test_changed_command_object_or_source_key_recompiles(self):
        self.cached()
        self.command.insert(1, "-DCHANGED=1")
        self.cached()
        self.object.write_bytes(b"replaced object")
        self.cached()
        unit = ("kicad/renamed.cpp", *self.unit[1:])
        self.cached(unit=unit)
        self.assertEqual(self.calls, 4)

    def test_missing_header_is_a_miss_then_real_compiler_failure_preserves_checkpoint(self):
        self.cached()
        checkpoint = self.checkpoint()
        previous = checkpoint.read_bytes()
        self.header.unlink()
        with self.assertRaises(FileNotFoundError):
            self.cached()
        self.assertEqual(self.calls, 2)
        self.assertEqual(checkpoint.read_bytes(), previous)

    def test_missing_object_and_invalid_sidecar_recompile(self):
        self.cached()
        self.object.unlink()
        self.cached()
        self.checkpoint().write_text("not JSON")
        self.cached()
        self.assertEqual(self.calls, 3)

    def test_nonobject_json_checkpoint_is_an_invalid_sidecar(self):
        self.cached()
        for invalid in ("[]", "null", "42", '"string"'):
            self.checkpoint().write_text(invalid)
            self.cached()
        self.assertEqual(self.calls, 5)

    def test_cache_dependency_escape_is_a_miss_without_reading_outside_root(self):
        self.cached()
        payload = json.loads(self.checkpoint().read_text())
        payload["dependencies"]["kicad/../outside.h"] = {"sha256": "0" * 64}
        payload["record"]["dependencies"].append("kicad/../outside.h")
        self.checkpoint().write_text(json.dumps(payload))
        self.cached()
        self.assertEqual(self.calls, 2)

    def test_invalid_new_dependency_keys_fail_closed_and_preserve_checkpoint(self):
        self.cached()
        checkpoint = self.checkpoint()
        previous = checkpoint.read_bytes()
        self.command.insert(1, "-DRECOMPILE=1")
        for key in ("kicad/../outside.h", "kicad//header.hpp", "/absolute.h", "unknown/header.hpp",
                    "kicad/C:/outside.h", "kicad/..\\outside.h"):
            def invalid(*args):
                record, dependencies = self.compile(*args)
                dependencies[key] = hashes(self.header)
                record["dependencies"].append(key)
                return record, dependencies
            with self.subTest(key=key), self.assertRaises(cache.InvalidCheckpoint):
                self.cached(callback=invalid)
            self.assertEqual(checkpoint.read_bytes(), previous)

    def test_actual_source_must_appear_in_compiler_dependencies(self):
        def no_source(*args):
            record, dependencies = self.compile(*args)
            del dependencies["kicad/unit.cpp"]
            record["dependencies"] = sorted(dependencies)
            return record, dependencies
        with self.assertRaisesRegex(cache.InvalidCheckpoint, "omit.*source"):
            self.cached(callback=no_source)
        self.assertFalse(list((self.output / "checkpoints").glob("*.json")))

    def test_generated_source_key_can_differ_from_actual_source_path(self):
        source = self.source_root / "board.pb.cc"
        source.write_text("// regenerated protobuf\n")
        command = ["clang++", "-c", str(source), "-o", str(self.object)]
        unit = ("generated/cpp/api/board.pb.cc", self.unit[1], command, *self.unit[3:])
        self.cached(unit=unit)
        self.cached(unit=unit)
        self.assertEqual(self.calls, 1)

    def test_failed_or_invalid_callback_never_creates_checkpoint(self):
        def failure(*args):
            raise RuntimeError("compiler failed")
        with self.assertRaises(RuntimeError):
            self.cached(callback=failure)
        def stale_object(*args):
            record, dependencies = self.compile(*args)
            record["object"]["sha256"] = "0" * 64
            return record, dependencies
        with self.assertRaises(cache.InvalidCheckpoint):
            self.cached(callback=stale_object)
        self.assertFalse(list((self.output / "checkpoints").glob("*.json")))

    def test_missing_dependency_and_symlink_escape_fail_closed(self):
        with self.assertRaises(FileNotFoundError):
            cache._dependency_path("kicad/missing.hpp", dict(self.roots))
        outside = self.root / "outside.hpp"
        outside.write_text("outside root\n")
        link = self.source_root / "linked.hpp"
        try:
            link.symlink_to(outside)
        except (OSError, NotImplementedError):
            self.skipTest("Host does not permit symlink creation")
        with self.assertRaises(cache.InvalidCheckpoint):
            cache._dependency_path("kicad/linked.hpp", dict(self.roots))

    def test_object_destination_cannot_escape_output(self):
        unit = (*self.unit[:3], self.root / "foreign.o", self.unit[4])
        with self.assertRaises(cache.InvalidCheckpoint):
            self.cached(unit=unit)
        self.assertEqual(self.calls, 0)


if __name__ == "__main__":
    unittest.main()
