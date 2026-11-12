"""F4 package boundary regressions; no native geometry is implied. MIT."""
import json
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import package
import build as session_build

_runner_spec = importlib.util.spec_from_file_location("corvcad_session_validation", package.HERE / "run.py")
session_run = importlib.util.module_from_spec(_runner_spec)
_runner_spec.loader.exec_module(session_run)


@unittest.skipUnless(shutil.which("cmake"), "CMake required")
class DeferredConfiguration(unittest.TestCase):
    def test_runtime_build_includes_independent_upstream_copy_targets(self):
        # The production invocation must run ALL resource targets, even when
        # neither executable declares those files as a dependency.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "Init.py").write_text("# reviewed module initializer\n")
            (root / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.24)
project(ResourceFixture NONE)
add_custom_target(FreeCADMainCmd)
add_custom_target(corvcad_mechanical_probe)
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/Mod/Material/Init.py"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/Mod/Material"
  COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_SOURCE_DIR}/Init.py" "${CMAKE_BINARY_DIR}/Mod/Material/Init.py"
  DEPENDS "${CMAKE_SOURCE_DIR}/Init.py")
add_custom_target(MaterialResources ALL DEPENDS "${CMAKE_BINARY_DIR}/Mod/Material/Init.py")
''')
            command = ["cmake", "-S", str(root), "-B", str(root / "build")]
            if shutil.which("ninja"):
                command += ["-G", "Ninja"]
            subprocess.run(command, check=True, capture_output=True)
            session_build.build_runtime(root / "build", 1)
            self.assertEqual((root / "build/Mod/Material/Init.py").read_bytes(), (root / "Init.py").read_bytes())

    def test_real_hook_adds_targets_after_upstream_targets_without_subdirectories(self):
        # Exercise the production hook with a compiler-free CMake project. The
        # fixture payload checks late target visibility and list-file paths;
        # this establishes configuration semantics, not native compilation.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            session = root / "session"
            session.mkdir()
            shutil.copyfile(package.HERE / "hook.cmake", session / "hook.cmake")
            (session / "CMakeLists.txt").write_text('''
if(NOT TARGET FreeCADMainCmd)
  message(FATAL_ERROR "Hook ran before upstream targets existed")
endif()
add_library(session_probe INTERFACE)
target_sources(session_probe INTERFACE "${CMAKE_CURRENT_LIST_DIR}/mechanical_session.cpp")
get_target_property(sources session_probe INTERFACE_SOURCES)
file(WRITE "${CMAKE_BINARY_DIR}/hook-source.txt" "${sources}")
''')
            (session / "mechanical_session.cpp").touch()
            (root / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.24)
project(FreeCAD NONE)
add_library(FreeCADMainCmd INTERFACE)
''')
            command = ["cmake", "-S", str(root), "-B", str(root / "build"),
                       "-DCMAKE_PROJECT_FreeCAD_INCLUDE=" + (session / "hook.cmake").as_posix()]
            if shutil.which("ninja"):
                command += ["-G", "Ninja"]
            for _ in range(2):
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(Path((root / "build/hook-source.txt").read_text()).resolve(),
                                 (session / "mechanical_session.cpp").resolve())


