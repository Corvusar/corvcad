"""A failed build must still produce an honest partial inventory."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import inventory


class InventoryTests(unittest.TestCase):
    def check(self, relative, expected_error=None):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, build, output = (root/name for name in ("source", "build", "output"))
            source.mkdir()
            build.mkdir()
            file = (build if relative == "generated.cpp" else source)/relative
            (build/"compile_commands.json").write_text(json.dumps([
                {"directory": str(build), "file": str(file),
                 "arguments": ["c++", "-o", "object.o", "-c", str(file)]}]))
            with patch.object(inventory, "capture", return_value={"exit": 0, "stdout": "", "stderr": ""}):
                if expected_error:
                    with self.assertRaises(expected_error):
                        inventory.collect(source, build, output)
                    return
                inventory.collect(source, build, output)
            return json.loads((output/"inventory.json").read_text())

    def test_unbuilt_generated_source_is_explicitly_partial(self):
        report = self.check("generated.cpp")
        self.assertEqual(report["status"], "partial-build")
        self.assertEqual(report["produced_objects"], 0)
        self.assertIsNone(report["translation_units"][0]["sha256"])
        self.assertFalse(report["translation_units"][0]["source_produced"])

    def test_missing_upstream_source_is_not_disguised_as_generated(self):
        self.check("missing.cpp", FileNotFoundError)

    def test_gui_source_is_rejected(self):
        self.check("Gui/View.cpp", RuntimeError)

    def test_gui_links_are_detected_on_elf_and_macos_frameworks(self):
        for link in ("libQt6Gui.so.6", "/QtWidgets.framework/Versions/A/QtWidgets",
                     "/Coin.framework/Versions/C/Coin", "libQuarter.dylib", "libpyside6.abi3.so"):
            with self.subTest(link=link):
                self.assertTrue(inventory.gui_dependency(link))
        self.assertFalse(inventory.gui_dependency("libQt6Core.so.6 /QtConcurrent.framework/QtConcurrent libTKV3d.so"))

    def test_missing_link_target_is_recorded_even_when_objects_exist(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary).resolve()
            reply = build/".cmake/api/v1/reply"
            reply.mkdir(parents=True)
            (reply/"index-1.json").write_text(json.dumps({"objects": [{"kind": "codemodel", "jsonFile": "model.json"}]}))
            (reply/"model.json").write_text(json.dumps({"configurations": [{"targets": [{"jsonFile": "import.json"}]}]}))
            (reply/"import.json").write_text(json.dumps({"name": "Import", "type": "SHARED_LIBRARY",
                                                        "artifacts": [{"path": "Mod/Import/Import.so"}]}))
            source = build/"source"
            source.mkdir()
            (build/"generated.cpp").write_text("int generated;\n")
            (build/"object.o").write_bytes(b"compiled object")
            (build/"bin").mkdir()
            (build/"bin/FreeCADCmd").write_bytes(b"existing core executable")
            (build/"compile_commands.json").write_text(json.dumps([
                {"directory": str(build), "file": str(build/"generated.cpp"),
                 "arguments": ["c++", "-o", "object.o", "-c", str(build/"generated.cpp")]}]))
            with patch.object(inventory, "capture", return_value={"exit": 0, "stdout": "", "stderr": ""}):
                report = inventory.collect(source, build, build/"evidence")
            self.assertEqual(report["produced_objects"], 1)
            self.assertEqual(report["status"], "partial-build")
            self.assertEqual(report["target_artifacts"][0]["target"], "Import")
            self.assertFalse(report["target_artifacts"][0]["produced"])
            self.assertIsNone(report["target_artifacts"][0]["sha256"])


if __name__ == "__main__":
    unittest.main()
