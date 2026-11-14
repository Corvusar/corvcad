"""Fail-closed tests for the combined edition's KiCad rebuild recipe."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

import kicad_build as build


class RecipeTests(unittest.TestCase):
    def test_resume_requires_matching_owned_build_without_overwriting_other_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "native"
            claim = {"schema": 1, "target": "x86_64-apple-ios17.0-simulator", "recipe": "reviewed"}
            build.claim_output(root, claim)
            preserved = root / "log.txt"
            preserved.write_bytes(b"previous compiler evidence")
            build.claim_output(root, claim, resume=True)
            with self.assertRaisesRegex(ValueError, "matching native build claim"):
                build.claim_output(root, {**claim, "recipe": "changed"}, resume=True)
            with self.assertRaisesRegex(ValueError, "new output directory"):
                build.claim_output(root, claim)
            self.assertEqual(preserved.read_bytes(), b"previous compiler evidence")
            unknown = Path(directory) / "someone-elses-build"
            unknown.mkdir()
            (unknown / "user.cpp").write_text("preserve")
            with self.assertRaisesRegex(ValueError, "matching native build claim"):
                build.claim_output(unknown, claim, resume=True)
            self.assertEqual((unknown / "user.cpp").read_text(), "preserve")

    def test_plot_dialog_standin_is_limited_to_plot_implementation(self):
        root = Path(tempfile.gettempdir()).resolve() / "plot-compat-fixture"
        for source in ("pcbnew/pcbplot.cpp", "pcbnew/board.cpp"):
            row = {"directory": str(root), "file": str(root / "kicad" / source),
                   "arguments": ["clang++", "-c", str(root / "kicad" / source)]}
            result = build.transform_command(row, kicad=root / "kicad", build=root / "build",
                generated=root / "generated", prefix=root / "union", standin=root / "standin",
                wx_flags=[], sdk_path=root / "sdk", triple="x86_64-apple-ios17.0-simulator",
                compiler=root / "clang++", obj=root / "unit.o", depfile=root / "unit.d", extra=root / "extra")
            include = "-I" + str(build.HERE / "headless/ios")
            self.assertEqual(include in result, source == "pcbnew/pcbplot.cpp")
            if source == "pcbnew/pcbplot.cpp":
                self.assertLess(result.index(include), result.index("-I" + str(root / "standin")))

    def test_debug_simulator_standin_is_included_once(self):
        standin = next(iter(build.REQUIRED_STANDINS))
        for closure in (["kicad/model.cpp"], ["kicad/model.cpp", standin]):
            selected = build.native_sources({"closure": closure + [build.FMT_SOURCE],
                                             "direct_objects": list(build.APP_OWNED | build.TEST_ROOTS)})
            self.assertEqual(selected.count(standin), 1)
            self.assertIn("kicad/model.cpp", selected)
            self.assertNotIn(build.FMT_SOURCE, selected)
            self.assertFalse(set(selected) & (build.APP_OWNED | build.TEST_ROOTS))

    def test_real_simulator_replaces_the_standin(self):
        standin = next(iter(build.REQUIRED_STANDINS))
        selected = build.native_sources({"closure": ["kicad/model.cpp", build.NGSPICE_SOURCE],
                                         "direct_objects": []})
        self.assertIn(build.NGSPICE_SOURCE, selected)
        self.assertNotIn(standin, selected)

    def test_ngspice_build_comes_from_the_probe_cache(self):
        with tempfile.TemporaryDirectory() as folder:
            cache = Path(folder) / "CMakeCache.txt"
            cache.write_text("NGSPICE_BUILD:PATH=\nKICAD_SOURCE:PATH=/k\n")
            self.assertIsNone(build.ngspice_build(folder))
            cache.write_text("NGSPICE_BUILD:PATH=" + folder + "\n")
            self.assertEqual(build.ngspice_build(folder), Path(folder).resolve())

    def test_device_architecture_is_not_a_simulator(self):
        self.assertEqual(build.target("iphonesimulator", "x86_64"), "x86_64-apple-ios17.0-simulator")
        self.assertEqual(build.target("iphoneos", "arm64"), "arm64-apple-ios17.0")
        with self.assertRaises(ValueError):
            build.target("iphoneos", "x86_64")

    def test_translation_replaces_target_headers_without_losing_model_defines(self):
        root = Path(tempfile.gettempdir()).resolve() / "workspace-build-fixture"
        kicad, desktop, prefix = root / "kicad", root / "desktop", root / "union"
        row = {"directory": str(desktop), "arguments": ["/usr/bin/c++", "-DPCBNEW", "-DKIAPI_IMPORTEXPORT=",
               "-DPROTOBUF_USE_DLLS", "-D__WXMAC__", "-I" + str(kicad / "thirdparty/fmt/include"),
               "-I", str(kicad / "thirdparty/fmt"), "-isystem", "/usr/local/include/wx-3.3",
               "-I" + str(desktop / "generated/cpp"), "-I" + str(kicad / "include"),
               "-std=gnu++20", "-MD", "-MT", "old.o", "-MF", "old.d", "-o", "old.o",
               "-c", str(desktop / "generated/cpp/api/board.pb.cc")]}
        result = build.transform_command(row, kicad=kicad, build=desktop, generated=root / "generated-target",
                  prefix=prefix, standin=root / "standins", wx_flags=["-D__WXOSX_IPHONE__"],
                  sdk_path=root / "iPhoneSimulator.sdk", triple=build.target("iphonesimulator", "x86_64"),
                  compiler=root / "clang++", obj=root / "new.o", depfile=root / "new.d", extra=root / "extra")
        self.assertIn("-DPCBNEW", result)
        self.assertIn("-DKIAPI_IMPORTEXPORT=", result)
        self.assertIn("-D__WXOSX_IPHONE__", result)
        self.assertIn("-DFMT_DEPRECATED_HEAVY_CORE=1", result)
        self.assertIn(str(root / "generated-target/api/board.pb.cc"), result)
        self.assertNotIn("-DPROTOBUF_USE_DLLS", result)
        self.assertNotIn("old.o", result)
        self.assertFalse(any("thirdparty/fmt" in x or "/usr/local" in x for x in result))
        self.assertEqual(result.count("-MD"), 1)
        self.assertEqual(result[result.index("-MF") + 1], str(root / "new.d"))

    def test_shell_commands_are_never_evaluated(self):
        row = {"command": "clang++ -c file.cpp && echo unexpected", "directory": "/tmp"}
        with self.assertRaises(ValueError):
            build.transform_command(row, kicad=Path("/k"), build=Path("/b"), generated=Path("/g"),
              prefix=Path("/p"), standin=Path("/s"), wx_flags=[], sdk_path=Path("/sdk"), triple="test",
              compiler=Path("/clang"), obj=Path("/o"), depfile=Path("/d"), extra=Path("/e"))

    def test_ambiguous_or_missing_source_commands_fail(self):
        path = Path(tempfile.gettempdir()).resolve() / "foo.cpp"
        row = {"file": str(path), "directory": str(path.parent), "command": "clang++ -c foo.cpp"}
        with self.assertRaises(ValueError):
            build.select_commands([row, row], {"kicad/foo.cpp": path})
        with self.assertRaises(ValueError):
            build.select_commands([], {"kicad/foo.cpp": path})
        self.assertEqual(len(build.select_commands([row], {"kicad/foo.cpp": path})), 1)

    def test_depfile_preserves_escaped_paths(self):
        deps = build.parse_depfile("/tmp/a.o: /tmp/a.cpp \\\n /tmp/folder\\ with\\ spaces/header.h /tmp/a.cpp /tmp/dollar$$.h\n")
        self.assertEqual(deps, ["/tmp/a.cpp", "/tmp/dollar$.h", "/tmp/folder with spaces/header.h"])

    def test_actual_header_paths_reject_vendored_fmt_and_desktop_leaks(self):
        root = Path(tempfile.gettempdir()).resolve() / "kicad-fixture"
        roots = [("kicad", root), ("vcpkg", root.parent / "union")]
        with self.assertRaisesRegex(ValueError, "Vendored fmt"):
            build.dependency_key(root / "thirdparty/fmt/include/fmt/base.h", roots)
        with self.assertRaisesRegex(ValueError, "Unclassified compiler input"):
            build.dependency_key(root.parent / "Homebrew/expat.h", roots)
        self.assertEqual(build.dependency_key(root.parent / "union/include/fmt/base.h", roots),
                         "vcpkg/include/fmt/base.h")

    def test_receipt_must_match_target_and_prefix(self):
        prefix = Path(tempfile.gettempdir()).resolve() / "prefix"
        receipt = {"schema": 1, "status": "dependency inputs built and target archives verified",
                   "target": {"sdk": "iphonesimulator", "architecture": "x86_64"},
                   "paths": {"prefix": str(prefix)}, "inventory": {"archives": {"lib/a.a": {}}},
                   "macho": {"sdk": "iphonesimulator", "architecture": "x86_64", "archives": {"lib/a.a": {}}}}
        build.validate_dependency_receipt(receipt, "iphonesimulator", "x86_64", prefix)
        for field, value in (("status", "failed"), ("schema", 2), ("macho", {})):
            changed = copy.deepcopy(receipt)
            changed[field] = value
            with self.assertRaises(ValueError):
                build.validate_dependency_receipt(changed, "iphonesimulator", "x86_64", prefix)
        with self.assertRaises(ValueError):
            build.validate_dependency_receipt(receipt, "iphoneos", "arm64", prefix)
        with self.assertRaises(ValueError):
            build.validate_dependency_receipt(receipt, "iphonesimulator", "x86_64", prefix / "other")

    def test_changed_dependency_archive_and_foreign_protoc_fail(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            prefix = root / "installed/x64-ios-simulator"
            (prefix / "include/fmt").mkdir(parents=True)
            (prefix / "include/fmt/base.h").write_text("#define FMT_VERSION 120200\n")
            (prefix / "lib").mkdir()
            archive = prefix / "lib/fmt.a"
            archive.write_bytes(b"reviewed archive")
            protoc = root / "installed/x64-osx/tools/protobuf/protoc"
            protoc.parent.mkdir(parents=True)
            protoc.write_bytes(b"host tool")
            receipt = {"target": {"host_triplet": "x64-osx"}, "paths": {"installed": str(root / "installed")},
                       "inventory": {"archives": {"lib/fmt.a": {"sha256": build.sha(archive)}}},
                       "macho": {"archives": {"lib/fmt.a": {"sha256": build.sha(archive)}}}}
            for name in ("inventory", "macho"):
                path = root / (name + ".json")
                path.write_text(json.dumps(receipt[name]))
                receipt["paths"][name] = str(path)
                receipt[name + "_sha256"] = build.sha(path)
            build.verify_dependency_inputs(receipt, prefix, protoc)
            with self.assertRaisesRegex(ValueError, "protoc"):
                build.verify_dependency_inputs(receipt, prefix, root / "other-protoc")
            archive.write_bytes(b"stale incompatible archive")
            with self.assertRaisesRegex(ValueError, "archive changed"):
                build.verify_dependency_inputs(receipt, prefix, protoc)


if __name__ == "__main__":
    unittest.main()
