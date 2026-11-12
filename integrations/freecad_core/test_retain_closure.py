"""Failed or differently pinned artifacts must never become accepted lock inputs."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import retain_closure


class RetainClosureTests(unittest.TestCase):
    def test_different_source_pin_is_rejected_before_copying_notices(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root/"slice.json").write_bytes(b"profile")
            (root/"license-policy.json").write_bytes(b"policy")
            (root/"upstream.json").write_text(json.dumps({"source": {"commit": "expected"}}))
            archive = root/"artifact.zip"
            with zipfile.ZipFile(archive, "w") as bundle:
                bundle.writestr("evidence/closure.json", json.dumps({"source_commit": "other",
                    "profile_sha256": hashlib.sha256(b"profile").hexdigest()}))
            with patch.object(retain_closure, "HERE", root), self.assertRaisesRegex(ValueError, "source/profile"):
                retain_closure.retain({"linux": archive}, root/"output", "run", "commit")

    def test_existing_review_material_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            (output/"review.txt").write_text("keep")
            with self.assertRaises(FileExistsError):
                retain_closure.retain({}, output, "run", "commit")
            self.assertEqual((output/"review.txt").read_text(), "keep")


if __name__ == "__main__":
    unittest.main()
