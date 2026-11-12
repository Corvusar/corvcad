"""Regression checks for mixed-probe ownership and post-process acceptance. MIT."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("mixed_probe_runner", Path(__file__).with_name("run.py"))
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)
DEVICE = "93b3f777-f8f2-4206-a916-c0ee6df62dc1"


class RunnerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.runtime, self.source = self.root / "runtime", self.root / "source"
        self.runtime.mkdir()
        self.source.mkdir()
        (self.runtime / "resource.py").write_bytes(b"read-only runtime\r\n")
        (self.source / "RoundTrip.kicad_pro").write_bytes(b"original project\r\n")
        (self.source / "RoundTrip.kicad_sch").write_bytes(b"original schematic\r\n")
        (self.source / "RoundTrip.kicad_pcb").write_bytes(b"original PCB\r\n")
        self.fixture, self.binary, self.output = (self.root / name for name in ("fixture.json", "probe", "output"))
        self.fixture.write_bytes(b"{}")
        self.binary.write_bytes(b"actual binary")
        self.calls = []
        self.mode = None

    def check_output(self, command, **kwargs):
        if command[1:3] == ["lipo", "-archs"]:
            return "x86_64\n"
        if command[1:3] == ["vtool", "-show-build"]:
            return "platform IOSSIMULATOR\nminos 17.0\n"
        if command[0] == "otool":
            return "probe:\n /usr/lib/libSystem.B.dylib (compatibility version 1.0.0)\n"
        if command[2:5] == ["list", "runtimes", "-j"]:
            return json.dumps({"runtimes": [{"isAvailable": True, "identifier": "com.apple.CoreSimulator.SimRuntime.iOS-26-2", "version": "26.2"}]}).encode()
        if command[2:5] == ["list", "devicetypes", "-j"]:
            return json.dumps({"devicetypes": [{"name": "iPad Pro", "identifier": "com.apple.CoreSimulator.SimDeviceType.iPad-Pro"}]}).encode()
        if command[2] == "create":
            return DEVICE + "\n"
        raise AssertionError(command)

    def process(self, command, **kwargs):
        self.calls.append((command, kwargs))
        if command[2] == "spawn":
            self.assertEqual(command[3:5], ["--standalone", DEVICE])
            order, case = command[6], Path(command[-1])
            case.mkdir()
            project = case / "project"
            project.mkdir()
            original = runner.snapshot(self.source)
            for name in original:
                (project / name).write_bytes((self.source / name).read_bytes())
            board_copy = case / "board-copy"
            board_copy.mkdir()
            for name in original:
                (board_copy / name).write_bytes((self.source / name).read_bytes())
            (board_copy / "RoundTrip.kicad_pro").write_bytes(b"edited native clearance project")
            (board_copy / "RoundTrip.kicad_pcb").write_bytes(b"native serialized edited PCB")
            (case / "board-setup-before.txt").write_bytes(b"rule\tmin_clearance\t0.2\nother\trule\tvalue\n")
            (case / "board-setup-after.txt").write_bytes(b"rule\tmin_clearance\t0.225\nother\trule\tvalue\n")
            edit = {"rule": "min_clearance", "before_mm": 0.2, "after_mm": 0.225,
                    "copy": "board-copy", "board_file": "RoundTrip.kicad_pcb", "project_file": "RoundTrip.kicad_pro",
                    "board": runner.record(board_copy / "RoundTrip.kicad_pcb"),
                    "project": runner.record(board_copy / "RoundTrip.kicad_pro"),
                    "setup_before_sha256": runner.record(case / "board-setup-before.txt")["sha256"],
                    "setup_after_sha256": runner.record(case / "board-setup-after.txt")["sha256"]}
            if self.mode == "board-path-escape":
                edit["board_file"] = "../RoundTrip.kicad_pcb"
            if self.mode == "board-bad-delta":
                edit["after_mm"] = 0.3
            if self.mode == "board-nan":
                edit["after_mm"] = float("nan")
            operations = {key: True for key in runner.OPERATIONS}
            if self.mode == "missing-board-operation":
                del operations["pcb_board_setup_edit_reopen"]
            exports = {}
            for name in runner.EXPORTS:
                path = project / name if name == "enclosure.FCStd" else case / name
                path.write_bytes(b"actual output")
                exports[name] = runner.record(path)
            (case / "input.json").write_bytes(b"{\"source\":{}}")
            runner.write_json(case / "mixed-report.json", {
                "schema": 1, "passed": True, "order": order, "source_inputs": original,
                "board_edit": edit, "operations": operations, "capabilities": 57,
                "board_backend": runner.NATIVE_BACKEND, "schematic_backend": runner.NATIVE_BACKEND,
                "exports": exports, "input_json_sha256": runner.record(case / "input.json")["sha256"]})
            if self.mode == "project-teardown":
                (project / "RoundTrip.kicad_pro").write_bytes(b"settings cleanup changed this after the report")
            if self.mode == "runtime-teardown":
                (self.runtime / "new-resource.pyc").write_bytes(b"unexpected mutation")
            if self.mode == "board-teardown":
                (board_copy / "RoundTrip.kicad_pcb").write_bytes(b"mutated after process report")
            if self.mode == "board-other-file":
                (board_copy / "RoundTrip.kicad_sch").write_bytes(b"unrelated schematic mutation")
            if self.mode == "board-setup-teardown":
                (case / "board-setup-after.txt").write_bytes(b"changed after report")
            code = 23 if self.mode == "late-crash" else 0
        elif command[2] == "delete" and self.mode == "delete-failure":
            code = 1
        else:
            code = 0
        return subprocess.CompletedProcess(command, code, stdout="", stderr="")

    def validate(self):
        with patch.object(runner.subprocess, "check_output", side_effect=self.check_output), patch.object(runner.subprocess, "run", side_effect=self.process):
            return runner.validate(self.binary, self.runtime, self.source, self.fixture, self.output)

    def test_both_orders_are_fresh_processes_and_owned_simulator_is_deleted(self):
        with patch.dict(os.environ, {"SIMCTL_CHILD_PYTHONPATH": "/user/site-packages"}):
            result = self.validate()
        self.assertTrue(result["passed"])
        self.assertTrue(result["runtime_unchanged"])
        self.assertEqual((self.source / "RoundTrip.kicad_pcb").read_bytes(), b"original PCB\r\n")
        for order in runner.ORDERS:
            self.assertEqual((self.output / order / "project/RoundTrip.kicad_pcb").read_bytes(), b"original PCB\r\n")
            self.assertEqual((self.output / order / "board-copy/RoundTrip.kicad_sch").read_bytes(), b"original schematic\r\n")
        spawned = [(command, options) for command, options in self.calls if command[2] == "spawn"]
        self.assertEqual([command[6] for command, _ in spawned], list(runner.ORDERS))
        self.assertNotEqual(spawned[0][1]["env"]["SIMCTL_CHILD_HOME"], spawned[1][1]["env"]["SIMCTL_CHILD_HOME"])
        self.assertEqual(spawned[0][1]["env"]["SIMCTL_CHILD_PYTHONPATH"], "")
        self.assertEqual([command for command, _ in self.calls if command[2] in ("shutdown", "delete")],
                         [["xcrun", "simctl", "shutdown", DEVICE], ["xcrun", "simctl", "delete", DEVICE]])

    def test_report_written_before_teardown_crash_cannot_pass(self):
        self.mode = "late-crash"
        with self.assertRaisesRegex(RuntimeError, "process failed"):
            self.validate()
        summary = json.loads((self.output / "mixed-simulator.json").read_text())
        self.assertFalse(summary["passed"])
        self.assertEqual(summary["cases"]["kicad-first"]["returncode"], 23)
        self.assertTrue((self.output / "kicad-first/mixed-report.json").exists())
        self.assertTrue(summary["simulator_deleted"])

    def test_post_exit_project_mutation_is_rejected(self):
        self.mode = "project-teardown"
        with self.assertRaisesRegex(ValueError, "Copied original project changed after process exit"):
            self.validate()
        self.assertTrue(json.loads((self.output / "simulator-cleanup.json").read_text())["actions"]["delete"]["returncode"] == 0)

    def test_new_runtime_file_is_rejected_and_retained_in_failed_output(self):
        self.mode = "runtime-teardown"
        with self.assertRaisesRegex(ValueError, "Input files changed"):
            self.validate()
        summary = json.loads((self.output / "mixed-simulator.json").read_text())
        self.assertFalse(summary["runtime_unchanged"])
        self.assertFalse(summary["passed"])
        self.assertTrue((self.runtime / "new-resource.pyc").exists())

    def test_post_exit_edited_board_copy_mutation_is_rejected(self):
        self.mode = "board-teardown"
        with self.assertRaisesRegex(ValueError, "Board edit copy changed after process exit"):
            self.validate()
        self.assertEqual((self.source / "RoundTrip.kicad_pcb").read_bytes(), b"original PCB\r\n")
        self.assertFalse(json.loads((self.output / "mixed-simulator.json").read_text())["passed"])

    def test_board_edit_cannot_change_another_original_file(self):
        self.mode = "board-other-file"
        with self.assertRaisesRegex(ValueError, "Board edit copy changed another original file"):
            self.validate()

    def test_board_setup_snapshot_mutation_is_rejected(self):
        self.mode = "board-setup-teardown"
        with self.assertRaisesRegex(ValueError, "Board Setup snapshot changed after process exit"):
            self.validate()

    def test_report_paths_cannot_escape_owned_board_copy(self):
        self.mode = "board-path-escape"
        with self.assertRaisesRegex(ValueError, "copy filenames do not match"):
            self.validate()

    def test_invalid_board_rule_delta_is_rejected(self):
        self.mode = "board-bad-delta"
        with self.assertRaisesRegex(ValueError, "unexpected delta"):
            self.validate()

    def test_nonfinite_board_rule_delta_is_rejected(self):
        self.mode = "board-nan"
        with self.assertRaisesRegex(ValueError, "finite sizes"):
            self.validate()

    def test_board_edit_operation_is_required(self):
        self.mode = "missing-board-operation"
        with self.assertRaisesRegex(ValueError, "required successful native operations"):
            self.validate()

    def test_cleanup_failure_cannot_be_reported_as_passed(self):
        self.mode = "delete-failure"
        with self.assertRaisesRegex(RuntimeError, "cleanup/preservation failed"):
            self.validate()
        self.assertFalse(json.loads((self.output / "mixed-simulator.json").read_text())["passed"])

    def test_existing_output_and_overlapping_inputs_are_preserved(self):
        self.output.mkdir()
        sentinel = self.output / "user-work.txt"
        sentinel.write_text("preserve")
        with self.assertRaises(FileExistsError):
            self.validate()
        self.assertEqual(sentinel.read_text(), "preserve")
        with self.assertRaisesRegex(ValueError, "must not overlap"):
            runner.validate(self.binary, self.runtime, self.source, self.fixture, self.runtime / "output")
        self.assertFalse((self.runtime / "output").exists())

    def test_symlinked_runtime_resource_is_rejected(self):
        linked = self.runtime / "linked.py"
        try:
            linked.symlink_to(self.fixture)
        except OSError:
            self.skipTest("Host cannot create symlinks")
        with self.assertRaisesRegex(ValueError, "Linked or special"):
            runner.snapshot(self.runtime)


if __name__ == "__main__":
    unittest.main()
