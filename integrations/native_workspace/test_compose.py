"""Focused raw-link boundaries and retained failure evidence. MIT."""
from argparse import Namespace
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
from types import ModuleType, SimpleNamespace
import unittest
from unittest.mock import patch

import compose


class Flags(unittest.TestCase):
    def test_frameworks_are_deduplicated_as_pairs(self):
        words = ["-framework", "Foundation", "-framework", "CoreFoundation",
                 "-framework", "Foundation", "-lobjc", "-lobjc",
                 "-weak_framework", "Foundation"]
        self.assertEqual(compose.system_options(words),
                         ["-framework", "Foundation", "-framework", "CoreFoundation",
                          "-lobjc", "-weak_framework", "Foundation"])
        with self.assertRaisesRegex(ValueError, "Incomplete"):
            compose.system_options(["-framework"])
        with self.assertRaisesRegex(ValueError, "Unreviewed"):
            compose.system_options(["-all_load"])

    def test_only_native_cores_are_force_loaded_and_raw_libraries_are_selected_once(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            core, module, dep = (root / name for name in ("core.a", "module.a", "libz.a"))
            for path in (core, module, dep):
                path.touch()
            options = compose.link_options([core, module, dep, dep], [core, module],
                                           ["-lz", "-framework", "Foundation"])
            self.assertEqual(options, ["-Wl,-dead_strip", "-Xlinker", "-force_load", "-Xlinker", str(core.resolve()),
                                       "-Xlinker", "-force_load", "-Xlinker", str(module.resolve()),
                                       str(dep.resolve()), "-framework", "Foundation"])
            self.assertEqual(shlex.split(shlex.join(options)), options)
            with self.assertRaisesRegex(ValueError, "omitted a required native core"):
                compose.link_options([core, dep], [core, module], [])
            bundled = root / "libcorvcad_freecad_deps.a"
            bundled.touch()
            with self.assertRaisesRegex(ValueError, "original raw"):
                compose.link_options([core, bundled], [core], [])

    def test_probe_uses_cpp20_public_c_abi_qtcore_and_explicit_ios_target(self):
        flags = compose.compile_flags(Path("/target"), Path("/sdk"), "iphonesimulator", "x86_64")
        self.assertIn("-std=c++20", flags)
        self.assertIn("--driver-mode=g++", flags)
        self.assertIn("x86_64-apple-ios17.0-simulator", flags)
        self.assertIn("-DQT_CORE_LIB", flags)
        self.assertIn("-DQT_STATIC", flags)
        self.assertIn("-I" + str(Path("/target") / "include/Qt6/QtCore"), flags)
        self.assertFalse(any("FreeCAD" in word or "Python" in word for word in flags))

    @unittest.skipUnless(shutil.which("xcrun"), "Xcode required for the actual C++ driver regression")
    def test_canonical_clang_driver_still_selects_cxx_runtime(self):
        compiler = Path(subprocess.check_output(["xcrun", "--sdk", "iphonesimulator", "--find", "clang++"], text=True).strip()).resolve()
        sdk = Path(subprocess.check_output(["xcrun", "--sdk", "iphonesimulator", "--show-sdk-path"], text=True).strip())
        flags = compose.compile_flags(Path("/unused-prefix"), sdk, "iphonesimulator", "x86_64")
        result = subprocess.run([str(compiler), *flags, "-###", "-x", "c++", "/dev/null"],
                                capture_output=True, text=True, check=True)
        self.assertIn("-lc++", shlex.split(result.stderr))


class LinkEvidence(unittest.TestCase):
    def fixture(self, root):
        repo = root / "repo"
        here = repo / "integrations/native_workspace"
        sources = [here / "probe/mixed.cpp", repo / "engine/src/schematic_route.cpp"]
        for source in sources:
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text("int source_fixture;\n")
        prefix, sdk = root / "prefix", root / "sdk"
        prefix.mkdir()
        sdk.mkdir()
        compiler = root / "clang++"
        compiler.write_bytes(b"compiler fixture")
        core, dep = root / "native.a", root / "libcommon.a"
        core.write_bytes(b"native core")
        dep.write_bytes(b"dependency archive")
        receipts = []
        for name in ("deps.json", "wx.json", "kicad.json", "package/package-manifest.json"):
            path = root / name
            path.parent.mkdir(exist_ok=True)
            path.write_text("{}")
            receipts.append(path)
        verifier = ModuleType("verify_inputs")
        verifier.__file__ = str(root / "verify_inputs.py")
        Path(verifier.__file__).write_text("# verifier fixture")
        verified = {"sdk": "iphonesimulator", "architecture": "x86_64", "prefix": prefix,
                    "core": [core], "dependencies": [dep], "system": ["-lobjc"],
                    "runtime": root / "runtime", "evidence": {"verified": True}}
        verifier.verify = lambda args: verified
        args = Namespace(sdk="iphonesimulator", architecture="x86_64", output=root / "output",
                         dependency_receipt=receipts[0], wx_receipt=receipts[1],
                         kicad_receipt=receipts[2], freecad_root=root / "freecad",
                         freecad_package=receipts[3].parent)
        return repo, here, sdk, compiler, verifier, verified, args

    def test_link_failure_keeps_response_preflight_logs_and_failure_recipe(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repo, here, sdk, compiler, verifier, verified, args = self.fixture(root)

            def output(command, **kwargs):
                if command[-1] == "--show-sdk-path":
                    return str(sdk)
                if command[-1] == "clang++":
                    return str(compiler)
                return "clang fixture"

            def run(command, **kwargs):
                if "-M" in command or "-c" in command:
                    source = Path(command[-1] if "-M" in command else command[command.index("-c") + 1])
                    depfile = Path(command[command.index("-MF") + 1])
                    depfile.write_text("unit: " + shlex.quote(source.as_posix()) + "\n")
                    if "-c" in command:
                        Path(command[command.index("-o") + 1]).write_bytes(b"compiled object")
                    return SimpleNamespace(returncode=0)
                kwargs["stdout"].write("ld: duplicate symbol fixture\n")
                return SimpleNamespace(returncode=1)

            with patch.object(compose, "REPO", repo), patch.object(compose, "HERE", here), \
                    patch.dict(sys.modules, {"verify_inputs": verifier}), \
                    patch.object(compose.select_archives, "select", return_value=(verified["core"] + verified["dependencies"], {"status": "archive variants selected"})), \
                    patch.object(compose.combine, "inspect", return_value={"strong_symbol_conflicts": {}}), \
                    patch.object(compose.subprocess, "check_output", side_effect=output), \
                    patch.object(compose.subprocess, "run", side_effect=run):
                with self.assertRaisesRegex(RuntimeError, "link failed"):
                    compose.compose(args)
            self.assertTrue((args.output / "input-selection.json").is_file())
            self.assertTrue((args.output / "preflight.json").is_file())
            self.assertIn("duplicate symbol fixture", (args.output / "link.log").read_text())
            recipe = json.loads((args.output / "link-recipe.json").read_text())
            self.assertEqual(recipe["status"], "failed")
            self.assertEqual(recipe["stage"], "link")
            self.assertEqual(recipe["commands"][-1]["returncode"], 1)
            self.assertFalse(recipe["runtime_validation"])
            response = shlex.split((args.output / "mixed-link.rsp").read_text())
            self.assertEqual(response.count("-force_load"), 1)
            self.assertEqual(response.count(str(verified["dependencies"][0].resolve())), 1)

    def test_existing_output_is_preserved_before_verification(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "output"
            output.mkdir()
            sentinel = output / "previous.log"
            sentinel.write_bytes(b"retain previous build")
            args = Namespace(output=output)
            with self.assertRaisesRegex(ValueError, "must be new"):
                compose.compose(args)
            self.assertEqual(list(output.iterdir()), [sentinel])
            self.assertEqual(sentinel.read_bytes(), b"retain previous build")

    def test_omission_rejection_retains_measured_selection_and_skips_link(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repo, here, sdk, compiler, verifier, verified, args = self.fixture(root)
            failed = {"missing_retained_providers": {"_required": {}}, "status": "rejected"}
            with patch.dict(sys.modules, {"verify_inputs": verifier}), \
                    patch.object(compose.select_archives, "select", side_effect=compose.select_archives.OmissionError(failed)), \
                    patch.object(compose.subprocess, "run") as run:
                with self.assertRaises(compose.select_archives.OmissionError):
                    compose.compose(args)
            self.assertEqual(json.loads((args.output / "input-selection.json").read_text()), failed)
            self.assertEqual(json.loads((args.output / "preflight.json").read_text())["status"], "not run")
            run.assert_not_called()

    def test_input_mutation_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "input.a"
            path.write_bytes(b"original")
            before = compose.snapshot([path])
            path.write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError, "Link input changed"):
                compose.unchanged(before)

    def test_strong_collision_report_is_retained_and_no_compiler_is_run(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repo, here, sdk, compiler, verifier, verified, args = self.fixture(root)
            preflight = {"status": "conflicts", "strong_symbol_conflicts": {"_same": [{}, {}]}}
            with patch.object(compose, "REPO", repo), patch.object(compose, "HERE", here), \
                    patch.dict(sys.modules, {"verify_inputs": verifier}), \
                    patch.object(compose.select_archives, "select", return_value=(verified["core"] + verified["dependencies"], {})), \
                    patch.object(compose.combine, "inspect", return_value=preflight), \
                    patch.object(compose.subprocess, "run") as run:
                with self.assertRaisesRegex(ValueError, "strong-symbol collisions"):
                    compose.compose(args)
            self.assertEqual(json.loads((args.output / "preflight.json").read_text()), preflight)
            run.assert_not_called()

    def test_archive_change_during_selection_cannot_become_the_link_baseline(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repo, here, sdk, compiler, verifier, verified, args = self.fixture(root)

            def select(paths):
                verified["dependencies"][0].write_bytes(b"changed after verification")
                return paths, {}

            with patch.object(compose, "REPO", repo), patch.object(compose, "HERE", here), \
                    patch.dict(sys.modules, {"verify_inputs": verifier}), \
                    patch.object(compose.select_archives, "select", side_effect=select), \
                    patch.object(compose.combine, "inspect") as preflight:
                with self.assertRaisesRegex(ValueError, "Link input changed"):
                    compose.compose(args)
            preflight.assert_not_called()


if __name__ == "__main__":
    unittest.main()
