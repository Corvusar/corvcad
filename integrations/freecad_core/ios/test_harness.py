"""Regression checks for runtime immutability and independently checked F1 evidence."""
import copy
import json
import os
import shutil
from pathlib import Path
import tempfile
import unittest
import sys
import subprocess
from unittest.mock import patch

import run_ios
import stage_runtime


class RuntimeBoundary(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def runtime(self):
        binary = self.root / "bin/FreeCADCmd"
        binary.parent.mkdir()
        binary.write_bytes(b"fixture executable")
        manifest = {"schema": 1, "executable": "bin/FreeCADCmd", "stdlib": "lib/python3.13",
                    "files": {"bin/FreeCADCmd": {"bytes": binary.stat().st_size, "sha256": run_ios.sha(binary)}},
                    "file_bytes": binary.stat().st_size}
        (self.root / "runtime-manifest.json").write_text(json.dumps(manifest))
        return binary

    def test_runtime_file_mutation_is_rejected(self):
        binary = self.runtime()
        run_ios.verify_runtime(self.root)
        binary.write_bytes(b"different executable")
        with self.assertRaisesRegex(ValueError, "modified"):
            run_ios.verify_runtime(self.root)

    def test_uninventoried_file_is_rejected(self):
        self.runtime()
        (self.root / "unreviewed.py").touch()
        with self.assertRaisesRegex(ValueError, "file set"):
            run_ios.verify_runtime(self.root)

    def test_linked_parent_is_rejected(self):
        directory = self.root / "actual"
        directory.mkdir()
        (directory / "resource.py").touch()
        try:
            (self.root / "linked").symlink_to(directory, target_is_directory=True)
        except OSError as error:
            self.skipTest(str(error))
        with self.assertRaisesRegex(ValueError, "Symlink"):
            stage_runtime.checked_file(self.root / "linked/resource.py", self.root)

    def test_generated_namespace_only_allows_cmake_substitution(self):
        original = self.root / "__init__.py.template"
        built = self.root / "__init__.py"
        # Independent small source has repeated substitutions, as upstream does.
        original.write_text('path="${FREECAD_LIBRARY_INSTALL_DIR}"\nprint("${FREECAD_LIBRARY_INSTALL_DIR}")\n')
        built.write_text(original.read_text().replace("${FREECAD_LIBRARY_INSTALL_DIR}", "/runtime/lib"))
        self.assertTrue(stage_runtime.generated_namespace_matches(built, original))
        built.write_text(built.read_text() + "unexpected()\n")
        self.assertFalse(stage_runtime.generated_namespace_matches(built, original))

    def test_wrong_host_python_is_rejected_before_staging(self):
        output = self.root / "runtime"
        with patch.object(stage_runtime.subprocess, "check_output", return_value="3.12.0\n"):
            with self.assertRaisesRegex(ValueError, "exact target"):
                stage_runtime.stage(self.root, self.root, self.root, self.root / "python", output)
        self.assertFalse(output.exists())

    @unittest.skipUnless(shutil.which("cmake"), "CMake required")
    def test_configured_helpers_accept_cmake_newlines_and_reject_edits(self):
        original = self.root / "part.py"
        built = self.root / "configured.py"
        script = self.root / "configure.cmake"
        script.write_text(f'configure_file("{original.as_posix()}" "{built.as_posix()}")\n')
        # The pinned part.py, partdesign.py and sketcher.py omit the final LF.
        for data in (b"# SPDX-License-Identifier: LGPL-2.1-or-later\n\nfrom Part import *",
                     b"# helper\r\npass\r\n", b"# helper\npass\n"):
            with self.subTest(source=data):
                original.write_bytes(data)
                subprocess.run(["cmake", "-P", str(script)], check=True, capture_output=True)
                self.assertTrue(stage_runtime.configured_helper_matches(built, original))
                generated = built.read_bytes()
                for changed in (generated + b"unexpected()\n", generated + b"\n",
                                generated.replace(b"#", b"# modified", 1)):
                    built.write_bytes(changed)
                    self.assertFalse(stage_runtime.configured_helper_matches(built, original))

    def test_staging_records_sources_bytecode_and_notices(self):
        source, build, python_stage = (self.root / name for name in ("source", "build", "python"))
        def file(root, name, text):
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        file(build, "bin/FreeCADCmd", "fixture binary")
        for module in stage_runtime.MODULES:
            file(source, f"src/Mod/{module}/Init.py", "# fixture Init.py\n")
            file(build, f"Mod/{module}/Init.py", "# fixture Init.py\n")
        file(source, "src/Mod/PartDesign/__init__.py", "# fixture module\n")
        file(build, "Mod/PartDesign/__init__.py", "# fixture module\n")
        for name in stage_runtime.EXT_FILES:
            text = 'path = "${FREECAD_LIBRARY_INSTALL_DIR}"\n' if name == "__init__.py" else "# fixture helper"
            file(source, "src/Ext/freecad/" + name + (".template" if name == "__init__.py" else ""), text)
            file(build, "Ext/freecad/" + name, text.replace("${FREECAD_LIBRARY_INSTALL_DIR}", "/runtime/lib").rstrip("\n") + "\n")
        for relative in ("Mod/Material/Resources/Materials/Standard/Default.FCMat",
                         "Mod/Material/Resources/Models/Mechanical/Density.yml",
                         "Mod/PartDesign/Resources/Hole/iso4762.json"):
            file(source, "src/" + relative, "fixture resource\n")
            file(build, "data/" + relative, "fixture resource\n")
        file(source, "LICENSE", "upstream fixture licence\n")
        file(source, "src/Doc/LICENSE.html", "upstream fixture licence\n")
        file(python_stage, "PYTHON-LICENSE.txt", "CPython fixture licence\n")
        file(python_stage, "lib/python3.13/encodings/__init__.py", "# fixture stdlib\n")
        output = self.root / "runtime"
        inputs = copy.deepcopy(stage_runtime.INPUTS)
        inputs["python"]["version"] = ".".join(map(str, sys.version_info[:3]))
        with patch.object(stage_runtime, "INPUTS", inputs), patch.object(stage_runtime.prepare_ios, "verify", return_value={"fixture": True}):
            manifest = stage_runtime.stage(source, build, python_stage, Path(sys.executable), output)
        self.assertIn("harness/proof.pyc", manifest["files"])
        self.assertEqual((output / "harness/proof.py").read_bytes(), (stage_runtime.CORE / "proof.py").read_bytes())
        self.assertEqual(run_ios.verify_runtime(output)["file_bytes"], manifest["file_bytes"])
        self.assertTrue((output / "notices/PYTHON-LICENSE.txt").is_file())
        self.assertFalse((output / "Mod/Part/InitGui.py").exists())
        with self.assertRaises(FileExistsError):
            output.mkdir(exist_ok=False)

    def test_child_paths_are_isolated_and_existing_injections_removed(self):
        case = self.root / "proof"
        case.mkdir()
        with patch.dict(os.environ, {"SIMCTL_CHILD_PYTHONPATH": "/host/site-packages"}):
            env = run_ios.child_environment(self.root / "runtime", case, self.root / "fixture.json")
        self.assertNotIn("SIMCTL_CHILD_PYTHONPATH", env)
        self.assertEqual(env["SIMCTL_CHILD_CORVCAD_PYTHON_STDLIB"], str(self.root / "runtime/lib/python3.13"))
        for name in ("home", "data", "temp"):
            self.assertTrue((case / name).is_dir())

    def test_simulator_delete_is_attempted_even_when_shutdown_times_out(self):
        with patch.object(run_ios.subprocess, "run", side_effect=[subprocess.TimeoutExpired("shutdown", 60),
                                                                  subprocess.CompletedProcess([], 0)]) as command:
            self.assertTrue(run_ios.cleanup_simulator("owned-device", self.root))
        self.assertEqual([call.args[0] for call in command.call_args_list],
                         [["xcrun", "simctl", "shutdown", "owned-device"], ["xcrun", "simctl", "delete", "owned-device"]])


class ProofEvidence(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.output = Path(temporary.name)
        self.harness = stage_runtime.CORE
        self.fixture = self.harness / "fixtures/demo.json"
        self.reference = self.harness / "evidence/f2-accepted-macos-demo.json"
        self.report = json.loads(self.reference.read_text())
        self.report["input_sha256"] = run_ios.sha(self.fixture)
        self.report["tools_sha256"] = {name: run_ios.sha(self.harness / name) for name in stage_runtime.PROOF_FILES}
        for name in run_ios.OUTPUTS:
            artifact = self.output / name
            artifact.write_bytes(("output fixture " + name).encode())
            self.report["files"][name] = {"bytes": artifact.stat().st_size, "sha256": run_ios.sha(artifact)}
        self.metrics = {"schema": 1, "passed": True, "platform": "ios",
                        "extension_origins": {name: "built-in" for name in run_ios.BUILTINS},
                        "recompute_call_seconds": [0.1, 0.2], "recompute_calls": 2,
                        "recompute_total_seconds": 0.3, "recompute_max_seconds": 0.2,
                        "peak_process_rss_bytes": 123456}

    def verify(self, reference=None):
        (self.output / "report.json").write_text(json.dumps(self.report))
        (self.output / "ios-metrics.json").write_text(json.dumps(self.metrics))
        return run_ios.verify_proof(self.output, self.fixture, self.harness, reference or self.reference)

    def test_complete_evidence_and_desktop_parity(self):
        result = self.verify()
        self.assertEqual(result["desktop_geometry_comparison"], "passed")

    def test_stale_input_report_is_rejected(self):
        self.report["input_sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "stale"):
            self.verify()

    def test_missing_or_extra_export_is_rejected(self):
        self.report["files"]["../unreviewed.step"] = self.report["files"].pop("tray.step")
        with self.assertRaisesRegex(ValueError, "output set"):
            self.verify()

    def test_corrupted_export_is_rejected(self):
        (self.output / "tray.step").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "hash or size"):
            self.verify()

    def test_nonfinite_geometry_is_rejected(self):
        self.report["parts"]["Tray"]["volume_mm3"] = float("nan")
        with self.assertRaisesRegex(ValueError, "finite"):
            self.verify()

    def test_duplicate_parameter_recompute_is_rejected(self):
        self.report["parameter_recomputes"][0] = copy.deepcopy(self.report["parameter_recomputes"][1])
        with self.assertRaisesRegex(ValueError, "parameter recomputes"):
            self.verify()

    def test_desktop_geometry_difference_is_rejected(self):
        reference = json.loads(self.reference.read_text())
        reference["step_reimport"]["Lid"]["bounds_mm"][0] += 0.01
        path = self.output / "different-desktop.json"
        path.write_text(json.dumps(reference))
        with self.assertRaises(AssertionError):
            self.verify(path)

    def test_desktop_process_cannot_count_as_ios(self):
        self.metrics["platform"] = "darwin"
        with self.assertRaisesRegex(ValueError, "actual iOS"):
            self.verify()

    def test_dynamic_native_module_is_rejected(self):
        self.metrics["extension_origins"]["Part"] = "/host/Part.so"
        with self.assertRaisesRegex(ValueError, "inittab"):
            self.verify()

    def test_missing_memory_measurement_is_rejected(self):
        self.metrics["peak_process_rss_bytes"] = 0
        with self.assertRaisesRegex(ValueError, "RSS"):
            self.verify()


if __name__ == "__main__":
    unittest.main()
