"""Exercise patch application against real temporary Git repositories."""
import difflib
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import prepare_static as static
import run_static


def sha(text):
    return hashlib.sha256(text.encode()).hexdigest()


class StaticPreparation(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.here = self.root / "integration/ios"
        self.here.mkdir(parents=True)
        self.source = self.root / "FreeCAD"
        self.source.mkdir()
        subprocess.run(["git", "init", "-q", str(self.source)], check=True)
        subprocess.run(["git", "-C", str(self.source), "config", "core.autocrlf", "false"], check=True)
        # A CRLF blob in the repository reproduces FreeCAD's mixed source formats.
        self.original = "first\nsecond\n"
        self.modified = "first\nstatic second\n"
        (self.source / "source.cpp").write_bytes(self.original.replace("\n", "\r\n").encode())
        subprocess.run(["git", "-C", str(self.source), "add", "source.cpp"], check=True)
        subprocess.run(["git", "-C", str(self.source), "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                        "commit", "-qm", "Fixture"], check=True)
        commit = subprocess.check_output(["git", "-C", str(self.source), "rev-parse", "HEAD"], text=True).strip()
        (self.here.parent / "licence.txt").write_bytes(b"fixture notice\n")
        base = {"patches": [], "retained_license": {"path": "licence.txt", "sha256_lf": sha("fixture notice\n")}}
        (self.here.parent / "slice.json").write_text(json.dumps(base))
        (self.here / "static_registry.cpp").write_text("// registry fixture\n")
        diff = "".join(difflib.unified_diff(self.original.splitlines(True), self.modified.splitlines(True),
                                           fromfile="a/source.cpp", tofile="b/source.cpp"))
        (self.here / "static.patch").write_bytes(diff.encode())
        profile = {"desktop_profile_sha256_lf": static.digest(self.here.parent / "slice.json"),
                   "registry_sha256_lf": static.digest(self.here / "static_registry.cpp"),
                   "patch": {"path": "static.patch", "sha256": sha(diff), "files": {
                       "source.cpp": {"before_sha256_lf": sha(self.original), "after_sha256_lf": sha(self.modified)}}}}
        (self.here / "static-profile.json").write_text(json.dumps(profile))
        for target, name, value in [(static, "HERE", self.here), (static, "PROFILE", profile),
                                    (static.desktop, "HERE", self.here.parent), (static.desktop, "PROFILE", base),
                                    (static.desktop.fetch, "LOCK", {"source": {"commit": commit}})]:
            manager = patch.object(target, name, value)
            manager.start()
            self.addCleanup(manager.stop)

    def test_crlf_source_application_and_reentry(self):
        first = static.prepare(self.source)
        before = (self.source / "source.cpp").read_bytes()
        second = static.prepare(self.source)
        self.assertEqual(first, second)
        self.assertEqual((self.source / "source.cpp").read_bytes(), before)
        self.assertEqual((self.source / "source.cpp").read_text(), self.modified)

    def test_source_drift_is_preserved(self):
        static.prepare(self.source)
        path = self.source / "source.cpp"
        path.write_text(self.modified + "unintended edit\n")
        before = path.read_bytes()
        with self.assertRaisesRegex((ValueError, RuntimeError), "Modified|Partial/modified|Undeclared"):
            static.prepare(self.source)
        self.assertEqual(path.read_bytes(), before)

    def test_untracked_input_is_preserved(self):
        static.prepare(self.source)
        path = self.source / "extra.cpp"
        path.write_text("untracked input\n")
        with self.assertRaisesRegex(ValueError, "Untracked"):
            static.prepare(self.source)
        self.assertEqual(path.read_text(), "untracked input\n")

    def test_patch_corruption_rejected_before_mutation(self):
        original = (self.source / "source.cpp").read_bytes()
        with (self.here / "static.patch").open("ab") as stream:
            stream.write(b"modified\n")
        with self.assertRaisesRegex(ValueError, "patch checksum"):
            static.prepare(self.source)
        self.assertEqual((self.source / "source.cpp").read_bytes(), original)

    def test_changed_registry_rejected(self):
        (self.here / "static_registry.cpp").write_text("// modified registry\n")
        with self.assertRaisesRegex(ValueError, "registry changed"):
            static.prepare(self.source)

    def test_changed_desktop_base_rejected(self):
        with (self.here.parent / "slice.json").open("a") as stream:
            stream.write("\n")
        with self.assertRaisesRegex(ValueError, "base profile changed"):
            static.prepare(self.source)


class StaticArtifactBoundary(unittest.TestCase):
    def records(self):
        names = ["FreeCADBase", "FreeCADApp", "E57Format", *static.PROFILE["module_targets"]]
        return [{"target": name, "type": "STATIC_LIBRARY", "produced": True} for name in names] + [
            {"target": "FreeCADMainCmd", "type": "EXECUTABLE", "produced": True}]

    def test_static_closure_accepted(self):
        self.assertEqual(run_static.check_artifacts(self.records())["target"], "FreeCADMainCmd")

    def test_shared_module_rejected(self):
        records = self.records()
        records[0]["type"] = "SHARED_LIBRARY"
        with self.assertRaisesRegex(ValueError, "not produced"):
            run_static.check_artifacts(records)

    def test_unbuilt_archive_rejected(self):
        records = self.records()
        records[0]["produced"] = False
        with self.assertRaisesRegex(ValueError, "not produced"):
            run_static.check_artifacts(records)

    def test_unexpected_module_rejected(self):
        records = self.records() + [{"target": "Measure", "type": "STATIC_LIBRARY", "produced": True}]
        with self.assertRaisesRegex(ValueError, "Unexpected"):
            run_static.check_artifacts(records)


@unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja"), "CMake and Ninja required")
class StaticImportDependencies(unittest.TestCase):
    """Configure the patch's actual Import declaration without compiling FreeCAD."""

    def configure(self, static_modules, occt_tbb, provide_tbb):
        patch_text = (static.HERE / static.PROFILE["patch"]["path"]).read_text()
        section = patch_text.split("--- a/src/Mod/Import/App/CMakeLists.txt\n", 1)[1].split("--- a/", 1)[0]
        after = "\n".join(line[1:] for line in section.splitlines()
                          if line.startswith(("+", " ")) and not line.startswith("+++"))
        start = after.index("add_library(Import ")
        declaration = after[start:after.index("\nif(FREECAD_USE_PCH)", start)]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "empty.cpp").touch()
            (root / "CMakeLists.txt").write_text("""cmake_minimum_required(VERSION 3.24)
project(ImportDependencyFixture NONE)
set(FREECAD_MODULE_LIBRARY_TYPE STATIC)
set(Import_SRCS empty.cpp)
""" + declaration + """
# No compiler is needed: this fixture checks configuration and link propagation.
set_target_properties(Import PROPERTIES LINKER_LANGUAGE CXX)
set(CMAKE_CXX_ARCHIVE_CREATE "")
set(CMAKE_CXX_ARCHIVE_FINISH "")
get_target_property(dependencies Import INTERFACE_LINK_LIBRARIES)
file(WRITE "${CMAKE_BINARY_DIR}/dependencies.txt" "${dependencies}")
""")
            if provide_tbb:
                (root / "TBBConfig.cmake").write_text("""set(TBB_FOUND TRUE)
set(TBB_tbb_FOUND TRUE)
add_library(TBB::tbb INTERFACE IMPORTED)
""")
            command = ["cmake", "-S", str(root), "-B", str(root / "build"), "-G", "Ninja",
                       "-DFREECAD_STATIC_MODULES=" + ("ON" if static_modules else "OFF"),
                       "-DOpenCASCADE_WITH_TBB=" + ("ON" if occt_tbb else "OFF")]
            command += (["-DTBB_DIR=" + str(root)] if provide_tbb else ["-DCMAKE_DISABLE_FIND_PACKAGE_TBB=TRUE"])
            result = subprocess.run(command, text=True, capture_output=True)
            report = root / "build/dependencies.txt"
            return result, report.read_text() if report.exists() else ""

    def test_tbb_dependency_reaches_static_consumers(self):
        result, dependencies = self.configure(True, True, True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("TBB::tbb", dependencies.split(";"))

    def test_no_tbb_occt_does_not_require_tbb(self):
        result, dependencies = self.configure(True, False, False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("TBB::tbb", dependencies)

    def test_dynamic_profile_is_unchanged(self):
        result, dependencies = self.configure(False, True, False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("TBB::tbb", dependencies)

    def test_missing_required_tbb_fails_configuration(self):
        result, _ = self.configure(True, True, False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("TBB", result.stderr)
        self.assertIn("REQUIRED", result.stderr)


if __name__ == "__main__":
    unittest.main()
