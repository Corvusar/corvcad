"""Cross-platform source integrity and safe evidence paths."""
from pathlib import Path
import tempfile
import unittest

from retain_inventory import add_source, retain, safe_relative


class RetentionTests(unittest.TestCase):
    def test_source_changed_between_platforms_is_not_merged(self):
        files = {}
        add_source(files, "src/App/Document.cpp", {"sha256": "linux"}, "translation-unit", "linux")
        with self.assertRaisesRegex(ValueError, "differ"):
            add_source(files, "src/App/Document.cpp", {"sha256": "macos"}, "translation-unit", "macos")

    def test_archive_paths_cannot_escape_evidence_root(self):
        for name in ("../LICENSE", "/LICENSE", "src/../../LICENSE", "C:/LICENSE", "..\\LICENSE"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                safe_relative(name)

    def test_existing_evidence_is_preserved(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            (output/"user.json").write_text("preserve")
            with self.assertRaises(FileExistsError):
                retain({}, output, "unused", "unused")
            self.assertEqual((output/"user.json").read_text(), "preserve")


if __name__ == "__main__":
    unittest.main()