class LinkBoundary(unittest.TestCase):
    def test_each_edition_gets_its_requested_project_without_overwriting(self):
        with tempfile.TemporaryDirectory() as directory:
            container = Path(directory)
            for name in ("FreeCAD-F4", "FreeCAD-F4-Landscape", "Demo"):
                project = session_run.seed_project(container, name)
                for extension in ("kicad_pro", "kicad_sch", "kicad_pcb"):
                    original = session_run.REPO / "engine/tests/fixtures/roundtrip" / ("RoundTrip." + extension)
                    self.assertEqual((project / (name + "." + extension)).read_bytes(), original.read_bytes())
                with self.assertRaises(FileExistsError):
                    session_run.seed_project(container, name)

    def test_package_architecture_is_checked_before_resources_or_archives(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "package-manifest.json").write_text(json.dumps({
                "schema": 1, "sdk": "iphonesimulator", "architecture": "arm64"}))
            with self.assertRaisesRegex(ValueError, "another architecture"):
                package.verify_package(root, "iphonesimulator", "x86_64")
            (root / "package-manifest.json").write_text(json.dumps({
                "schema": 1, "sdk": "iphonesimulator", "architecture": "x86_64"}))
            with self.assertRaisesRegex(ValueError, "another architecture"):
                package.verify_package(root, "iphonesimulator")
            with self.assertRaisesRegex(ValueError, "another SDK"):
                package.verify_package(root, "iphoneos")

    def test_resources_must_remain_unchanged_after_session_execution(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("libcorvcad_freecad.a", "libcorvcad_freecad_deps.a"):
                (root / name).write_bytes(b"archive fixture")
            resources = root / "FreeCADRuntime"
            resources.mkdir()
            helper = resources / "model.py"
            helper.write_bytes(b"# reviewed fixture\n")
            report = {"schema": 1, "sdk": "iphoneos", "core_sha256": package.digest(root / "libcorvcad_freecad.a"),
                      "dependencies_sha256": package.digest(root / "libcorvcad_freecad_deps.a"),
                      "resources": {"model.py": {"bytes": helper.stat().st_size, "sha256": package.digest(helper)}}}
            (root / "package-manifest.json").write_text(json.dumps(report))
            package.verify_package(root, "iphoneos")
            helper.write_bytes(b"unexpected edit\n")
            with self.assertRaisesRegex(ValueError, "resource changed"):
                package.verify_package(root, "iphoneos")
            helper.write_bytes(b"# reviewed fixture\n")
            (resources / "model.pyc").touch()
            with self.assertRaisesRegex(ValueError, "resource set changed"):
                package.verify_package(root, "iphoneos")

    def fixture(self, root, bad=False):
        reply = root / ".cmake/api/v1/reply"
        reply.mkdir(parents=True)
        core = sorted(package.CORE_ARCHIVES)
        fragments = []
        for name in core:
            p = root / "native libraries" / name
            p.parent.mkdir(exist_ok=True)
            p.touch()
            fragments += [{"fragment": '-Xlinker -force_load -Xlinker "' + str(p) + '"', "role": "flags"}]
        for name in ("libcorvcad_freecad_session.a", "libpython3.13.a", "libz.a", *core):
            p = root / "native libraries" / name
            p.touch()
            fragments += [{"fragment": '"' + str(p) + '"', "role": "libraries"}]
        fragments += [{"fragment": "-framework UIKit -lsqlite3 -lz", "role": "libraries"}]
        if bad:
            fragments += [{"fragment": "/host/libpython.dylib", "role": "libraries"}]
        for name, value in (("index-test.json", {"reply": {"codemodel-v2": {"jsonFile": "model.json"}}}),
                            ("model.json", {"configurations": [{"targets": [{"name": "corvcad_mechanical_probe", "jsonFile": "probe.json"}]}]}),
                            ("probe.json", {"link": {"commandFragments": fragments}})):
            (reply / name).write_text(json.dumps(value))

    def test_force_load_scope_and_shared_archive_deduplication(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root)
            core, deps, system = package.link_inputs(root)
            self.assertEqual(len(core), 10)
            self.assertEqual({p.name for p in deps}, {"libpython3.13.a", "libz.a"})
            self.assertEqual(system, ["-framework", "UIKit", "-lsqlite3"])

    def test_host_shared_dependency_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root, bad=True)
            with self.assertRaisesRegex(ValueError, "Non-static"):
                package.link_inputs(root)

    def test_changed_module_force_load_scope_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root)
            p = root / ".cmake/api/v1/reply/probe.json"
            v = json.loads(p.read_text())
            v["link"]["commandFragments"].pop(0)
            p.write_text(json.dumps(v))
            with self.assertRaisesRegex(ValueError, "nine reviewed"):
                package.link_inputs(root)


if __name__ == "__main__":
    unittest.main()
