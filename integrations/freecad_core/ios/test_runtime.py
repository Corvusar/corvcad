"""Check source preservation and actual CMake host/target Python separation."""
import difflib
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import sysconfig
from unittest.mock import patch

import prepare_ios as ios
import prepare_static as static
import test_static as fixtures


def checksum(text):
    return hashlib.sha256(text.encode()).hexdigest()


class IOSPreparation(fixtures.StaticPreparation):
    def setUp(self):
        super().setUp()
        before, after = "runtime before\n", "runtime after\n"
        (self.source / "runtime.cpp").write_bytes(before.replace("\n", "\r\n").encode())
        subprocess.run(["git", "-C", str(self.source), "add", "runtime.cpp"], check=True)
        subprocess.run(["git", "-C", str(self.source), "-c", "user.name=Fixture", "-c",
                        "user.email=fixture@example.invalid", "commit", "-qm", "Runtime fixture"], check=True)
        commit = subprocess.check_output(["git", "-C", str(self.source), "rev-parse", "HEAD"], text=True).strip()
        manager = patch.object(static.desktop.fetch, "LOCK", {"source": {"commit": commit}})
        manager.start()
        self.addCleanup(manager.stop)
        diff = "".join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                           fromfile="a/runtime.cpp", tofile="b/runtime.cpp"))
        (self.here / "runtime.patch").write_bytes(diff.encode())
        profile = {"static_profile_sha256_lf": static.digest(self.here / "static-profile.json"),
                   "patch": {"path": "runtime.patch", "sha256": checksum(diff), "files": {
                       "runtime.cpp": {"before_sha256_lf": checksum(before), "after_sha256_lf": checksum(after)}}}}
        (self.here / "ios-profile.json").write_text(json.dumps(profile))
        for name, value in (("HERE", self.here), ("PROFILE", profile)):
            manager = patch.object(ios, name, value)
            manager.start()
            self.addCleanup(manager.stop)

    def test_ios_crlf_application_and_reentry(self):
        first = ios.prepare(self.source)
        original_bytes = (self.source / "runtime.cpp").read_bytes()
        self.assertEqual(first, ios.prepare(self.source))
        self.assertEqual((self.source / "runtime.cpp").read_bytes(), original_bytes)
        self.assertEqual((self.source / "source.cpp").read_text(), self.modified)
        self.assertEqual((self.source / "runtime.cpp").read_text(), "runtime after\n")

    def test_modified_runtime_is_preserved(self):
        ios.prepare(self.source)
        path = self.source / "runtime.cpp"
        path.write_text("user edit\n")
        before = path.read_bytes()
        with self.assertRaises((ValueError, RuntimeError)):
            ios.prepare(self.source)
        self.assertEqual(path.read_bytes(), before)

    def test_modified_static_baseline_is_preserved(self):
        ios.prepare(self.source)
        path = self.source / "source.cpp"
        path.write_text("user static edit\n")
        before = path.read_bytes()
        with self.assertRaisesRegex(ValueError, "Modified iOS source"):
            ios.prepare(self.source)
        self.assertEqual(path.read_bytes(), before)

    def test_runtime_patch_corruption_prevents_static_mutation(self):
        before = (self.source / "source.cpp").read_bytes()
        (self.here / "runtime.patch").write_text("corrupt\n")
        with self.assertRaisesRegex(ValueError, "iOS patch checksum"):
            ios.prepare(self.source)
        self.assertEqual((self.source / "source.cpp").read_bytes(), before)

    def test_runtime_untracked_file_is_preserved(self):
        ios.prepare(self.source)
        path = self.source / "extra.cpp"
        path.write_text("keep this\n")
        with self.assertRaisesRegex(ValueError, "Untracked"):
            ios.prepare(self.source)
        self.assertEqual(path.read_text(), "keep this\n")

    def test_changed_static_profile_rejected(self):
        with (self.here / "static-profile.json").open("a") as stream:
            stream.write("\n")
        with self.assertRaisesRegex(ValueError, "Static base profile"):
            ios.prepare(self.source)

    def test_unreviewed_overlap_rejected_before_mutation(self):
        ios.PROFILE["patch"]["files"]["source.cpp"] = {
            "before_sha256_lf": checksum("wrong baseline"), "after_sha256_lf": checksum("wrong after")}
        before = (self.source / "source.cpp").read_bytes()
        with self.assertRaisesRegex(ValueError, "reviewed static file"):
            ios.prepare(self.source)
        self.assertEqual((self.source / "source.cpp").read_bytes(), before)


