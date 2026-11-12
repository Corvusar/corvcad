"""Intel simulator opt-in and architecture/cache boundary regressions."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import build_occt
import cross
import run_ios
import target_config


class TargetSelection(unittest.TestCase):
    def test_default_remains_arm64_for_both_sdks(self):
        self.assertEqual(target_config.target("iphoneos").triplet, "arm64-ios")
        self.assertEqual(target_config.target("iphonesimulator").triplet, "arm64-ios-simulator")
        self.assertEqual(target_config.target("iphoneos").compiler_target, "arm64-apple-ios17.0")

    def test_intel_is_explicit_simulator_only_and_host_is_native(self):
        selected = target_config.target("iphonesimulator", "x86_64")
        self.assertEqual(selected.triplet, "x64-ios-simulator")
        self.assertEqual(selected.compiler_target, "x86_64-apple-ios17.0-simulator")
        self.assertEqual(target_config.from_triplet(selected.triplet), selected)
        self.assertEqual(target_config.host_triplet("x86_64"), "x64-osx")
        self.assertEqual(target_config.host_triplet("arm64"), "arm64-osx")
        with self.assertRaisesRegex(ValueError, "simulator-only"):
            target_config.target("iphoneos", "x86_64")
        with self.assertRaises(ValueError):
            target_config.from_triplet("x64-ios")

    def test_occt_compiler_architecture_is_selected_explicitly(self):
        command = build_occt.command(Path("source"), Path("build"), Path("stage"),
                                     "iphonesimulator", "17.0", "x86_64")
        self.assertIn("-DCMAKE_OSX_ARCHITECTURES=x86_64", command)
        self.assertNotIn("-DCMAKE_OSX_ARCHITECTURES=arm64", command)

    def test_target_cache_cannot_be_reused_for_a_different_architecture(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "sdk"
            selected = target_config.target("iphonesimulator", "x86_64")
            target_config.claim(root, selected)
            target_config.claim(root, selected)
            with self.assertRaisesRegex(ValueError, "another SDK/architecture"):
                target_config.claim(root, target_config.target("iphonesimulator"))

    def test_legacy_arm64_report_is_accepted_only_for_its_original_target(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "python-build.json").write_text(json.dumps({
                "sdk": "iphonesimulator", "target": "arm64-apple-ios17.0-simulator"}))
            with self.assertRaisesRegex(ValueError, "another SDK/architecture"):
                target_config.claim(root, target_config.target("iphonesimulator", "x86_64"), "python-build.json")
            self.assertFalse((root / "corvcad-target.json").exists())
            target_config.claim(root, target_config.target("iphonesimulator"), "python-build.json")

    def test_unidentified_existing_cache_is_preserved_and_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "object.o").write_bytes(b"existing work")
            with self.assertRaisesRegex(ValueError, "Unidentified existing target"):
                target_config.claim(root, target_config.target("iphonesimulator", "x86_64"))
            self.assertEqual((root / "object.o").read_bytes(), b"existing work")

    def test_legacy_occt_cache_uses_actual_archive_hash_and_architecture(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "stage/lib/libTKDESTEP.a"
            archive.parent.mkdir(parents=True)
            archive.write_bytes(b"archive fixture")
            report = {"sdk": "iphonesimulator", "archives": {
                "lib/libTKDESTEP.a": {"sha256": build_occt.digest(archive)}}}
            (root / "occt-build.json").write_text(json.dumps(report))
            with patch.object(build_occt.subprocess, "check_output", return_value="x86_64"):
                with self.assertRaisesRegex(ValueError, "another SDK/architecture"):
                    target_config.claim(root, target_config.target("iphonesimulator"), "occt-build.json", build_occt.cached_architecture)
            self.assertFalse((root / "corvcad-target.json").exists())
            with patch.object(build_occt.subprocess, "check_output", return_value="arm64"):
                target_config.claim(root, target_config.target("iphonesimulator"), "occt-build.json", build_occt.cached_architecture)
            archive.write_bytes(b"externally changed")
            with self.assertRaisesRegex(ValueError, "recorded hash"):
                build_occt.cached_architecture(root, report)

    def test_cross_inputs_reject_python_from_the_other_architecture(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            directory = root / "python/iphonesimulator"
            directory.mkdir(parents=True)
            (directory / "python-build.json").write_text(json.dumps({
                "sdk": "iphonesimulator", "target": "arm64-apple-ios17.0-simulator",
                "architecture": "arm64", "source": cross.LOCK["python"]}))
            with self.assertRaisesRegex(ValueError, "CPython SDK"):
                cross.inputs(root, "iphonesimulator", "x86_64")

    def test_requested_intel_runtime_rejects_arm64_manifest_before_execution(self):
        with patch.object(run_ios.platform, "system", return_value="Darwin"), \
                patch.object(run_ios, "verify_runtime", return_value={"architecture": "arm64"}):
            with self.assertRaisesRegex(ValueError, "Staged runtime architecture"):
                run_ios.run(Path("runtime"), "iphonesimulator", Path("output"), Path("references"), "x86_64")


if __name__ == "__main__":
    unittest.main()
