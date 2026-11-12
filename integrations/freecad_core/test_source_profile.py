"""Source-preservation regressions for the F2 patch applicator."""
import difflib
import hashlib
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import slice


class SourceProfileTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "FreeCAD"
        self.source.mkdir()
        self.tool = self.root / "tool"
        self.tool.mkdir()
        self.git("init", "-q")
        self.git("config", "core.autocrlf", "false")
        (self.source / "edit.txt").write_bytes(b"original\n")
        (self.source / "user.txt").write_bytes(b"preserve\n")
        self.git("add", "edit.txt", "user.txt")
        self.git("-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-qm", "fixture")
        self.pin = self.git("rev-parse", "HEAD")
        text = "".join(difflib.unified_diff(["original\n"], ["patched\n"], fromfile="a/edit.txt", tofile="b/edit.txt"))
        (self.tool / "change.patch").write_text(text, newline="\n")
        self.profile = {"patches": [{"path": "change.patch", "sha256": slice.digest(self.tool/"change.patch"),
                                     "files": {"edit.txt": {"before_sha256_lf": hashlib.sha256(b"original\n").hexdigest(),
                                                            "after_sha256_lf": hashlib.sha256(b"patched\n").hexdigest()}}}]}
        for context in (patch.object(slice, "HERE", self.tool), patch.object(slice, "PROFILE", self.profile),
                        patch.dict(slice.fetch.LOCK, {"source": {"commit": self.pin}})):
            context.start()
            self.addCleanup(context.stop)

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.source), *args], text=True).strip()

    def test_applies_complete_series_and_is_idempotent(self):
        slice.prepare(self.source)
        self.assertEqual((self.source/"edit.txt").read_text(), "patched\n")
        slice.prepare(self.source)
        self.assertEqual((self.source/"user.txt").read_text(), "preserve\n")

    def test_unrelated_user_edit_is_preserved(self):
        (self.source/"user.txt").write_text("user change\n")
        with self.assertRaisesRegex(RuntimeError, "Undeclared"):
            slice.prepare(self.source)
        self.assertEqual((self.source/"user.txt").read_text(), "user change\n")

    def test_modified_patch_result_is_preserved(self):
        (self.source/"edit.txt").write_text("partial user change\n")
        with self.assertRaisesRegex(RuntimeError, "Partial"):
            slice.prepare(self.source)
        self.assertEqual((self.source/"edit.txt").read_text(), "partial user change\n")

    def test_untracked_file_is_preserved(self):
        path = self.source/"private.txt"
        path.write_text("user data")
        with self.assertRaisesRegex(RuntimeError, "Undeclared"):
            slice.prepare(self.source)
        self.assertEqual(path.read_text(), "user data")

    def test_wrong_source_commit_is_rejected(self):
        with patch.dict(slice.fetch.LOCK, {"source": {"commit": "wrong"}}):
            with self.assertRaisesRegex(RuntimeError, "pin"):
                slice.prepare(self.source)

    def test_tampered_patch_is_rejected_before_edits(self):
        (self.tool/"change.patch").write_text("tampered")
        with self.assertRaisesRegex(RuntimeError, "hash"):
            slice.prepare(self.source)
        self.assertEqual((self.source/"edit.txt").read_text(), "original\n")

    def test_retained_license_accepts_platform_line_endings(self):
        path = self.tool/"LICENSE.txt"
        path.write_bytes(b"retained licence\r\n")
        self.profile["retained_license"] = {"path": path.name,
                                            "sha256_lf": hashlib.sha256(b"retained licence\n").hexdigest()}
        slice.prepare(self.source)
        self.assertEqual((self.source/"edit.txt").read_text(), "patched\n")

    def test_modified_retained_license_rejects_before_source_edits(self):
        path = self.tool/"LICENSE.txt"
        path.write_bytes(b"modified licence\n")
        self.profile["retained_license"] = {"path": path.name,
                                            "sha256_lf": hashlib.sha256(b"retained licence\n").hexdigest()}
        with self.assertRaisesRegex(RuntimeError, "licence hash"):
            slice.prepare(self.source)
        self.assertEqual((self.source/"edit.txt").read_text(), "original\n")

    def test_future_module_defaults_are_disabled(self):
        directory = self.source/"cMake/FreeCAD_Helpers"
        directory.mkdir(parents=True)
        (directory/"InitializeFreeCADBuildOptions.cmake").write_text(
            'option(BUILD_NEW "Build the FreeCAD future module" ON)\n'
            'option(BUILD_PART "Build the FreeCAD part module" ON)\n')
        with patch.object(slice, "PROFILE", {"enabled_modules": ["BUILD_PART"]}):
            flags = slice.options(self.source)
        self.assertIn("-DBUILD_NEW=OFF", flags)
        self.assertIn("-DBUILD_PART=ON", flags)


if __name__ == "__main__":
    unittest.main()
