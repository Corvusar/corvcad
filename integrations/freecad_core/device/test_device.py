"""Protocol/packet regressions only; synthetic replies never establish device or geometry acceptance."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import subprocess
import sys
import unittest
from unittest.mock import patch
import urllib.error

from collect import OUTPUTS, Probe, collect, validate_reply
from desktop_check import verify_collection
from packet import inside, prepare, record, verify


class FixtureProbe:
    """Deterministic transport fixture; every output is deliberately fake, not FreeCAD geometry."""
    token = "unit-test-token"

    def __init__(self, expected):
        self.expected = expected
        self.uploads = {}
        self.warm = False
        self.corrupt = False
        self.hardware = "simulator"
        self.pid = 123
        self.info_calls = 0
        self.reply = self.make_reply()

    def make_reply(self):
        files = {name: b"SYNTHETIC TEST FIXTURE: " + name.encode() for name in OUTPUTS}
        files["input.json"] = json.dumps(self.expected).encode()
        self.outputs = files
        report = {"schema": 1, "passed": True, "f5_accepted": False, "hardware_class": self.hardware,
                  "process_id": self.pid, "input": self.expected, "settings_mm": {},
                  "runtime_initialized_before": self.warm,
                  "operations": dict.fromkeys(("parameter_restore", "undo", "redo", "rejected_edit_rollback",
                                               "save_reopen_byte_preservation", "step", "stl"), True),
                  "files": {name: {"bytes": len(value), "sha256": hashlib.sha256(value).hexdigest()} for name,value in files.items()},
                  "parameter_timings": [{"parameter": name, "edit_recompute_ms": 2, "restore_recompute_ms": 3}
                      for name in ("margin", "wall", "floor", "board_clearance", "standoff_height", "standoff_diameter", "lid_thickness", "lid_gap")]}
        self.outputs["device-report.json"] = json.dumps(report).encode()
        return {**report, "footprint_before": 100, "footprint_after": 200, "footprint_peak_during": 300, "wall_ms": 10}

    def call(self, route, data=None, **query):
        if route == "/metrics":
            return {"machine": "x86_64", "uptime_ms": 1000, "footprint": 100, "footprint_peak": 300}
        if route == "/run" and query["command"] == "mechanical-info":
            self.info_calls += 1
            return {"native_available": True, "process_id": self.pid, "hardware_class": self.hardware,
                    "runtime_initialized": self.warm}
        if route == "/ls":
            return []
        if route == "/put":
            self.uploads[query["path"]] = data
            return {"written": True, "bytes": len(data)}
        if route == "/get":
            if query["path"] in self.uploads:
                return self.uploads[query["path"]]
            name = query["path"].split("/")[-1]
            return self.outputs[name] + (b"bad" if self.corrupt and name == "tray.step" else b"")
        if route == "/run":
            return self.reply
        if route == "/snapshot":
            return b"\x89PNG\r\n\x1a\nSYNTHETIC"
        if route == "/timeline":
            return {"interval_ms":100, "samples":[[1000,100],[1100,300]]}
        return {"synthetic": True}


class DevicePacketTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.packet = self.root / "packet"
        self.manifest = prepare(self.packet)
        self.expected = json.loads((self.packet / "demo/expected-input.json").read_text())
        self.probe = FixtureProbe(self.expected)

    def test_packet_retains_actual_board_bytes_and_refuses_overwrite(self):
        self.assertEqual(verify(self.packet)["run_id"], self.manifest["run_id"])
        self.assertEqual(self.expected["source"]["sha256"], record(self.packet / "demo/demo.kicad_pcb")["sha256"])
        with self.assertRaises(FileExistsError):
            prepare(self.packet)

    def test_packet_detects_modified_sources(self):
        (self.packet / "demo/demo.kicad_pcb").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            verify(self.packet)

    def test_packet_path_cannot_escape_root(self):
        for path in ("../outside", "/absolute", "demo/../../outside", "C:/outside", "demo\\outside"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                inside(self.packet, path)

    def test_synthetic_simulator_collection_is_never_physical_acceptance(self):
        output = self.root / "result"
        result = collect(self.packet, "demo", output, self.probe, hardware="simulator", fresh_launch=True)
        self.assertFalse(result["f5_accepted"])
        self.assertEqual(result["hardware_class"], "simulator")
        self.assertEqual(verify_collection(output)["status"], result["status"])
        self.assertEqual(set(self.probe.uploads), {f"Gate5/{self.manifest['run_id']}/demo/{name}" for name in self.manifest["cases"]["demo"]["files"]})
        self.assertNotIn(self.probe.token, (output / "collection.json").read_text())

    def test_simulator_fails_physical_request_before_upload(self):
        with self.assertRaisesRegex(ValueError, "hardware class"):
            collect(self.packet, "demo", self.root / "result", self.probe)
        self.assertFalse(self.probe.uploads)

    def test_fresh_launch_refuses_preinitialized_runtime(self):
        self.probe.warm = True
        with self.assertRaisesRegex(ValueError, "already initialized"):
            collect(self.packet, "demo", self.root / "result", self.probe, hardware="simulator", fresh_launch=True)

    def test_corrupted_download_retains_failed_receipt(self):
        self.probe.corrupt = True
        output = self.root / "result"
        with self.assertRaisesRegex(ValueError, "device hash"):
            collect(self.packet, "demo", output, self.probe, hardware="simulator")
        self.assertEqual(json.loads((output / "collection.json").read_text())["status"], "failed")

    def test_desktop_preflight_rejects_tampered_artifact(self):
        output = self.root / "result"
        collect(self.packet, "demo", output, self.probe, hardware="simulator")
        (output / "tray.step").write_bytes(b"tampered")
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            verify_collection(output)

    def test_missing_operation_and_nan_timing_rejected(self):
        reply = copy.deepcopy(self.probe.reply)
        reply["operations"].pop("undo")
        with self.assertRaisesRegex(ValueError, "operations"):
            validate_reply(reply, self.expected, "simulator", {})
        reply = copy.deepcopy(self.probe.reply)
        reply["parameter_timings"][0]["edit_recompute_ms"] = float("nan")
        with self.assertRaisesRegex(ValueError, "timing"):
            validate_reply(reply, self.expected, "simulator", {})

    def test_transport_errors_do_not_expose_token(self):
        probe = Probe("127.0.0.1", "private-token")
        error = urllib.error.HTTPError("http://127.0.0.1/?token=private-token", 403, "Forbidden", {}, None)
        with patch("urllib.request.urlopen", side_effect=error):
            with self.assertRaisesRegex(RuntimeError, r"HTTP 403") as caught:
                probe.call("/metrics")
        self.assertNotIn("private-token", str(caught.exception))

    def test_process_restart_fails_and_retains_logs(self):
        output = self.root / "result"
        original = self.probe.call
        def restart(route, **query):
            value = original(route, **query)
            if route == "/run" and query["command"] == "mechanical-info" and self.probe.info_calls > 1:
                value["process_id"] += 1
            return value
        self.probe.call = restart
        with self.assertRaisesRegex(ValueError, "restarted"):
            collect(self.packet, "demo", output, self.probe, hardware="simulator")
        self.assertTrue((output / "device-log.json").is_file())
        self.assertTrue((output / "memory-timeline.json").is_file())

    def test_missing_memory_samples_fail_collection(self):
        original = self.probe.call
        def no_samples(route, **query):
            return {"interval_ms":100,"samples":[]} if route == "/timeline" else original(route, **query)
        self.probe.call = no_samples
        with self.assertRaisesRegex(ValueError, "footprint samples"):
            collect(self.packet, "demo", self.root / "result", self.probe, hardware="simulator")

    def test_desktop_script_bootstraps_imports_like_freecadcmd(self):
        output = self.root / "result"
        collect(self.packet, "demo", output, self.probe, hardware="simulator")
        script = Path(__file__).with_name("desktop_check.py").resolve()
        # FreeCADCmd executes a file without Python's normal script-directory insertion.
        run = subprocess.run([sys.executable, "-I", "-c",
            "import runpy,sys; from pathlib import Path; namespace=runpy.run_path(sys.argv[1]); namespace['verify_collection'](Path(sys.argv[2]))",
            str(script), str(output)], cwd=self.root, capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