def patched_hunks(name):
    """Read actual patched lines without downloading or mutating FreeCAD."""
    section = (ios.HERE / ios.PROFILE["patch"]["path"]).read_text().split("--- a/" + name + "\n", 1)[1]
    section = section.split("diff --git ", 1)[0]
    return "\n".join(line[1:] for line in section.splitlines()
                     if line.startswith(("+", " ")) and not line.startswith("+++"))


def patched_source(name):
    after = patched_hunks(name)
    return after[:after.index("    if (${Python3_VERSION} VERSION_LESS")] + "endmacro()\n"


@unittest.skipUnless(shutil.which("clang"), "Clang preprocessor required")
class WildMagicPlatformHeaders(unittest.TestCase):
    def test_carbon_is_only_used_by_legacy_apple_path_lookup(self):
        # Exercise the real patch's include with a poisoned Carbon header;
        # no Apple SDK is needed and no iOS runtime result is implied.
        source = patched_hunks("src/Mod/Mesh/App/WildMagic4/Wm4System.cpp")
        source = source[source.index("// support for locating the application directory"):]
        source += "\n#ifndef CORVCAD_CARBON_SELECTED\n#define CORVCAD_CARBON_SELECTED 0\n#endif\nCORVCAD_CARBON_SELECTED\n"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "Carbon").mkdir()
            (root / "Carbon/Carbon.h").write_text(
                "#ifdef CORVCAD_FORBID_CARBON\n#error Desktop Carbon header selected\n#endif\n"
                "#define CORVCAD_CARBON_SELECTED 1\n")
            for name, flags, expected in (
                ("ios-arm64", ["-D__APPLE__", "-D__aarch64__"], "0"),
                ("macos-x86_64", ["-D__APPLE__", "-D__x86_64__"], "0"),
                ("legacy-apple", ["-D__APPLE__"], "1"),
                ("other", [], "0"),
            ):
                with self.subTest(platform=name):
                    command = [shutil.which("clang"), "-E", "-P", "-x", "c", "-nostdinc",
                               "-U__APPLE__", "-U__aarch64__", "-U__x86_64__", "-I", str(root), *flags]
                    if expected == "0":
                        command.append("-DCORVCAD_FORBID_CARBON")
                    result = subprocess.run([*command, "-"], input=source, text=True, capture_output=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stdout.strip(), expected)


@unittest.skipUnless(shutil.which("cmake"), "CMake required")
class CrossPythonDiscovery(unittest.TestCase):
    def configure(self, host_version="3.13.7", target_version="3.13.7", static_modules=True, explicit=True):
        # Configure the patch's real macro against fake target artifacts. The
        # host command reports a fixture version; no target executable exists.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            host = root / ("host.cmd" if os.name == "nt" else "host")
            host.write_text(("@echo off\n@echo " if os.name == "nt" else "#!/bin/sh\necho ") + host_version + "\n")
            host.chmod(0o755)
            include = root / "target/include"
            include.mkdir(parents=True)
            (include / "Python.h").touch()
            major, minor, micro = target_version.split(".")
            (include / "patchlevel.h").write_text(f'''#define PY_MAJOR_VERSION {major}
#define PY_MINOR_VERSION {minor}
#define PY_MICRO_VERSION {micro}
#define PY_VERSION "{target_version}"
''')
            (include / "pyconfig.h").write_text("#define SIZEOF_VOID_P 8\n")
            library = root / "target/libpython3.13.a"
            library.touch()
            (root / "SetupPython.cmake").write_text(patched_source("cMake/FreeCAD_Helpers/SetupPython.cmake"))
            (root / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.24)
project(CrossPythonFixture NONE)
if(POLICY CMP0190)
  cmake_policy(SET CMP0190 NEW)
