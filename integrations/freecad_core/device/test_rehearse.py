"""Simulator-runner preflight contracts with synthetic data; no runtime acceptance."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

from collect import validate_reply
from packet import prepare, save
from rehearse import copy_outputs, inventory, validate_rehearsal, verify_package_manifest
from test_device import FixtureProbe


class RehearsalTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        packet = self.root / "packet"
        prepare(packet)
        self.expected = json.loads((packet / "demo/expected-input.json").read_text())
        self.fixture = FixtureProbe(self.expected)
        self.resources = {"harness/"+name:{"bytes":1,"sha256":"a"*64} for name in (
            "upstream.json", "session_runtime.py", "model.py", "inputs.py", "checks.py")}
        self.reply = {key:value for key,value in self.fixture.reply.items()
                      if key not in ("footprint_before", "footprint_after", "footprint_peak_during", "wall_ms")}
        self.reply.update(rehearsal_only=True, physical_footprint_measured=False, native_available=True,
                          runtime_sources=self.resources,
                          settings_mm=dict.fromkeys(("margin","wall","floor","board_clearance","standoff_height",
                                                     "standoff_diameter","lid_thickness","lid_gap"),2))

    def test_rehearsal_requires_no_fabricated_physical_measurements(self):
        self.assertIs(validate_rehearsal(self.reply,self.expected,self.resources), self.reply)
        with self.assertRaisesRegex(ValueError,"process measurement"):
            validate_reply(self.reply,self.expected,"simulator",{})

    def test_classification_cold_and_scope_flags_are_strict(self):
        for key,value in (("hardware_class","physical-device"),("runtime_initialized_before",True),
                          ("physical_footprint_measured",True),("rehearsal_only",False),
                          ("f5_accepted",True),("native_available",False)):
            with self.subTest(key=key), self.assertRaises(ValueError):
                reply = copy.deepcopy(self.reply)
                reply[key] = value
                validate_rehearsal(reply,self.expected,self.resources)

    def test_running_helper_identity_must_match_app_bundle(self):
        reply = copy.deepcopy(self.reply)
        reply["runtime_sources"]["harness/model.py"]["sha256"] = "b"*64
        with self.assertRaisesRegex(ValueError,"helper identity"):
            validate_rehearsal(reply,self.expected,self.resources)

    def test_package_receipt_must_match_actual_resources(self):
        path = self.root / "package.json"
        save(path,{"schema":1,"sdk":"iphonesimulator","resources":self.resources})
        verify_package_manifest(path,self.resources)
        changed = copy.deepcopy(self.resources)
        changed["harness/model.py"]["bytes"] += 1
        with self.assertRaisesRegex(ValueError,"differs"):
            verify_package_manifest(path,changed)

    def test_actual_exports_are_hash_checked_before_copy(self):
        source, target = self.root/"outputs", self.root/"retained"
        source.mkdir(); target.mkdir()
        for name,data in self.fixture.outputs.items():
            (source/name).write_bytes(data)
        # Fixture's durable report is the subset carried by its notification reply.
        copy_outputs(source,target,self.fixture.reply)
        (source/"tray.step").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError,"reported hash"):
            copy_outputs(source,target,self.fixture.reply)

    def test_empty_resource_inventory_rejected(self):
        empty = self.root/"empty"
        empty.mkdir()
        with self.assertRaisesRegex(ValueError,"Empty"):
            inventory(empty)


if __name__ == "__main__":
    unittest.main()
