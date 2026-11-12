"""Regression checks for source integrity and the dependency sharing boundary."""
import hashlib
import io
import json
import os
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch

import build_python
import dependency_inventory
import simulator
import binary_report


class Sources(unittest.TestCase):
    def archive(self, root, name="Python-test/configure", link=False):
        path = root / "python.tar.xz"
        with tarfile.open(path, "w:xz") as archive:
            member = tarfile.TarInfo(name)
            if link:
                member.type = tarfile.SYMTYPE
                member.linkname = "../../outside"
                archive.addfile(member)
            else:
                member.size = 4
                archive.addfile(member, io.BytesIO(b"test"))
        return path

    def test_archive_traversal_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = self.archive(root, "../outside")
            with self.assertRaisesRegex(ValueError, "escapes"):
                build_python.extract(archive, root / "source")

    def test_archive_link_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaisesRegex(ValueError, "member type"):
                build_python.extract(self.archive(root, link=True), root / "source")

    def test_cached_source_edits_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = self.archive(root)
            pin = {"version": "test", "url": "https://example.invalid/python.tar.xz",
                   "sha256": hashlib.sha256(archive.read_bytes()).hexdigest()}
            with patch.dict(build_python.LOCK, {"python": pin}):
                src = build_python.source(root)
                (src / "configure").write_text("modified")
                with self.assertRaisesRegex(ValueError, "Modified CPython source"):
                    build_python.source(root)

    def test_cached_archive_corruption_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = self.archive(root)
            pin = {"version": "test", "url": "https://example.invalid/python.tar.xz", "sha256": "0" * 64}
            with patch.dict(build_python.LOCK, {"python": pin}):
                with self.assertRaisesRegex(ValueError, "checksum"):
                    build_python.source(root)

    def test_common_registry_matches_kicad(self):
        here = Path(__file__).resolve().parent
        mechanical = json.loads((here / "inputs.json").read_text())
        electronics = json.loads((here.parent.parent / "kicad_core/upstream.json").read_text())
        manifest = json.loads((here / "vcpkg.json").read_text())
        self.assertEqual(mechanical["vcpkg"]["commit"], electronics["vcpkg"]["commit"])
        self.assertEqual(manifest["builtin-baseline"], mechanical["vcpkg"]["commit"])