endif()
set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_CROSSCOMPILING TRUE)
set(CMAKE_SIZEOF_VOID_P 8)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
include(SetupPython.cmake)
SetupPython()
file(WRITE "${CMAKE_BINARY_DIR}/artifacts.txt" "${Python3_EXECUTABLE}\\n${Python3_LIBRARIES}\\n${Python3_INCLUDE_DIRS}")
''')
            command = ["cmake", "-S", str(root), "-B", str(root / "build"),
                       "-DFREECAD_STATIC_MODULES=" + ("ON" if static_modules else "OFF"),
                       "-DPython3_EXECUTABLE=" + host.as_posix()]
            if explicit:
                command += ["-DPython3_INCLUDE_DIR=" + include.as_posix(), "-DPython3_LIBRARY=" + library.as_posix()]
            if shutil.which("ninja"):
                command += ["-G", "Ninja"]
            result = subprocess.run(command, text=True, capture_output=True)
            report = root / "build/artifacts.txt"
            return result, report.read_text() if report.exists() else "", host.as_posix(), library.as_posix()

    def test_host_command_and_target_development_work_without_emulator(self):
        result, report, host, target = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(host, report)
        self.assertIn(target, report)

    def test_host_version_mismatch_fails(self):
        result, *_ = self.configure(host_version="3.12.0")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("host CPython 3.13.7", result.stderr)

    def test_target_version_mismatch_fails(self):
        result, *_ = self.configure(target_version="3.13.6")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("3.13.7", result.stderr)

    def test_missing_target_inputs_fail_before_discovery(self):
        result, *_ = self.configure(explicit=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Explicit iOS Python artifact", result.stderr)

    def test_ios_dynamic_profile_rejected(self):
        result, *_ = self.configure(static_modules=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires static modules", result.stderr)


@unittest.skipUnless(shutil.which("clang++"), "Host clang++ required for native embedding check")
class IsolatedInterpreter(unittest.TestCase):
    def test_actual_initialization_controls_and_process_audit_hook(self):
        """Compile the patch's actual helper and embed the available host Python.

        This checks CPython API/configuration behavior, not an iOS device result.
        """
        include = Path(sysconfig.get_path("include"))
        config = Path(sysconfig.get_config_var("BINDIR") or "") / ("python" + sysconfig.get_python_version() + "-config")
        if not config.is_file() or not (include / "Python.h").exists():
            self.skipTest("Host Python development headers/config are unavailable")
        link = subprocess.run([str(config), "--embed", "--ldflags"], text=True, capture_output=True)
        if link.returncode:
            self.skipTest("Host Python does not provide embedding link flags")
        import shlex
        code = patched_hunks("src/Base/Interpreter.cpp")
        code = code.split("#ifdef FC_OS_IOS\n", 1)[1].split("#else\nvoid initInterpreter", 1)[0]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "runtime.cpp"
            program = root / "runtime"
            source.write_text('''#include <Python.h>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
