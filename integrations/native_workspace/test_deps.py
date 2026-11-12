"""Shared dependency selection and SDK cache ownership regressions. MIT."""
import copy
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import deps


class Selection(unittest.TestCase):
    def test_real_manifest_is_the_pinned_root_union(self):
        references = deps.verify_inputs()
        self.assertIn("freecad_manifest", references)
        self.assertIn("kicad", references)
        manifest = json.loads((deps.HERE / "vcpkg.json").read_text())
        self.assertEqual(manifest["builtin-baseline"], deps.LOCK["vcpkg"]["commit"])
        self.assertEqual(manifest["overrides"], [{"name": "eigen3", "version": "3.4.0", "port-version": 5}])
        by_name = {d if isinstance(d, str) else d["name"]: d for d in manifest["dependencies"]}
        self.assertEqual(by_name["harfbuzz"]["features"], ["freetype"])
        self.assertTrue(by_name["qttools"]["host"])
        self.assertFalse(by_name["qtbase"]["default-features"])
        self.assertEqual(set(by_name["qtbase"]["features"]), {"concurrent", "network", "thread", "appstore-compliant"})
        self.assertEqual(by_name["expat"], "expat")
        self.assertIn("expat", deps.LOCK["additional_dependencies"])

    def test_overlapping_root_features_are_not_silently_discarded(self):
        freecad = {"dependencies": [{"name": "qtbase", "default-features": False, "features": ["thread"]}]}
        with self.assertRaisesRegex(ValueError, "unreviewed features"):
            deps.expected_manifest(freecad, {"vcpkg": {"ports": ["qtbase[gui]"]}})
        result = deps.expected_manifest(freecad, {"vcpkg": {"ports": ["qtbase[thread]"]}})
        self.assertEqual(len(result["dependencies"]), 1)

    def test_source_input_drift_is_rejected_before_any_cache_change(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for reference in deps.LOCK["references"].values():
                path = root / reference["path"]
                path.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(deps.REPOSITORY / reference["path"], path)
            target = root / deps.LOCK["references"]["freecad_manifest"]["path"]
            target.write_text(target.read_text() + " ")
            with self.assertRaisesRegex(ValueError, "Reviewed dependency input changed"):
                deps.verify_inputs(root)
            self.assertFalse((root / "installed").exists())

    def test_per_sdk_install_roots_and_explicit_host_target_selection(self):
        with tempfile.TemporaryDirectory() as temporary, patch.object(deps.target_config, "host_triplet", return_value="x64-osx"):
            root = Path(temporary) / "not-created"
            simulator = deps.plan(root, "iphonesimulator", "x86_64", jobs=2)
            device = deps.plan(root, "iphoneos", jobs=2)
            self.assertEqual(Path(simulator["directory"]), (root / "iphonesimulator").resolve())
            self.assertEqual(Path(device["directory"]), (root / "iphoneos").resolve())
            self.assertNotEqual(simulator["installed"], device["installed"])
            self.assertEqual(simulator["target"]["triplet"], "x64-ios-simulator")
            self.assertEqual(simulator["target"]["host_triplet"], "x64-osx")
            self.assertEqual(device["target"]["architecture"], "arm64")
            self.assertIn("--host-triplet", simulator["command"])
            self.assertEqual(simulator["host_triplet_sha256_lf"], deps.LOCK["host_triplets"]["x64-osx"]["sha256_lf"])
            self.assertFalse(root.exists())
            with self.assertRaisesRegex(ValueError, "simulator-only"):
                deps.plan(root, "iphoneos", "x86_64")

    def test_host_recipes_are_pinned_without_changing_target_policy(self):
        for host in ("x64-osx", "arm64-osx"):
            recipe = deps.HERE / "triplets" / (host + ".cmake")
            self.assertEqual(deps.sha(recipe, True), deps.LOCK["host_triplets"][host]["sha256_lf"])
            with patch.object(deps.target_config, "host_triplet", return_value=host):
                selected = deps.plan(Path("not-created"), "iphoneos")
                self.assertEqual(selected["target"]["host_triplet"], host)
                self.assertEqual(selected["host_triplet_sha256_lf"], deps.sha(recipe, True))
        for target in deps.target_config.TRIPLETS:
            self.assertNotIn("NO_SYSTEM_FROM_IMPORTED", (deps.HERE / "triplets" / (target + ".cmake")).read_text())
        changed = copy.deepcopy(deps.LOCK)
        changed["host_triplets"]["x64-osx"]["sha256_lf"] = "0" * 64
        with patch.object(deps, "LOCK", changed), self.assertRaisesRegex(ValueError, "Reviewed host triplet changed"):
            deps.verify_inputs()


class CacheOwnership(unittest.TestCase):
    def test_cleanup_changes_command_and_receipt_without_invalidating_recipe_claim(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            ordinary = deps.plan(root, "iphonesimulator", "x86_64")
            selected = deps.plan(root, "iphonesimulator", "x86_64", clean_buildtrees_after_build=True)
            self.assertNotIn("--clean-buildtrees-after-build", ordinary["command"])
            self.assertEqual(selected["command"], ordinary["command"] + ["--clean-buildtrees-after-build"])
            self.assertEqual(selected["retention"], {"clean_buildtrees_after_build": True})
            deps.claim(ordinary)
            marker = Path(selected["directory"]) / "native-dependency-target.json"
            before = marker.read_bytes()
            executable = root / "vcpkg-fixture"
            executable.write_bytes(b"test tool")
            with patch.object(deps.platform, "system", return_value="Darwin"), \
                    patch.object(deps, "checkout", return_value=executable), \
                    patch.object(deps, "run") as invocation, \
                    patch.object(deps.dependency_inventory, "capture", return_value={}), \
                    patch.object(deps.dependency_inventory, "inspect_target", return_value={}), \
                    patch.object(deps.subprocess, "check_output", side_effect=["Xcode fixture", "26.2", "clang fixture"]):
                receipt = deps.build(selected)
            self.assertEqual(invocation.call_args.args[0], selected["command"])
            self.assertEqual(receipt["retention"], selected["retention"])
            self.assertEqual(json.loads(Path(selected["receipt_path"]).read_text())["retention"], selected["retention"])
            self.assertEqual(marker.read_bytes(), before)

    def test_other_sdk_prefix_is_not_pruned_or_adopted(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            device, simulator = deps.plan(root, "iphoneos"), deps.plan(root, "iphonesimulator", "x86_64")
            deps.claim(device)
            preserved = Path(device["installed"]) / "arm64-ios/lib/libz.a"
            preserved.parent.mkdir(parents=True)
            preserved.write_bytes(b"accepted device archive")
            deps.claim(simulator)
            self.assertEqual(preserved.read_bytes(), b"accepted device archive")
            self.assertNotEqual(device["installed"], simulator["installed"])

    def test_reusing_same_sdk_with_another_architecture_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            intel = deps.plan(root, "iphonesimulator", "x86_64")
            deps.claim(intel)
            marker = Path(intel["directory"]) / "native-dependency-target.json"
            before = marker.read_bytes()
            with self.assertRaisesRegex(ValueError, "different reviewed inputs or architecture"):
                deps.claim(deps.plan(root, "iphonesimulator", "arm64"))
            self.assertEqual(marker.read_bytes(), before)

    def test_recipe_change_requires_a_new_owned_root(self):
        with tempfile.TemporaryDirectory() as temporary:
            selected = deps.plan(Path(temporary), "iphoneos")
            deps.claim(selected)
            changed = copy.deepcopy(selected)
            changed["manifest_sha256_lf"] = "0" * 64
            with self.assertRaisesRegex(ValueError, "different reviewed inputs"):
                deps.claim(changed)

    def test_host_recipe_change_preserves_failed_root_and_logs(self):
        with tempfile.TemporaryDirectory() as temporary:
            selected = deps.plan(Path(temporary), "iphonesimulator", "x86_64")
            deps.claim(selected)
            log = Path(selected["directory"]) / "failed-protobuf.log"
            log.write_bytes(b"original compiler diagnostic")
            marker = Path(selected["directory"]) / "native-dependency-target.json"
            before = marker.read_bytes()
            changed = copy.deepcopy(selected)
            changed["host_triplet_sha256_lf"] = "0" * 64
            with self.assertRaisesRegex(ValueError, "use a new root"):
                deps.claim(changed)
            self.assertEqual(marker.read_bytes(), before)
            self.assertEqual(log.read_bytes(), b"original compiler diagnostic")

    def test_unknown_existing_prefix_is_preserved(self):
        with tempfile.TemporaryDirectory() as temporary:
            selected = deps.plan(Path(temporary), "iphoneos")
            directory = Path(selected["directory"])
            directory.mkdir()
            existing = directory / "user-work.txt"
            existing.write_text("do not replace")
            with self.assertRaisesRegex(ValueError, "Unowned dependency directory"):
                deps.claim(selected)
            self.assertEqual(existing.read_text(), "do not replace")

    def test_git_reference_dissociates_without_reusing_old_install_prefix(self):
        with tempfile.TemporaryDirectory() as temporary:
            selected = deps.plan(Path(temporary), "iphoneos", reference=Path(temporary) / "old-registry")
            directory = Path(selected["vcpkg"])
            commands = []

            def fake_run(command, cwd=None, env=None):
                commands.append([str(x) for x in command])
                if command[:2] == ["git", "clone"]:
                    directory.mkdir(parents=True)
                    (directory / "vcpkg").write_bytes(b"tool fixture")
            with patch.object(deps, "run", fake_run), patch.object(deps.subprocess, "check_output", side_effect=["", selected["registry_commit"], selected["registry_commit"]]):
                deps.checkout(selected)
            self.assertIn("--reference-if-able", commands[0])
            self.assertIn("--dissociate", commands[0])
            self.assertNotIn(str(Path(temporary) / "old-registry/installed"), str(commands))


class HostIncludeOrdering(unittest.TestCase):
    def test_real_cmake_host_overlay_promotes_imported_include_from_system(self):
        cmake, ninja = shutil.which("cmake"), shutil.which("ninja")
        if not cmake or not ninja:
            self.skipTest("CMake and Ninja are required for compiler-free command generation")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            include = root / "pinned-host-headers"
            include.mkdir()
            (root / "consumer.cpp").write_text("int answer;\n")
            # Force an identified compiler so configuration only generates
            # commands. No compiler process or target build is needed.
            (root / "toolchain.cmake").write_text(
                'set(CMAKE_SYSTEM_NAME Linux)\n'
                'set(CMAKE_CXX_COMPILER "' + Path(sys.executable).as_posix() + '")\n'
                'set(CMAKE_CXX_COMPILER_ID Clang)\n'
                'set(CMAKE_CXX_COMPILER_ID_RUN TRUE)\n'
                'set(CMAKE_CXX_COMPILER_FORCED TRUE)\n')
            (root / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.20)
if(HOST_RECIPE)
  include("${HOST_RECIPE}")
  foreach(option IN LISTS VCPKG_CMAKE_CONFIGURE_OPTIONS)
    if(option MATCHES "^-D([^=]+)=(.*)$")
      set("${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}")
    else()
      message(FATAL_ERROR "Unexpected host configure option: ${option}")
    endif()
  endforeach()
endif()
project(ImportedHostHeaders LANGUAGES CXX)
add_library(dependency INTERFACE IMPORTED)
set_target_properties(dependency PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/pinned-host-headers")
add_library(consumer STATIC consumer.cpp)
target_link_libraries(consumer PRIVATE dependency)
''')
            for recipe in (None, "x64-osx", "arm64-osx"):
                build = root / (recipe or "baseline")
                command = [cmake, "-S", str(root), "-B", str(build), "-G", "Ninja",
                           "-DCMAKE_MAKE_PROGRAM=" + ninja, "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                           "-DCMAKE_TOOLCHAIN_FILE=" + str(root / "toolchain.cmake")]
                if recipe:
                    command += ["-DHOST_RECIPE=" + str(deps.HERE / "triplets" / (recipe + ".cmake"))]
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                generated = json.loads((build / "compile_commands.json").read_text())[0]["command"].replace("\\", "/")
                if recipe:
                    self.assertIn("-I" + include.as_posix(), generated)
                    self.assertNotIn("-isystem", generated)
                else:
                    self.assertIn("-isystem " + include.as_posix(), generated)


if __name__ == "__main__":
    unittest.main()