class HostGenerator(unittest.TestCase):
    def test_host_crypto_and_inherited_build_flags_are_disabled(self):
        with patch.dict(os.environ, {"CPPFLAGS": "-I/foreign/include", "SDKROOT": "/ios/sdk",
                                     "PKG_CONFIG_PATH": "/foreign/pkgconfig"}):
            env = build_python.host_environment()
        self.assertNotIn("CPPFLAGS", env)
        self.assertNotIn("SDKROOT", env)
        self.assertNotIn("PKG_CONFIG_PATH", env)
        self.assertEqual(env["PKG_CONFIG"], "/usr/bin/false")
        self.assertEqual(env["ac_cv_working_openssl_ssl"], "no")
        self.assertEqual(env["ac_cv_working_openssl_hashlib"], "no")

    def test_partial_host_build_rebuilds_missing_generator_extensions(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            host = root / "host"
            host.mkdir()
            (host / "python.exe").touch()
            with patch.object(build_python.subprocess, "run") as probe, \
                    patch.object(build_python, "run") as commands:
                probe.return_value.returncode = 1
                build_python.build_host(root / "source", host, 2)
            self.assertEqual(commands.call_count, 2)
            self.assertEqual(commands.call_args_list[1].args[0], ["make", "-j2"])


class TargetSysconfig(unittest.TestCase):
    def test_copies_target_generated_module(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target, stdlib = root / "target", root / "stdlib"
            generated = target / "build/lib.ios-arm64-3.13"
            generated.mkdir(parents=True)
            stdlib.mkdir()
            (target / "pybuilddir.txt").write_text("build/lib.ios-arm64-3.13")
            (generated / "_sysconfigdata__ios_arm64.py").write_text("build_time_vars = {'MACHDEP': 'ios'}")
            build_python.stage_sysconfig(target, stdlib)
            self.assertEqual((stdlib / "_sysconfigdata__ios_arm64.py").read_bytes(),
                             (generated / "_sysconfigdata__ios_arm64.py").read_bytes())

    def test_host_path_escape_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "pybuilddir.txt").write_text("../host")
            with self.assertRaisesRegex(ValueError, "escapes target"):
                build_python.stage_sysconfig(root, root)


class TargetConfiguration(unittest.TestCase):
    def test_undefined_optional_apis_allow_existing_posix_fallbacks(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config = b"/* #undef HAVE_DUP3 */\n/* #undef HAVE_PIPE2 */\n#define HAVE_DUP2 1\n#define HAVE_PIPE 1\n"
            (root / "pyconfig.h").write_bytes(config)
            self.assertEqual(build_python.verify_ios_config(root), hashlib.sha256(config).hexdigest())

    def test_cached_symbol_only_detection_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for macro in ("HAVE_DUP3", "HAVE_PIPE2"):
                for value in ("0", "1"):
                    with self.subTest(macro=macro, value=value):
                        # posixmodule uses #ifdef, so even a definition of 0
                        # would select the unavailable API and must fail.
                        (root / "pyconfig.h").write_text(f"# define {macro} {value}\n")
                        with self.assertRaisesRegex(ValueError, "undeclared"):
                            build_python.verify_ios_config(root)


class SystemExpat(unittest.TestCase):
    def dependencies(self, root, expat=True):
        (root / "include").mkdir(parents=True)
        (root / "lib").mkdir()
        for name in ("ffi", "z", "bz2", *(("expat",) if expat else ())):
            (root / "lib" / f"lib{name}.a").write_bytes(name.encode())
        for name in ("ffi.h", "zlib.h", "bzlib.h", *(("expat.h", "expat_external.h") if expat else ())):
            (root / "include" / name).write_bytes(name.encode())
        return root

    def test_default_keeps_bundled_expat_and_original_dependency_set(self):
        with tempfile.TemporaryDirectory() as directory:
            env, archives, headers = build_python.target_dependencies(self.dependencies(Path(directory), expat=False))
        self.assertEqual(set(archives), {"libffi.a", "libz.a", "libbz2.a"})
        self.assertFalse(any(name.startswith("LIBEXPAT") for name in env))
        self.assertNotIn("expat.h", headers)

    def test_explicit_expat_uses_upstream_configure_variable_and_records_hash(self):
        with tempfile.TemporaryDirectory() as directory:
            deps = self.dependencies(Path(directory))
            env, archives, headers = build_python.target_dependencies(deps, True)
            self.assertEqual(env["LIBEXPAT_CFLAGS"], f"-I{deps / 'include'}")
            self.assertEqual(env["LIBEXPAT_LDFLAGS"], str(deps / "lib/libexpat.a"))
            self.assertNotIn("LIBEXPAT_LIBS", env)
            self.assertEqual(archives["libexpat.a"], hashlib.sha256(b"expat").hexdigest())
            self.assertEqual(headers["expat_external.h"], hashlib.sha256(b"expat_external.h").hexdigest())

    def test_system_expat_requires_explicit_dependencies(self):
        with self.assertRaisesRegex(ValueError, "requires --deps"):
            build_python.target_dependencies(None, True)

    def test_missing_expat_archive_or_either_public_header_is_rejected(self):
        for missing in ("lib/libexpat.a", "include/expat.h", "include/expat_external.h"):
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as directory:
                deps = self.dependencies(Path(directory))
                (deps / missing).unlink()
                with self.assertRaisesRegex(ValueError, "required: expat"):
                    build_python.target_dependencies(deps, True)

    def test_configure_switch_is_passed_only_for_system_expat(self):
        class Configured(Exception):
            pass

        for system_expat in (False, True):
            with self.subTest(system_expat=system_expat), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                deps = self.dependencies(root / "deps")
                with patch.object(build_python, "source", return_value=root / "source"), \
                        patch.object(build_python, "build_host", return_value=root / "host/python.exe"), \
                        patch.object(build_python.subprocess, "check_output", return_value="Python " + build_python.LOCK["python"]["version"]), \
                        patch.object(build_python, "run", side_effect=Configured) as commands:
                    with self.assertRaises(Configured):
                        build_python.build(root, "iphonesimulator", 1, deps, "x86_64", system_expat)
                command, target, env = commands.call_args.args
                self.assertEqual("--with-system-expat" in command, system_expat)
                self.assertEqual("LIBEXPAT_LDFLAGS" in env, system_expat)

    def test_configuration_claim_rejects_switching_either_expat_provider(self):
        for initial in (False, True):
            with self.subTest(initial=initial), tempfile.TemporaryDirectory() as directory:
                target = Path(directory)
                build_python.claim_configuration(target, initial, {}, {})
                build_python.claim_configuration(target, initial, {}, {})
                with self.assertRaisesRegex(ValueError, "configuration differs"):
                    build_python.claim_configuration(target, not initial, {}, {})

    def test_configuration_claim_rejects_changed_dependency_archive_or_header(self):
        for field in ("archive", "header"):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as directory:
                target = Path(directory)
                build_python.claim_configuration(target, True, {"libexpat.a": "original"}, {"expat.h": "original"})
                with self.assertRaisesRegex(ValueError, "configuration differs"):
                    build_python.claim_configuration(target, True,
                        {"libexpat.a": "changed" if field == "archive" else "original"},
                        {"expat.h": "changed" if field == "header" else "original"})

    def test_accepted_legacy_default_can_be_adopted_but_not_reconfigured(self):
        for system_expat in (False, True):
            with self.subTest(system_expat=system_expat), tempfile.TemporaryDirectory() as directory:
                target = Path(directory)
                (target / "python-build.json").write_text(json.dumps({"dependency_archives": {"libffi.a": "same"}}))
                if system_expat:
                    with self.assertRaisesRegex(ValueError, "cached report"):
                        build_python.claim_configuration(target, True, {"libffi.a": "same"}, {})
                else:
                    build_python.claim_configuration(target, False, {"libffi.a": "same"}, {})
                    self.assertTrue((target / "corvcad-python-config.json").is_file())

    def test_partial_unidentified_legacy_cache_is_not_reused(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory)
            (target / "pyconfig.h").write_text("old partial configuration")
            with self.assertRaisesRegex(ValueError, "Unidentified CPython configuration"):
                build_python.claim_configuration(target, False, {}, {})


class SharingBoundary(unittest.TestCase):
    def tree(self, root):
        triplet = "arm64-ios"
        prefix = root / triplet
        (prefix / "lib").mkdir(parents=True)
        (prefix / "lib/libcommon.a").write_bytes(b"archive fixture")
        (prefix / "share/common").mkdir(parents=True)
        (prefix / "share/common/copyright").write_text("MIT notice fixture")
        (root / "vcpkg").mkdir()
        (root / "vcpkg/status").write_text("Package: common\nVersion: 1.0\nArchitecture: arm64-ios\nAbi: example-abi\n\n")
        return prefix

    def test_records_versions_abis_and_content_hashes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.tree(root)
            result = dependency_inventory.capture(root, "arm64-ios")
            self.assertEqual(result["packages"][0]["Abi"], "example-abi")
            self.assertEqual(result["archives"]["lib/libcommon.a"]["sha256"], hashlib.sha256(b"archive fixture").hexdigest())

    def test_feature_records_use_their_core_version_and_abi(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.tree(root)
            with (root / "vcpkg/status").open("a") as status:
                status.write("Package: common\nFeature: optional\nArchitecture: arm64-ios\n\n")
            result = dependency_inventory.capture(root, "arm64-ios")
            self.assertEqual(len(result["packages"]), 2)

    def test_feature_without_core_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.tree(root)
            with (root / "vcpkg/status").open("a") as status:
                status.write("Package: missing\nFeature: optional\nArchitecture: arm64-ios\n\n")
            with self.assertRaisesRegex(ValueError, "no installed core"):
                dependency_inventory.capture(root, "arm64-ios")

    def test_gui_archive_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix = self.tree(root)
            (prefix / "lib/libQt6Gui.a").write_bytes(b"gui")
            with self.assertRaisesRegex(ValueError, "GUI dependency"):
                dependency_inventory.capture(root, "arm64-ios")


    def test_dynamic_library_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix = self.tree(root)
            (prefix / "lib/libcommon.dylib").write_bytes(b"dynamic")
            with self.assertRaisesRegex(ValueError, "Dynamic library"):
                dependency_inventory.capture(root, "arm64-ios")

    def test_missing_abi_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.tree(root)
            status = root / "vcpkg/status"
            status.write_text(status.read_text().replace("Abi: example-abi\n", ""))
            with self.assertRaisesRegex(ValueError, "ABI hashes"):
                dependency_inventory.capture(root, "arm64-ios")

    def test_framework_binary_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix = self.tree(root)
            framework = prefix / "lib/Unexpected.framework"
            framework.mkdir()
            (framework / "Unexpected").write_bytes(b"binary")
            with self.assertRaisesRegex(ValueError, "Unexpected framework"):
                dependency_inventory.capture(root, "arm64-ios")

    def test_archive_inspection_rejects_wrong_sdk(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.tree(root)
            report = dependency_inventory.capture(root, "arm64-ios")
            commands = "cmd LC_BUILD_VERSION\ncmdsize 24\nplatform 7\nminos 17.0\n"
            with patch.object(dependency_inventory.subprocess, "check_output", side_effect=["arm64", commands]):
                with self.assertRaisesRegex(ValueError, "unexpected SDK"):
                    dependency_inventory.inspect_target(root, "arm64-ios", report)

    def test_archive_inspection_records_actual_platform(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.tree(root)
            report = dependency_inventory.capture(root, "arm64-ios")
            commands = "cmd LC_BUILD_VERSION\ncmdsize 24\nplatform 2\nminos 17.0\n"
            with patch.object(dependency_inventory.subprocess, "check_output", side_effect=["arm64", commands]):
                target = dependency_inventory.inspect_target(root, "arm64-ios", report)
            self.assertEqual(target["archives"]["lib/libcommon.a"]["verified_build_versions"], 1)

    def test_intel_simulator_archives_require_x86_64_and_simulator_platform(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            triplet = "x64-ios-simulator"
            prefix = root / triplet
            (prefix / "lib").mkdir(parents=True)
            (prefix / "lib/libcommon.a").write_bytes(b"archive fixture")
            (prefix / "share/common").mkdir(parents=True)
            (prefix / "share/common/copyright").write_text("MIT notice fixture")
            (root / "vcpkg").mkdir()
            (root / "vcpkg/status").write_text("Package: common\nVersion: 1.0\nArchitecture: x64-ios-simulator\nAbi: intel-abi\n\n")
            report = dependency_inventory.capture(root, triplet)
            commands = "cmd LC_BUILD_VERSION\ncmdsize 24\nplatform 7\nminos 17.0\n"
            with patch.object(dependency_inventory.subprocess, "check_output", side_effect=["x86_64", commands]):
                target = dependency_inventory.inspect_target(root, triplet, report)
            self.assertEqual(target["architecture"], "x86_64")
            with patch.object(dependency_inventory.subprocess, "check_output", return_value="arm64"):
                with self.assertRaisesRegex(ValueError, "Wrong dependency archive architecture"):
                    dependency_inventory.inspect_target(root, triplet, report)


class SimulatorSelection(unittest.TestCase):
    def test_selects_available_compatible_ipad(self):
        runtimes = [
            {"identifier": "runtime.iOS-18-5", "version": "18.5", "isAvailable": True},
            {"identifier": "runtime.iOS-26-0", "version": "26.0", "isAvailable": False},
            {"identifier": "runtime.tvOS-26-0", "version": "26.0", "isAvailable": True},
        ]
        types = [
            {"name": "iPad obsolete", "maxRuntimeVersion": simulator.version("16.0")},
            {"name": "iPad current", "minRuntimeVersion": simulator.version("18.0")},
            {"name": "iPhone current"},
        ]
        runtime, device = simulator.select(runtimes, types)
        self.assertEqual(runtime["version"], "18.5")
        self.assertEqual(device["name"], "iPad current")

    def test_unavailable_runtime_fails(self):
        with self.assertRaisesRegex(ValueError, "No compatible"):
            simulator.select([{"identifier": "runtime.iOS-18-5", "version": "18.5", "isAvailable": False}], [{"name": "iPad"}])


class BinaryBoundary(unittest.TestCase):
    def test_device_binary_does_not_count_as_simulator(self):
        with self.assertRaisesRegex(ValueError, "platform"):
            binary_report.check("platform IOS\nminos 17.0\n", "probe:\n /usr/lib/libSystem.B.dylib (compatibility version 1)\n", "iphonesimulator")

    def test_non_system_dynamic_library_rejected(self):
        with self.assertRaisesRegex(ValueError, "Non-system"):
            binary_report.check("platform IOSSIMULATOR\nminos 17.0\n", "probe:\n @rpath/FreeCAD.dylib (compatibility version 1)\n", "iphonesimulator")

    def test_system_library_allowed(self):
        result = binary_report.check("platform IOSSIMULATOR\nminos 17.0\n", "probe:\n /usr/lib/libSystem.B.dylib (compatibility version 1)\n", "iphonesimulator")
        self.assertEqual(result, ["/usr/lib/libSystem.B.dylib"])


if __name__ == "__main__":
    unittest.main()