namespace Base { using RuntimeError = std::runtime_error; }
''' + code + '''
int main(int argc, char** argv) {
    initInterpreter(argc, argv);
    int rc = PyRun_SimpleString(
        "import sys\\n"
        "assert sys.flags.isolated == 1\\n"
        "assert sys.flags.ignore_environment == 1\\n"
        "assert sys.flags.no_user_site == 1\\n"
        "assert sys.flags.no_site == 1\\n"
        "assert sys.flags.dont_write_bytecode == 1\\n"
        "assert sys.flags.safe_path\\n"
        "assert 'site' not in sys.modules\\n"
        "for event in ['subprocess.Popen', 'os.system', 'os.fork', 'os.posix_spawn', 'os.exec']:\\n"
        "    try: sys.audit(event)\\n"
        "    except RuntimeError: pass\\n"
        "    else: raise AssertionError(event)\\n"
        "sys.audit('corvcad.geometry')\\n");
    int final = Py_FinalizeEx();
    return rc == 0 && final == 0 ? 0 : 1;
}
''')
            result = subprocess.run(["clang++", "-std=c++17", "-I" + str(include), str(source),
                                     *shlex.split(link.stdout), "-o", str(program)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            env = {**os.environ, "CORVCAD_PYTHON_STDLIB": sysconfig.get_path("stdlib"),
                   "PYTHONPATH": str(root / "untrusted"), "PYTHONHOME": str(root / "untrusted")}
            result = subprocess.run([str(program)], env=env, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


@unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja"), "CMake and Ninja required")
class HeadlessQtEntryPoint(unittest.TestCase):
    def configure(self, system="iOS", static_modules=True):
        source = patched_hunks("src/Main/CMakeLists.txt")
        start = source.index('if(FREECAD_STATIC_MODULES AND CMAKE_SYSTEM_NAME STREQUAL "iOS")')
        block = source[start:source.index("\nendif()", start) + len("\nendif()")]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            # Model Qt 6.11.1's exact consumer-target predicate from
            # qtbase/src/corelib/CMakeLists.txt. Imported executables allow
            # generation without a compiler or Apple SDK; nothing is linked.
            (root / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.24)
project(QtEntryPointFixture NONE)
set(CMAKE_SYSTEM_NAME "${fixture_system}")
add_executable(FreeCADMainCmd IMPORTED)
add_executable(OtherApp IMPORTED)
add_library(QtCore INTERFACE)
set_property(TARGET QtCore PROPERTY INTERFACE_LINK_LIBRARIES
    "$<$<AND:$<NOT:$<BOOL:$<TARGET_PROPERTY:qt_no_entrypoint>>>,$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>>:QtEntryPoint>")
''' + block + '''
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/headless.txt"
    CONTENT "$<TARGET_GENEX_EVAL:FreeCADMainCmd,$<TARGET_PROPERTY:QtCore,INTERFACE_LINK_LIBRARIES>>")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/other.txt"
    CONTENT "$<TARGET_GENEX_EVAL:OtherApp,$<TARGET_PROPERTY:QtCore,INTERFACE_LINK_LIBRARIES>>")
''')
            build = root / "build"
            result = subprocess.run(["cmake", "-S", str(root), "-B", str(build), "-G", "Ninja",
                                     "-Dfixture_system=" + system,
                                     "-DFREECAD_STATIC_MODULES=" + ("ON" if static_modules else "OFF")],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return (build / "headless.txt").read_text(), (build / "other.txt").read_text()

    def test_ios_headless_target_uses_own_main_without_affecting_other_apps(self):
        self.assertEqual(self.configure(), ("", "QtEntryPoint"))

    def test_desktop_target_is_unchanged(self):
        self.assertEqual(self.configure(system="Darwin"), ("QtEntryPoint", "QtEntryPoint"))

    def test_nonstatic_target_is_unchanged(self):
        self.assertEqual(self.configure(static_modules=False), ("QtEntryPoint", "QtEntryPoint"))


@unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja"), "CMake and Ninja required")
class StaticOCCTClosure(unittest.TestCase):
    def configure(self, measured="TKDE;TKernel", exported="TKernel;TKDE", shared=False, missing=False):
        # Exercise the actual source patch's closure validation and link call.
        source = patched_hunks("src/Main/CMakeLists.txt")
        start = source.index("        # OCCT's exported static targets")
        end = source.index("        target_link_libraries(FreeCADMainCmd ${OpenCASCADE_LIBRARIES})", start)
        block = source[start:end] + "        target_link_libraries(FreeCADMainCmd ${OpenCASCADE_LIBRARIES})\n"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "empty.cpp").touch()
            (root / "empty.a").touch()
            location = ' PROPERTIES IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/empty.a")\n'
            targets = "add_library(TKernel STATIC IMPORTED)\nset_target_properties(TKernel" + location
            if not missing:
                targets += "add_library(TKDE " + ("SHARED" if shared else "STATIC") + " IMPORTED)\n"
                targets += "set_target_properties(TKDE" + location
            (root / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.24)
project(OCCTClosureFixture NONE)
add_library(FreeCADMainCmd STATIC empty.cpp)
set_target_properties(FreeCADMainCmd PROPERTIES LINKER_LANGUAGE CXX)
set(CMAKE_CXX_ARCHIVE_CREATE "")
set(CMAKE_CXX_ARCHIVE_FINISH "")
''' + targets + block + '''
get_target_property(toolkits FreeCADMainCmd INTERFACE_LINK_LIBRARIES)
file(WRITE "${CMAKE_BINARY_DIR}/linked.txt" "${toolkits}")
''')
            result = subprocess.run(["cmake", "-S", str(root), "-B", str(root / "build"), "-G", "Ninja",
                                     "-DCORVCAD_IOS_OCCT_LIBRARIES=" + measured,
                                     "-DOpenCASCADE_LIBRARIES=" + exported], text=True, capture_output=True)
            linked = root / "build/linked.txt"
            return result, linked.read_text() if linked.exists() else ""

    def test_measured_static_closure_linked_without_order_dependency(self):
        result, linked = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(set(linked.split(";")), {"TKernel", "TKDE"})

    def test_extra_exported_toolkit_rejected(self):
        result, _ = self.configure(exported="TKernel;TKDE;TKOpenGl")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("exports differ", result.stderr)

    def test_shared_toolkit_rejected(self):
        result, _ = self.configure(shared=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Non-static", result.stderr)

    def test_missing_toolkit_target_rejected(self):
        result, _ = self.configure(missing=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Missing static", result.stderr)

    def test_unmeasured_closure_rejected(self):
        result, _ = self.configure(measured="")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Explicit measured", result.stderr)


if __name__ == "__main__":
    unittest.main()
