"""Checks that the OCCT build cannot accept edited sources or viewer libraries."""
import hashlib
import io
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch

import build_occt


class SourceIntegrity(unittest.TestCase):
    def package(self, root, name="OCCT-test/CMakeLists.txt", symlink=False):
        archive = root / "OCCT-V7_8_1.tar.gz"
        with tarfile.open(archive, "w:gz") as package:
            member = tarfile.TarInfo(name)
            if symlink:
                member.type = tarfile.SYMTYPE
                member.linkname = "../../outside"
                package.addfile(member)
            else:
                member.size = 4
                package.addfile(member, io.BytesIO(b"test"))
        return archive

    def test_traversal_and_links_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name, link in [("OCCT-test/../../outside", False), ("OCCT-test/link", True)]:
                archive = self.package(root, name, link)
                with self.assertRaises(ValueError):
                    build_occt.extract(archive, root / "extract", "OCCT-test")
            self.assertFalse((root / "outside").exists())

    def test_checksum_checked_before_extraction(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.package(root)
            with patch.dict(build_occt.PIN, {"directory": "OCCT-test", "sha256": "0" * 64}):
                with self.assertRaisesRegex(ValueError, "checksum"):
                    build_occt.source(root)
            self.assertFalse((root / "OCCT-test").exists())

    def test_cached_source_edits_preserved_and_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = self.package(root)
            pin = {"directory": "OCCT-test", "sha256": hashlib.sha256(archive.read_bytes()).hexdigest()}
            with patch.dict(build_occt.PIN, pin):
                source, _ = build_occt.source(root)
                path = source / "CMakeLists.txt"
                path.write_text("user edit")
                with self.assertRaisesRegex(ValueError, "Modified OCCT source"):
                    build_occt.source(root)
                self.assertEqual(path.read_text(), "user edit")

    def test_extra_source_file_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = self.package(root)
            with patch.dict(build_occt.PIN, {"directory": "OCCT-test", "sha256": hashlib.sha256(archive.read_bytes()).hexdigest()}):
                source, _ = build_occt.source(root)
                (source / "extra.cpp").write_text("unreviewed")
                with self.assertRaisesRegex(ValueError, "Unexpected OCCT source"):
                    build_occt.source(root)


class Closure(unittest.TestCase):
    def test_provider_header_dependency_missing_from_externlib_is_checked(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for toolkit, package, filenames in [
                ("TKDESTEP", "STEPCAFControl", ["STEPCAFControl_Provider.cxx"]),
                ("TKBinXCAF", "BinXCAFDrivers", ["BinXCAFDrivers.hxx"]),
            ]:
                directory = root / "src" / toolkit
                directory.mkdir(parents=True)
                (directory / "PACKAGES").write_text(package + "\n")
                (directory / "EXTERNLIB").write_text("")
                directory = root / "src" / package
                directory.mkdir()
                (directory / "FILES").write_text("\n".join(filenames) + "\n")
                for name in filenames:
                    (directory / name).write_text('#include <BinXCAFDrivers.hxx>\n')
            graph = build_occt.closure(root, ["TKDESTEP"])
            with self.assertRaisesRegex(ValueError, "additional toolkit roots: TKBinXCAF"):
                build_occt.provider_dependencies(root, graph)
            graph = build_occt.closure(root, ["TKDESTEP", "TKBinXCAF"])
            self.assertEqual(build_occt.provider_dependencies(root, graph), {"TKDESTEP": ["TKBinXCAF"]})
            self.assertIn("TKBinXCAF", build_occt.ROOTS)

    def test_transitive_cycles_are_supported_but_drivers_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name, content in {"TKDESTEP": "TKXCAF\n", "TKXCAF": "TKDESTEP\nCSF_objc\n"}.items():
                directory = root / "src" / name
                directory.mkdir(parents=True)
                (directory / "EXTERNLIB").write_text(content)
            self.assertEqual(build_occt.closure(root, ["TKDESTEP"]), {
                "TKDESTEP": ["TKXCAF"], "TKXCAF": ["TKDESTEP"]})
            (root / "src/TKXCAF/EXTERNLIB").write_text("TKOpenGl\n")
            with self.assertRaisesRegex(ValueError, "Viewer/driver"):
                build_occt.closure(root, ["TKDESTEP"])

    def test_unexpected_or_dynamic_archive_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "lib").mkdir()
            (root / "lib/libTKDESTEP.a").write_bytes(b"fixture")
            graph = {"TKDESTEP": []}
            self.assertEqual(len(build_occt.inventory(root, graph)), 1)
            (root / "lib/libTKOpenGl.a").write_bytes(b"viewer")
            with self.assertRaisesRegex(ValueError, "closure"):
                build_occt.inventory(root, graph)
            (root / "lib/libTKOpenGl.a").unlink()
            (root / "lib/libTKDESTEP.dylib").write_bytes(b"dynamic")
            with self.assertRaisesRegex(ValueError, "Dynamic"):
                build_occt.inventory(root, graph)

    def test_simulator_archive_cannot_contain_macos_or_device_members(self):
        header = "cmd LC_BUILD_VERSION\n cmdsize 24\n platform {}\n minos 17.0\n"
        self.assertEqual(build_occt.validate_platform(header.format("7") * 2, "iphonesimulator", "17.0"), 2)
        for other in ("1", "2"):
            with self.assertRaisesRegex(ValueError, "unexpected SDK"):
                build_occt.validate_platform(header.format("7") + header.format(other), "iphonesimulator", "17.0")
        with self.assertRaisesRegex(ValueError, "No archive"):
            build_occt.validate_platform("", "iphoneos", "17.0")


if __name__ == "__main__":
    unittest.main()
