"""Detect source/notice tampering without modifying the checkout or evidence."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from verify_inventory import check_file, verify


class VerifyTests(unittest.TestCase):
    def test_source_line_endings_and_escape_detection(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "input").write_bytes(b"source\r\n")
            expected = hashlib.sha256(b"source\n").hexdigest()
            check_file(root, "input", expected, True)
            check_file(root, "input", hashlib.sha256(b"source\r\n").hexdigest(), True)
            with self.assertRaisesRegex(ValueError, "checksum"):
                check_file(root, "input", expected)
            with self.assertRaises(ValueError):
                check_file(root, "../input", expected)
            (root/"binary").write_bytes(b"\0data\r\n")
            with self.assertRaisesRegex(ValueError, "checksum"):
                check_file(root, "binary", hashlib.sha256(b"\0data\n").hexdigest(), True)

    def test_snapshot_verifies_and_detects_source_and_notice_edits(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, integration = root / "source", root / "integration"
            audit = integration / "audit"
            source.mkdir()
            (audit / "notices").mkdir(parents=True)
            (source / "input.cpp").write_bytes(b"original\n")
            subprocess.run(["git", "init", "-q", str(source)], check=True)
            subprocess.run(["git", "-C", str(source), "add", "."], check=True)
            subprocess.run(["git", "-C", str(source), "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                            "commit", "-qm", "fixture"], check=True)
            pin = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
            def write(name, value):
                (integration / name).write_text(json.dumps(value), encoding="utf-8")
            def sha(data):
                return hashlib.sha256(data).hexdigest()
            (integration / "LICENSE").write_bytes(b"license")
            (audit / "notices/LICENSE").write_bytes(b"notice")
            write("upstream.json", {"source": {"commit": pin}})
            write("slice.json", {"patches": [], "retained_license": {"path": "LICENSE", "sha256_lf": sha(b"license")}})
            profile_sha = sha((integration / "slice.json").read_bytes())
            (audit / "desktop-inputs.json").write_text(json.dumps({
                "source_commit": pin, "profile_sha256": profile_sha,
                "source_files": {"input.cpp": {"sha256": sha(b"original\n")}},
                "source_tree_notices": {"LICENSE": sha(b"notice")}}))
            before = (source / ".git/index").read_bytes()
            self.assertEqual(verify(source, integration=integration)["source_inputs"], 1)
            self.assertEqual(before, (source / ".git/index").read_bytes())
            (source / "input.cpp").write_bytes(b"user change\n")
            with self.assertRaisesRegex(ValueError, "undeclared"):
                verify(source, integration=integration)
            self.assertEqual((source / "input.cpp").read_bytes(), b"user change\n")
            (source / "input.cpp").write_bytes(b"original\n")
            # Per-file hashes must catch changes hidden from Git's stat scan.
            subprocess.run(["git", "-C", str(source), "update-index", "--assume-unchanged", "input.cpp"], check=True)
            (source / "input.cpp").write_bytes(b"hidden change\n")
            before = (source / ".git/index").read_bytes()
            with self.assertRaisesRegex(ValueError, "checksum"):
                verify(source, integration=integration)
            self.assertEqual(before, (source / ".git/index").read_bytes())
            self.assertEqual((source / "input.cpp").read_bytes(), b"hidden change\n")
            (source / "input.cpp").write_bytes(b"original\n")
            subprocess.run(["git", "-C", str(source), "update-index", "--no-assume-unchanged", "input.cpp"], check=True)
            (audit / "notices/LICENSE").write_bytes(b"tampered")
            with self.assertRaisesRegex(ValueError, "checksum"):
                verify(source, integration=integration)


if __name__ == "__main__":
    unittest.main()
