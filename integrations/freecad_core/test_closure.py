"""Generator traversal and package ownership must not silently omit inputs."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock
import zipfile

from closure import archive_notices, brew_owner, debian_owners, graph_files, links


class ClosureTests(unittest.TestCase):
    def test_graph_keeps_files_and_skips_rule_nodes(self):
        graph = '''"0x1" [label="../FreeCAD/schema.xml"];
"0x2" [label="CUSTOM_COMMAND", shape=ellipse];
"0x3" [label="generated.cpp"];
"0x1" -> "0x2" [arrowhead=none];
"0x2" -> "0x3";
'''
        self.assertEqual(graph_files(graph), ["../FreeCAD/schema.xml", "generated.cpp"])

    def test_merged_usr_and_multiarch_package_owners(self):
        # Colons in real dpkg database names are NTFS alternate streams, so
        # exercise the Linux database layout without creating those on Windows.
        listing = Mock()
        listing.name = "libc6:amd64.list"
        listing.read_text.return_value = "/lib/x86_64-linux-gnu/libc.so.6\n/usr/share/doc/libc6/copyright\n"
        root = Mock()
        root.glob.return_value = [listing]
        path = Path("/usr/lib/x86_64-linux-gnu/libc.so.6")
        self.assertEqual(debian_owners([path], root)[path.as_posix()], {"libc6:amd64"})

    def test_runtime_links_and_unresolved_library(self):
        self.assertEqual(links("libx.so => /usr/lib/libx.so (0x1)\n/lib/ld.so (0x2)", "Linux"),
                         ["/usr/lib/libx.so", "/lib/ld.so"])
        self.assertEqual(links("binary:\n\t@rpath/libx.dylib (compatibility version 1.0.0)\n", "Darwin"),
                         ["@rpath/libx.dylib"])
        with self.assertRaises(ValueError):
            links("libx.so => not found", "Linux")

    def test_brew_ownership_uses_actual_installed_version(self):
        self.assertEqual(brew_owner(Path("/opt/homebrew/Cellar/qtbase/6.9.1/lib/QtCore"))[:2], ("qtbase", "6.9.1"))
        self.assertIsNone(brew_owner(Path("/System/Library/Frameworks/CoreFoundation")))

    def test_archive_notice_reader_cannot_escape_or_copy_arbitrary_code(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)/"source.zip"
            with zipfile.ZipFile(path, "w") as bundle:
                bundle.writestr("source/LICENSE", "grant")
                bundle.writestr("source/code.cpp", "code")
            saved = []
            archive_notices(path, lambda name, data: saved.append((name, data)))
            self.assertEqual(saved, [("source/LICENSE", b"grant")])
            with zipfile.ZipFile(path, "w") as bundle:
                bundle.writestr("../LICENSE", "escape")
            with self.assertRaises(ValueError):
                archive_notices(path, lambda name, data: saved.append((name, data)))


if __name__ == "__main__":
    unittest.main()
